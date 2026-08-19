#include "xclk.h"
#include "esp_shim/esp_log.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"

static const char *TAG = "xclk";

static uint s_pin  = 0;
static bool s_live = false;

int cam_xclk_start(uint pin, uint32_t freq_hz)
{
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t period;
    uint     slice, chan;

    if (freq_hz == 0 || sys_hz % freq_hz != 0) {
        ESP_LOGE(TAG, "%lu Hz is not an integer division of %lu Hz sys clock",
                 (unsigned long)freq_hz, (unsigned long)sys_hz);
        return -1;
    }

    period = sys_hz / freq_hz;

    /*
     * An odd period cannot be split into equal halves, and the sensor is
     * specified for a roughly symmetric clock. Refusing here is better than
     * emitting a lopsided one that mostly works and drifts under temperature.
     */
    if (period < 2 || (period & 1u)) {
        ESP_LOGE(TAG, "period %lu is unusable (needs to be even and >= 2)",
                 (unsigned long)period);
        return -1;
    }

    s_pin = pin;
    slice = pwm_gpio_to_slice_num(pin);
    chan  = pwm_gpio_to_channel(pin);

    gpio_set_function(pin, GPIO_FUNC_PWM);

    /*
     * clkdiv stays at 1. The whole point of choosing a frequency that divides
     * the system clock evenly is to keep the fractional divider - and its
     * cycle-skipping jitter - out of the sensor's PLL input.
     */
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&cfg, 1);
    pwm_config_set_wrap(&cfg, (uint16_t)(period - 1));
    pwm_init(slice, &cfg, false);

    pwm_set_chan_level(slice, chan, (uint16_t)(period / 2));
    pwm_set_enabled(slice, true);

    /*
     * Reported because a sensor with no master clock does not acknowledge its
     * own SCCB address, and that failure is indistinguishable from an absent
     * module unless this line is on the console next to it.
     */
    ESP_LOGW(TAG, "GPIO%u = %lu Hz (slice %u ch %c, wrap %lu)",
             pin, (unsigned long)freq_hz, slice, chan ? 'B' : 'A',
             (unsigned long)(period - 1));

    s_live = true;
    return 0;
}

int xclk_timer_conf(int ledc_timer, int xclk_freq_hz)
{
    (void)ledc_timer;

    if (!s_live) {
        ESP_LOGE(TAG, "retune before start");
        return -1;
    }
    return cam_xclk_start(s_pin, (uint32_t)xclk_freq_hz);
}
