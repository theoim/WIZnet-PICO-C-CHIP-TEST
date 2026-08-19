#include <string.h>

#include "dvp_capture.h"
#include "dvp.pio.h"
#include "esp_shim/esp_log.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#include "w6300_rp2354b_cam_pins.h"

static const char *TAG = "dvp";

/*
 * pio2, not pio0 or pio1.
 *
 * The W6300 driver claims a state machine from pio0, falling back to pio1
 * (port/ioLibrary_Driver/src/wizchip_qspi_pio.c). Taking the third block keeps
 * the camera off whichever one the Ethernet side settled on, and it also lets
 * the two run different GPIO bases - which they must, see below.
 */
#define DVP_PIO             pio2

/*
 * The camera bus needs a GPIO base of 16.
 *
 * An RP2350B PIO block reaches 32 pins at a time: either 0-31 or 16-47. PCLK
 * and HREF are GPIO32 and GPIO33, so the low window cannot see them and the
 * high window is the only choice. The W6300 pins (16-23) fit in either, so it
 * keeps the default base on its own block and nothing has to be moved.
 */
#define DVP_PIO_GPIO_BASE   16

#define VSYNC_TIMEOUT_US    2000000     /* sensor is not framing at all      */
#define DMA_TIMEOUT_US      500000      /* one chunk stalled                 */
#define MAX_SOI_RETRIES     3           /* frames to discard chasing a SOI   */

dvp_stats_t dvp_stats;

static uint     s_sm;
static uint     s_offset;
static int      s_dma   = -1;
static uint32_t s_chunk = 320 * 2;

