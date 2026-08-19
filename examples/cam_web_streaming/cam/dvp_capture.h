/*
 * DVP frame capture: PIO samples the bus, DMA lands it in a buffer, and the
 * JPEG stream is walked to find where the picture actually ends.
 *
 * The sensor is configured for JPEG output, so what arrives over the parallel
 * bus is already a compressed frame - there is no encoder on this chip and no
 * room for an uncompressed one. That also means the length of a frame is not
 * known in advance and has to be discovered from the data.
 */
#ifndef _CAM_DVP_CAPTURE_H_
#define _CAM_DVP_CAPTURE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** Why the last capture failed, for the status endpoint and the console. */
typedef enum {
    DVP_OK = 0,
    DVP_ERR_NO_SOI      = -1,   /* frame did not start with FF D8            */
    DVP_ERR_VSYNC       = -2,   /* sensor stopped framing                    */
    DVP_ERR_DMA         = -3,   /* transfer stalled mid-frame                */
    DVP_ERR_NO_EOI      = -4,   /* ran out of buffer before FF D9            */
    DVP_ERR_CONFIG      = -5    /* PIO could not be set up for these pins    */
} dvp_result_t;

/** Timing and shape of the last capture. Cleared at the start of each one. */
typedef struct {
    uint32_t vsync_wait_us;     /* idle, waiting for the sensor's next frame */
    uint32_t readout_us;        /* pulling the frame off the bus             */
    uint32_t line_count;        /* DMA chunks consumed                       */
    uint32_t parse_pos;         /* where the JPEG walker stopped             */
    bool     in_scan;           /* had it reached entropy-coded data         */
} dvp_stats_t;

extern dvp_stats_t dvp_stats;

/**
 * Claim a PIO state machine and a DMA channel, and route the bus pins.
 *
 * @return DVP_OK, or DVP_ERR_CONFIG if no state machine was free or the pins
 *         cannot be reached from one PIO's 32-pin window.
 */
int dvp_init(void);

/**
 * Set the DMA chunk size, in bytes.
 *
 * This is not a line of the image - in JPEG mode there are no pixel lines on
 * the bus. It is the granularity at which the capture wakes up to re-arm the
 * DMA and advance the JPEG parser, and it is sized from the frame width only
 * because that keeps the wake-up rate roughly constant across resolutions.
 *
 * Must be a multiple of 4: the DMA moves 32-bit words.
 */
void dvp_set_chunk_bytes(uint32_t bytes);

/**
 * Capture one JPEG frame.
 *
 * Blocks from the start of the next frame until the end-of-image marker is
 * found. On success *out_len holds the exact JPEG length, with no padding -
 * the sensor keeps driving the bus after the picture ends, and everything past
 * FF D9 is discarded.
 *
 * @param dst      destination, 4-byte aligned
 * @param dst_size bytes available at dst
 * @param out_len  set to the JPEG length on success
 * @return DVP_OK or one of the dvp_result_t failures
 */
int dvp_capture(uint8_t *dst, size_t dst_size, uint32_t *out_len);

#endif /* _CAM_DVP_CAPTURE_H_ */
