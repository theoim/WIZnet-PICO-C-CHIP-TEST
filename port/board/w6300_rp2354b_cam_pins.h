/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Everything on the RP2354-B + W6300 camera board that is not the W6300.
 *
 * The Ethernet pins live in port/ioLibrary_Driver/inc/wizchip_spi.h, behind the
 * same DEVICE_BOARD_NAME switch every other board in this repo uses. This file
 * covers the camera, the microSD socket and the PDM microphone.
 *
 * ---- Reading the schematic ------------------------------------------------
 *
 * The net names on the sheets are wrong, or rather they are right for a
 * different chip. Labels like MTDO/IO40/CAM_SDA, IO10/XMCLK and IO48/DVP_Y9 are
 * ESP32-S3 pin names carried over from the ESP32 version of this board -- MTDO
 * is GPIO40 on an S3, and an S3 has an IO48. None of those numbers mean
 * anything on RP2350. Only the suffix after the last slash (CAM_SDA, XMCLK,
 * DVP_Y9) identifies the signal; the GPIO number comes from the RP2354-B sheet.
 *
 * That mismatch is worth knowing before debugging: a reader who trusts the
 * numbers will look for GPIO40 and find the camera clock somewhere else.
 */
#ifndef _W6300_RP2354B_CAM_PINS_H_
#define _W6300_RP2354B_CAM_PINS_H_

/*
 * ---- Camera, J5 (Molex 505110-2491, 24-way FFC) ---------------------------
 *
 * 8-bit DVP. The sensor's Y0/Y1 are not brought out, so what the connector
 * calls Y2..Y9 is the top byte of its 10-bit bus -- the ordinary 8-bit wiring
 * for an OV sensor, not a missing pair.
 *
 * The data lines are consecutive and in order, GPIO24..GPIO31 = Y2..Y9, which
 * is what a PIO capture program wants: one `in pins, 8` from a single base.
 */
#define CAM_PIN_XCLK        35      /* FFC 13 */
#define CAM_PIN_PCLK        32      /* FFC 17 */
#define CAM_PIN_VSYNC       34      /* FFC  7 */
#define CAM_PIN_HREF        33      /* FFC  9 */
#define CAM_PIN_SIOD        36      /* FFC  3 -- I2C0 SDA */
#define CAM_PIN_SIOC        37      /* FFC  5 -- I2C0 SCL */

#define CAM_PIN_D0          24      /* FFC 19, DVP_Y2 */
#define CAM_PIN_D1          25      /* FFC 21, DVP_Y3 */
#define CAM_PIN_D2          26      /* FFC 22, DVP_Y4 */
#define CAM_PIN_D3          27      /* FFC 20, DVP_Y5 */
#define CAM_PIN_D4          28      /* FFC 18, DVP_Y6 */
#define CAM_PIN_D5          29      /* FFC 16, DVP_Y7 */
#define CAM_PIN_D6          30      /* FFC 14, DVP_Y8 */
#define CAM_PIN_D7          31      /* FFC 12, DVP_Y9 */

#define CAM_PIN_D_BASE      CAM_PIN_D0
#define CAM_PIN_D_COUNT     8

/*
 * No GPIO reaches the sensor's reset or power-down.
 *
 * FFC pin 6 is held by an RC to 3V3 (R18 10K with C33 4.7uF / C34 0.1uF), so
 * reset releases itself once the rail is stable; pin 8 sits behind R20 10K.
 * Both are hardware-only.
 *
 * Consequence for the driver: a wedged sensor cannot be recovered in software.
 * The only reset available is a board reset. Any port of a driver that expects
 * to toggle these -- most OV drivers do -- has to be given -1 and must not
 * treat that as a fatal configuration error.
 */
#define CAM_PIN_RESET       (-1)
#define CAM_PIN_PWDN        (-1)

/* The camera's own supplies come from the board: 1V8 on FFC 10, 2V8 on FFC 11
 * and 24, AVCC_2V8 on FFC 4. Nothing to enable in firmware. */

/*
 * ---- microSD, J7 (MSD-4-A) ------------------------------------------------
 *
 * The one peripheral here that lands exactly on a hardware SPI block:
 * GPIO8/9/10/11 are SPI1 RX/CSn/SCK/TX. No PIO needed.
 *
 * DAT1/DAT2 are not connected and CD/DAT3 is pulled up (R24 5.1k), which is the
 * usual SPI-mode wiring -- this socket is for SPI mode, not 4-bit SDIO.
 */
#define SD_PIN_MISO          8      /* DAT0 */
#define SD_PIN_CS            9
#define SD_PIN_SCK          10
#define SD_PIN_MOSI         11      /* CMD  */
#define SD_SPI_PORT         spi1

/*
 * ---- PDM microphone -------------------------------------------------------
 */
#define PDM_PIN_CLK         38
#define PDM_PIN_DATA        39

/*
 * ---- Free ------------------------------------------------------------------
 *
 * GPIO0..7, GPIO12..15, and GPIO40..47 (ADC0..ADC7) are unassigned and brought
 * out to the headers. GPIO0/1 are the default UART in the board header.
 */

#endif /* _W6300_RP2354B_CAM_PINS_H_ */
