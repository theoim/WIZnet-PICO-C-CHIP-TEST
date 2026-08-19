/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Board header for the RP2354-B + W6300 camera board.
 *
 * Lives in the project rather than in libraries/pico-sdk/src/boards, because the
 * SDK is a submodule and anything written inside it is lost on the next update.
 * The top-level CMakeLists adds this directory to PICO_BOARD_HEADER_DIRS, which
 * is the SDK's supported way of supplying an out-of-tree board.
 *
 * Based on boards/pico2.h. Three things differ, and the first one is the reason
 * this file has to exist at all:
 *
 *   PICO_RP2350A 0        This is the B package: 48 GPIOs, not 30.
 *   PICO_FLASH_SIZE_BYTES RP2354 carries 2 MB of stacked flash, not 4 MB.
 *   default SPI / I2C     Point at what is actually wired here.
 *
 * ---- Why PICO_RP2350A matters more than it looks -------------------------
 *
 * PICO_RP2350A is not documentation. NUM_BANK0_GPIOS comes from it, and every
 * GPIO bound in the SDK is checked against that number. Leave it at 1 (the
 * pico2.h default) and the SDK believes this chip has 30 GPIOs, so anything on
 * GPIO30..47 -- which here is the entire camera bus, both SCCB lines and the
 * PDM microphone -- fails a bounds check or silently writes to a pad register
 * that does not exist.
 *
 * That is the shape of "the I2C on the 40s did not work" on the previous
 * attempt: the pins were wired correctly and the code was correct, and the SDK
 * had been told the pins were not there.
 */
#ifndef _BOARDS_W6300_RP2354B_CAM_H
#define _BOARDS_W6300_RP2354B_CAM_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

/* For board detection in application code. */
#define W6300_RP2354B_CAM

/* --- RP2350 VARIANT ---
 * 0 selects the B package (48 GPIO / QFN-80). Everything above GPIO29 on this
 * board depends on it. */
#define PICO_RP2350A 0

/* --- UART ---
 * GPIO0/1 are free on this design and are the conventional pair. */
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

/* --- LED ---
 * Deliberately not defined. No LED is identifiable on the schematic sheets seen
 * so far, and a wrong PICO_DEFAULT_LED_PIN is worse than none: pico_examples
 * blink builds and does nothing, which reads as a dead board. Add it here once
 * the net is confirmed. */

/* --- I2C ---
 * The camera's SCCB lines. GPIO36 is a valid I2C0 SDA and GPIO37 a valid I2C0
 * SCL on RP2350B (SDA on 4n, SCL on 4n+1), so this is the hardware block and
 * not bit-banging -- the designer picked these two on purpose. */
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 36
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 37
#endif

/* --- SPI ---
 * The microSD socket, and the one peripheral here that lands exactly on a
 * hardware SPI block: GPIO8/9/10/11 are SPI1 RX/CSn/SCK/TX.
 *
 * The W6300 is NOT on a hardware SPI block -- see wizchip_spi.h -- so it is not
 * described here. */
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 10
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 11
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 8
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 9
#endif

/* --- FLASH ---
 * RP2354 is an RP2350 with 2 MB of stacked flash in the same package. Nothing
 * else about the flash path changes, so the stage2 choice stays as pico2.h has
 * it; only the size is different, and getting it wrong costs you the top half
 * of the address space silently. */
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif /* _BOARDS_W6300_RP2354B_CAM_H */