int dvp_init(void)
{
    /*
     * Before anything is loaded onto this block: pio_set_gpio_base refuses to
     * move the window once instruction memory is in use, because the programs
     * already there were placed against the old one.
     *
     * It returns an error code, not a flag - PICO_OK is zero, so a truth test
     * on the return value reads success as failure.
     */
    int rc = pio_set_gpio_base(DVP_PIO, DVP_PIO_GPIO_BASE);
    if (rc != PICO_OK) {
        ESP_LOGE(TAG, "PIO GPIO base %d rejected (%d)", DVP_PIO_GPIO_BASE, rc);
        return DVP_ERR_CONFIG;
    }

    int sm = pio_claim_unused_sm(DVP_PIO, false);
    if (sm < 0) {
        ESP_LOGE(TAG, "no free state machine");
        return DVP_ERR_CONFIG;
    }
    s_sm = (uint)sm;

    int offset = pio_add_program(DVP_PIO, &dvp_program);
    if (offset < 0) {
        ESP_LOGE(TAG, "program would not load (%d)", offset);
        return DVP_ERR_CONFIG;
    }
    s_offset = (uint)offset;

    /*
     * Hand the ten bus pins to the PIO. gpio_set_function takes the real GPIO
     * number and is unaffected by the window; only the state machine
     * configuration is relative to it.
     */
    for (uint i = 0; i < 10; i++) {
        gpio_set_function(CAM_PIN_D_BASE + i, GPIO_FUNC_PIO2);
    }

    rc = dvp_program_init(DVP_PIO, s_sm, s_offset, CAM_PIN_D_BASE);
    if (rc != PICO_OK) {
        ESP_LOGE(TAG, "state machine config rejected for pins %d..%d (%d)",
                 CAM_PIN_D_BASE, CAM_PIN_D_BASE + 9, rc);
        return DVP_ERR_CONFIG;
    }
    pio_sm_set_enabled(DVP_PIO, s_sm, true);

    /* VSYNC is read by the CPU, so it stays a plain input. */
    gpio_init(CAM_PIN_VSYNC);
    gpio_set_dir(CAM_PIN_VSYNC, GPIO_IN);

    s_dma = dma_claim_unused_channel(false);
    if (s_dma < 0) {
        ESP_LOGE(TAG, "no free DMA channel");
        return DVP_ERR_CONFIG;
    }

    dma_channel_config c = dma_channel_get_default_config((uint)s_dma);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(DVP_PIO, s_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

    /*
     * High priority on the bus. During an active line bytes arrive back to
     * back and the RX FIFO is eight words deep; losing arbitration for long
     * enough to overflow it punches a hole in the middle of the JPEG, which
     * shows up as a torn band across the picture.
     */
    channel_config_set_high_priority(&c, true);

    dma_channel_configure((uint)s_dma, &c,
                          NULL,                     /* write addr set per chunk */
                          &DVP_PIO->rxf[s_sm],
                          s_chunk / 4,
                          false);

    ESP_LOGW(TAG, "pio2 sm%u dma%d, D0=%d PCLK=%d HREF=%d VSYNC=%d",
             s_sm, s_dma, CAM_PIN_D_BASE, CAM_PIN_PCLK, CAM_PIN_HREF,
             CAM_PIN_VSYNC);
    return DVP_OK;
}

void dvp_set_chunk_bytes(uint32_t bytes)
{
    s_chunk = bytes & ~3u;
    if (s_chunk < 64) {
        s_chunk = 64;
    }
}

static inline void arm_chunk(uint8_t *dst)
{
    dma_channel_set_trans_count((uint)s_dma, s_chunk / 4, false);
    dma_channel_set_write_addr((uint)s_dma, dst, true);
}

int dvp_capture(uint8_t *dst, size_t dst_size, uint32_t *out_len)
{
    uint32_t        jpeg_end;
    uint32_t        parse_pos;
    bool            in_scan;
    uint32_t        chunks;
    int             retries = 0;
    absolute_time_t t0, t_readout, t_wait;

    if (s_dma < 0) {
        return DVP_ERR_CONFIG;
    }

retry:
    jpeg_end  = 0;
    parse_pos = 2;      /* SOI is checked directly; the walk starts after it */
    in_scan   = false;
    chunks    = 0;
    t0        = get_absolute_time();

    /* Wait out the frame in progress, so the next one is captured whole. */
    t_wait = get_absolute_time();
    while (gpio_get(CAM_PIN_VSYNC)) {
        if (absolute_time_diff_us(t_wait, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            ESP_LOGE(TAG, "VSYNC stuck high");
            return DVP_ERR_VSYNC;
        }
        tight_loop_contents();
    }

    /*
     * Arm while VSYNC is still low.
     *
     * The state machine is free-running, so the moment the frame starts it
     * pushes bytes whether or not a transfer is waiting. Clearing the FIFO
     * after the frame has begun throws away the first bytes, and those are the
     * SOI marker - which sends the capture straight into a retry. During
     * vertical blanking HREF is low, nothing is produced, and it is safe.
     */
    pio_sm_clear_fifos(DVP_PIO, s_sm);
    arm_chunk(dst);

    t_wait = get_absolute_time();
    while (!gpio_get(CAM_PIN_VSYNC)) {
        if (absolute_time_diff_us(t_wait, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            ESP_LOGE(TAG, "VSYNC stuck low");
            dma_channel_abort((uint)s_dma);
            return DVP_ERR_VSYNC;
        }
        tight_loop_contents();
    }

    t_readout = get_absolute_time();
    dvp_stats.vsync_wait_us = (uint32_t)absolute_time_diff_us(t0, t_readout);

    /*
     * Read the frame chunk by chunk, straight into dst.
     *
     * The FIFO is never cleared inside this loop. Anything the state machine
     * pushed between transfers is real image data; dropping it would leave a
     * silent gap that the SOI check cannot catch.
     */
    do {
        t_wait = get_absolute_time();
        while (dma_channel_is_busy((uint)s_dma)) {
            if (absolute_time_diff_us(t_wait, get_absolute_time()) > DMA_TIMEOUT_US) {
                ESP_LOGE(TAG, "DMA stalled on chunk %lu", (unsigned long)chunks);
                dma_channel_abort((uint)s_dma);
                return DVP_ERR_DMA;
            }
            tight_loop_contents();
        }

        uint32_t chunk_off = chunks * s_chunk;
        uint32_t scan_end  = chunk_off + s_chunk;
        bool     room      = (scan_end + s_chunk) <= dst_size;

        /*
         * Re-arm before parsing. The sensor keeps driving while the walk below
         * runs and only 32 bytes fit in the FIFO, so this cannot wait.
         */
        if (room) {
            arm_chunk(dst + scan_end);
        }
        chunks++;

        if (chunk_off == 0 && (dst[0] != 0xFF || dst[1] != 0xD8)) {
            /*
             * No SOI. Usually the sensor is still settling after a mode change
             * and the next frame is fine, so discard and try again a couple of
             * times before reporting it.
             */
            dma_channel_abort((uint)s_dma);
            if (++retries < MAX_SOI_RETRIES) {
                goto retry;
            }
            ESP_LOGE(TAG, "no SOI after %d frames (got %02x %02x)",
                     retries, dst[0], dst[1]);
            return DVP_ERR_NO_SOI;
        }

        /*
         * Walk the JPEG to find where the picture really ends.
         *
         * The sensor drives the bus for the whole active frame period, long
         * after the image is complete, so without this the capture reads
         * padding until VSYNC drops - which at the larger frame sizes overruns
         * the buffer and costs the frame.
         *
         * Hunting blindly for FF D9 is not safe: byte stuffing applies only to
         * entropy-coded data, so a quantisation or Huffman payload may contain
         * that pair and would truncate the image mid-header. Walk the marker
         * segments by their declared length until SOS, and only look for the
         * end marker after it.
         *
         * The walk is incremental - parse_pos survives across chunks and every
         * byte is examined once. Inside the entropy data it uses memchr rather
         * than a byte loop, because this runs between transfers and the FIFO
         * only buys 32 bytes of slack.
         */
        while (!jpeg_end) {
            if (!in_scan) {
                uint8_t  marker;
                uint32_t seg_len;

                if (parse_pos + 4 > scan_end) {
                    break;                          /* need more data */
                }
                if (dst[parse_pos] != 0xFF) {
                    parse_pos++;                    /* resynchronise */
                    continue;
                }
                marker = dst[parse_pos + 1];

                /* A run of FF bytes is legal padding ahead of a marker. */
                if (marker == 0xFF) {
                    parse_pos++;
                    continue;
                }

                /* Standalone markers carry no length field. */
                if (marker == 0xD8 || marker == 0x01 ||
                    (marker >= 0xD0 && marker <= 0xD7)) {
                    parse_pos += 2;
                    continue;
                }

                seg_len = ((uint32_t)dst[parse_pos + 2] << 8) |
                           (uint32_t)dst[parse_pos + 3];
                if (seg_len < 2) {
                    break;                          /* malformed - give up */
                }
                parse_pos += 2 + seg_len;
                if (marker == 0xDA) {
                    in_scan = true;                 /* entropy data follows */
                }
            } else {
                uint8_t *ff;

                if (parse_pos + 1 >= scan_end) {
                    break;                          /* need more data */
                }

                ff = memchr(dst + parse_pos, 0xFF, scan_end - parse_pos - 1);
                if (ff == NULL) {
                    parse_pos = scan_end - 1;       /* keep the trailing byte */
                    break;
                }

                parse_pos = (uint32_t)(ff - dst);
                if (dst[parse_pos + 1] == 0xD9) {
                    jpeg_end = parse_pos + 2;
                    break;
                }
                parse_pos += 2;                     /* FF 00 stuffing or RSTn */
            }
        }

        if (jpeg_end) {
            break;                  /* complete - stop reading padding */
        }
        if (!room) {
            ESP_LOGE(TAG, "buffer full at %lu bytes, no EOI",
                     (unsigned long)scan_end);
            dma_channel_abort((uint)s_dma);
            return DVP_ERR_NO_EOI;
        }

    } while (gpio_get(CAM_PIN_VSYNC));

    /* The last chunk was armed for data that will not arrive. */
    dma_channel_abort((uint)s_dma);

    dvp_stats.readout_us = (uint32_t)absolute_time_diff_us(t_readout,
                                                           get_absolute_time());
    dvp_stats.line_count = chunks;
    dvp_stats.parse_pos  = parse_pos;
    dvp_stats.in_scan    = in_scan;

    /*
     * Without an EOI the image is incomplete: the tail was lost, or VSYNC
     * dropped early. Report it rather than hand a truncated JPEG to the caller
     * - a decoder that rejects it makes the view blink, which is worse than
     * skipping the frame.
     */
    if (!jpeg_end) {
        return DVP_ERR_NO_EOI;
    }

    *out_len = jpeg_end;
    return DVP_OK;
}
