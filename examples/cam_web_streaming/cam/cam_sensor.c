#include "cam_sensor.h"
#include "ov3660.h"
#include "sccb.h"
#include "xclk.h"
#include "dvp_capture.h"
#include "esp_shim/esp_log.h"

#include "pico/stdlib.h"
#include "w6300_rp2354b_cam_pins.h"

static const char *TAG = "cam";

/*
 * 25 MHz, because it is an exact division of the 150 MHz system clock. See the
 * comment at the top of xclk.h for why the usual 20 MHz was not used.
 */
#define CAM_XCLK_HZ         (25 * 1000 * 1000)

/*
 * 20 kHz, which is far below what SCCB can do, because of how this bus is
 * pulled up.
 *
 * There are no pull-up resistors on it. The schematic puts a 10K on RESET and
 * a 10K on PWDN at J5 and nothing on SIOD/SIOC, so the only thing holding the
 * bus high is the RP2350's internal pull-up - 50k to 80k. Against the FFC and
 * the module, call it 50 pF, that is a rise time around 2.5 us, and standard
 * mode I2C allows 1 us. At 100 kHz the lines never reach the input threshold
 * inside a bit period and nothing acknowledges, which looks exactly like an
 * absent sensor.
 *
 * Slowing the clock is what buys the time for that RC to settle. Init is a few
 * hundred register writes that happen once, so the cost is a fraction of a
 * second at boot.
 *
 * If this turns out to be the fault, the proper fix is external pull-ups -
 * 2.2k to 4.7k on SIOD and SIOC to VCC_2V8 - after which this can go back up.
 */
#define CAM_SCCB_HZ         (20 * 1000)

static sensor_t   s_sensor;
static framesize_t s_framesize = FRAMESIZE_QVGA;

/*
 * The DMA chunk is two bytes per pixel of width.
 *
 * In JPEG mode there are no pixel lines on the bus, so this is not a line - it
 * is how often the capture loop wakes up to re-arm the DMA and advance the
 * JPEG parser. Deriving it from the width keeps that wake-up rate roughly
 * constant as the frame size changes, which is what the timeouts in
 * dvp_capture.c are sized against.
 */
static void apply_chunk_for(framesize_t fs)
{
    dvp_set_chunk_bytes((uint32_t)resolution[fs].width * 2u);
}

/*
 * Say what the bus actually looks like before giving up on it.
 *
 * "No sensor at 0x3C" has several very different causes and the same symptom:
 * an unseated or reversed FFC, a missing 1V8/2V8 rail, no XCLK so the sensor
 * never clocks its own register interface, or a module that answers perfectly
 * well but is not the part this driver was built for. Reading the ID registers
 * raw and reporting who acked separates them in one boot.
 */
/*
 * Time the low-to-high transition on one bus line, in microseconds.
 *
 * Returns 0xFFFFFFFF if the line never came up, which means it is being held
 * down rather than pulled up slowly.
 */
static uint32_t line_rise_us(uint pin)
{
    uint32_t t0, dt;

    /* Take the pin off the I2C block so it can be driven directly. */
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(100);              /* long enough for the line to settle low */

    /* Release to the internal pull-up and watch it climb. */
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_pulls(pin, true, false);

    t0 = time_us_32();
    while (!gpio_get(pin)) {
        if (time_us_32() - t0 > 10000) {
            gpio_set_function(pin, GPIO_FUNC_I2C);
            return 0xFFFFFFFFu;
        }
    }
    dt = time_us_32() - t0;

    gpio_set_function(pin, GPIO_FUNC_I2C);
    return dt;
}

/*
 * Count edges on one pin over a window, without disturbing whatever function
 * owns it - gpio_get reads the pad, not the peripheral.
 */
static uint32_t count_edges(uint pin, uint32_t window_ms)
{
    uint32_t t0    = time_us_32();
    uint32_t edges = 0;
    int      last  = gpio_get(pin);

    while (time_us_32() - t0 < window_ms * 1000u) {
        int now = gpio_get(pin);
        if (now != last) {
            edges++;
            last = now;
        }
    }
    return edges;
}

