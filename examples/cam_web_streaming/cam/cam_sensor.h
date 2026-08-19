/*
 * The OV3660 as this example uses it.
 *
 * A thin layer over the vendor driver in ov3660.c, which is carried here
 * unmodified from esp32-camera. Everything the driver expects from ESP-IDF is
 * supplied by the small headers in esp_shim/ and by sccb.c / xclk.c, so the
 * driver itself can be re-synced from upstream by copying the file over.
 *
 * The sensor is run in JPEG mode. There is no image codec on RP2350 and no
 * memory for an uncompressed 3 MP frame, so the compression has to happen in
 * the sensor - which is the reason this board pairs a JPEG-capable sensor with
 * a part that has 520 KB of SRAM.
 */
#ifndef _CAM_SENSOR_H_
#define _CAM_SENSOR_H_

#include <stdint.h>
#include "sensor.h"

/**
 * Start XCLK, bring up SCCB, identify the sensor and configure it for JPEG at
 * the given frame size.
 *
 * @param framesize initial frame size
 * @return 0 on success, negative on failure (no sensor answering, wrong PID,
 *         or a register write that did not take)
 */
int cam_sensor_init(framesize_t framesize);

/**
 * Change frame size.
 *
 * The sensor needs a moment after this before its output is stable; the first
 * frame or two afterwards may arrive without a SOI marker, which the capture
 * side already retries through.
 *
 * @return 0 on success, negative on failure
 */
int cam_sensor_set_framesize(framesize_t framesize);

/**
 * JPEG quality, 4 (best) to 63 (worst) in the sensor's own scale.
 * Lower numbers mean bigger frames, which cost both capture time and bandwidth.
 */
int cam_sensor_set_quality(int quality);

/** Frame size currently programmed. */
framesize_t cam_sensor_get_framesize(void);

/** Pixel width of the current frame size, for sizing DMA chunks and for the UI. */
uint16_t cam_sensor_get_width(void);

/** Pixel height of the current frame size. */
uint16_t cam_sensor_get_height(void);

#endif /* _CAM_SENSOR_H_ */
