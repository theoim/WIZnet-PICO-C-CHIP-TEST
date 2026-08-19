/*
 * Sensor master clock (XCLK) for RP2350.
 *
 * The name and the xclk_timer_conf signature come from esp32-camera's
 * driver/private_include/xclk.h, so ov3660.c's set_xclk() links without an
 * edit. On ESP32 that call retunes an LEDC timer; here it retunes a PWM slice.
 *
 * ---- Why 25 MHz and not the usual 20 ---------------------------------------
 *
 * esp32-camera drives OV3660 at 20 MHz. RP2350 cannot make 20 MHz cleanly from
 * the 150 MHz system clock: 150/20 is 7.5, and PWM would have to use its
 * fractional divider, which does not divide the clock so much as skip cycles.
 * The resulting jitter lands on the input of the sensor's PLL.
 *
 * 150/6 = 25 MHz exactly, with a 50% duty cycle and no fraction, and 25 MHz is
 * inside the OV3660's 6-27 MHz input range. Everything downstream scales with
 * it: the PLL settings ov3660.c picks for JPEG describe 10 MHz PCLK at a 20 MHz
 * XCLK, so at 25 MHz the same registers give 12.5 MHz - still far below what
 * the PIO capture can absorb.
 *
 * The other way out is a 160 MHz system clock, where 160/8 is exactly 20 MHz.
 * That was not taken because it also moves the W6300's PIO SPI off the divider
 * it was brought up on, and the sensor clock is the cheaper thing to move.
 */
#ifndef _CAM_XCLK_H_
#define _CAM_XCLK_H_

#include <stdint.h>
#include "pico/stdlib.h"

/**
 * Start XCLK on a pin.
 *
 * @param pin      GPIO to drive
 * @param freq_hz  requested frequency
 * @return 0 on success, -1 if the system clock cannot produce freq_hz as an
 *         even integer division (nothing is driven in that case)
 */
int cam_xclk_start(uint pin, uint32_t freq_hz);

/**
 * esp32-camera compatibility entry point, called by ov3660.c's set_xclk().
 * The timer argument is ignored; the pin is the one passed to cam_xclk_start().
 */
int xclk_timer_conf(int ledc_timer, int xclk_freq_hz);

#endif /* _CAM_XCLK_H_ */