/*
 * Is the sensor alive, independently of whether SCCB works?
 *
 * An OV part comes out of reset in a default mode and starts driving its
 * timing outputs as soon as it has XCLK - no register writes needed. So PCLK,
 * HREF and VSYNC toggling proves the module is fitted, powered and clocked
 * even when nothing acknowledges on the register bus. All three dead means the
 * problem is upstream of SCCB: no module, no 1V8/2V8, or no XCLK reaching it.
 *
 * Edge counts here are sampled by a polling loop, so they undercount badly at
 * PCLK rates. Only zero versus non-zero is meaningful.
 */
static void dvp_activity_report(void)
{
    /*
     * XCLK first, read back off its own pad.
     *
     * The polling loop is far slower than 25 MHz, so the count is meaningless
     * as a frequency - it aliases. What it does settle is whether the pin is
     * moving at all. A zero here means the PWM never reached the pad and the
     * sensor was never going to answer, which is a different fault from a
     * sensor that has a clock but no power.
     */
    ESP_LOGW(TAG, "XCLK pad on GPIO%d: %lu edges in 5 ms (non-zero = toggling)",
             CAM_PIN_XCLK, (unsigned long)count_edges(CAM_PIN_XCLK, 5));

    ESP_LOGW(TAG, "DVP activity over 50 ms: PCLK=%lu HREF=%lu VSYNC=%lu edges",
             (unsigned long)count_edges(CAM_PIN_PCLK,  50),
             (unsigned long)count_edges(CAM_PIN_HREF,  50),
             (unsigned long)count_edges(CAM_PIN_VSYNC, 50));
}

static void sccb_diagnose(void)
{
    uint8_t addr;
    uint8_t probe = 0;
    int     found = 0;

    /*
     * How fast does the bus actually get back up to a one?
     *
     * This is the measurement that matters here, because the board has no
     * pull-up resistors on SIOD/SIOC - only the RP2350's internal 50k-80k. Its
     * RC against the FFC and the module is what decides whether a bit can be
     * read at all, and it is the number to compare against the 1 us that
     * standard mode I2C allows.
     *
     * Drive the line low, let go, and time how long it takes to read high:
     *
     *   0-1 us    external pull-ups are fitted somewhere; the bus is healthy
     *   2 us +    internal pull-up only - too slow for 100 kHz, and the reason
     *             the clock is set to CAM_SCCB_HZ
     *   never     the line is shorted to ground, or held by something
     */
    ESP_LOGW(TAG, "SCCB rise time: SDA %lu us, SCL %lu us (1 us is the I2C limit)",
             (unsigned long)line_rise_us(CAM_PIN_SIOD),
             (unsigned long)line_rise_us(CAM_PIN_SIOC));

    for (addr = 0x08; addr < 0x78; addr++) {
        if (i2c_write_timeout_us(CAM_SCCB_I2C, addr, &probe, 1, false, 2000) >= 0) {
            ESP_LOGW(TAG, "SCCB device acked at 0x%02x", addr);
            found++;
        }
    }
    dvp_activity_report();

    if (!found) {
        ESP_LOGW(TAG, "SCCB: nothing acked on the whole bus");
        return;
    }

    /*
     * 0x300A/0x300B is where the OV parts with 16-bit registers keep their
     * chip ID, so this reads the same place whether the module is an OV3660
     * (0x3660), an OV5640 (0x5640) or an OV2640-class part that will not
     * answer here at all.
     */
    ESP_LOGW(TAG, "chip ID at 0x300A/0x300B: 0x%02x%02x",
             SCCB_Read16(OV3660_SCCB_ADDR, 0x300A),
             SCCB_Read16(OV3660_SCCB_ADDR, 0x300B));
}

int cam_sensor_init(framesize_t framesize)
{
    int ret;

    /*
     * XCLK first, and before any SCCB traffic. The sensor clocks its own
     * register interface from XCLK: with no clock it does not acknowledge its
     * address, and the PID read below would fail for a reason that looks
     * exactly like a dead sensor.
     */
    if (cam_xclk_start(CAM_PIN_XCLK, CAM_XCLK_HZ) != 0) {
        ESP_LOGE(TAG, "XCLK could not be generated");
        return -1;
    }

    /*
     * RESET and PWDN are hardware-only on this board - both sit on RC networks
     * on the FFC with no GPIO reaching them (CAM_PIN_RESET and CAM_PIN_PWDN
     * are -1). So there is no hard reset to assert here; the sensor comes out
     * of its power-on reset on its own and the driver relies on the software
     * reset in its register table instead.
     *
     * The consequence is worth stating plainly: a wedged sensor cannot be
     * recovered from firmware. The only reset available is a board reset.
     */
    /*
     * Wait out the reset RC.
     *
     * RESET is a 10K pull-up to 3V3 loaded by C33 4.7uF and C34 0.1uF, so the
     * time constant is about 48 ms - far slower than the 1 ms of the XIAO
     * reference design this board is derived from, which fits only 100 nF
     * there. Two time constants is roughly 100 ms, and the sensor does not
     * acknowledge its SCCB address while it is still held.
     *
     * 250 ms is comfortably past that and is paid once at boot.
     */
    sleep_ms(250);

    cam_sccb_init(CAM_SCCB_I2C, CAM_PIN_SIOD, CAM_PIN_SIOC, CAM_SCCB_HZ);

    s_sensor.slv_addr     = OV3660_SCCB_ADDR;
    s_sensor.xclk_freq_hz = CAM_XCLK_HZ;

    if (esp32_camera_ov3660_detect(s_sensor.slv_addr, &s_sensor.id) == 0) {
        ESP_LOGE(TAG, "no OV3660 at SCCB 0x%02x", s_sensor.slv_addr);
        sccb_diagnose();
        return -2;
    }
    ESP_LOGW(TAG, "OV3660 found, PID 0x%04x", s_sensor.id.PID);

    esp32_camera_ov3660_init(&s_sensor);

    ret = s_sensor.reset(&s_sensor);
    if (ret != 0) {
        ESP_LOGE(TAG, "reset failed (%d)", ret);
        return -3;
    }

    /*
     * Pixel format before frame size, not the other way round.
     *
     * set_framesize() picks the sensor PLL from the format that is already
     * set: JPEG asks for a 10 MHz PCLK, the raw formats for 8 MHz off a much
     * slower system clock. Setting the size first would program the PLL for
     * the format still in effect and then leave it there.
     */
    ret = s_sensor.set_pixformat(&s_sensor, PIXFORMAT_JPEG);
    if (ret != 0) {
        ESP_LOGE(TAG, "JPEG mode rejected (%d)", ret);
        return -4;
    }

    return cam_sensor_set_framesize(framesize);
}

int cam_sensor_set_framesize(framesize_t framesize)
{
    int ret;

    /* The OV3660 is a 3 MP part; QXGA is the top of its range. */
    if (framesize > FRAMESIZE_QXGA) {
        return -1;
    }

    ret = s_sensor.set_framesize(&s_sensor, framesize);
    if (ret != 0) {
        ESP_LOGE(TAG, "framesize %d rejected (%d)", framesize, ret);
        return -2;
    }

    s_framesize = framesize;
    apply_chunk_for(framesize);

    /*
     * The sensor retimes after a size change and the first frames out of it
     * are partial. Waiting here means the retries in dvp_capture() are kept
     * for real faults rather than being spent on every resolution change.
     */
    sleep_ms(100);
    return 0;
}

int cam_sensor_set_quality(int quality)
{
    if (quality < 4 || quality > 63) {
        return -1;
    }
    return s_sensor.set_quality(&s_sensor, quality) == 0 ? 0 : -2;
}

framesize_t cam_sensor_get_framesize(void)
{
    return s_framesize;
}

uint16_t cam_sensor_get_width(void)
{
    return resolution[s_framesize].width;
}

uint16_t cam_sensor_get_height(void)
{
    return resolution[s_framesize].height;
}
