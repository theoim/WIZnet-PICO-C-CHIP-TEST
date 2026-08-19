/*
 * SCCB (the sensor's I2C) for RP2350.
 *
 * The signatures match esp32-camera's driver/private_include/sccb.h exactly so
 * that ov3660.c compiles against this file without a single edit. Only the two
 * 16-bit-address entry points are implemented, because that is all the OV3660
 * driver calls; the rest of the vendor API is deliberately absent so a future
 * sensor that needs 8-bit addressing fails to link rather than silently reading
 * the wrong register.
 *
 * Note the return type of SCCB_Read16: uint8_t, inherited from the vendor
 * header. It cannot report a failure, and ov3660.c's `if (ret < 0)` checks
 * against it are therefore dead in both the original and here. A bus error
 * surfaces as 0xFF, which the PID check at startup catches.
 */
#ifndef _CAM_SCCB_H_
#define _CAM_SCCB_H_

#include <stdint.h>
#include "hardware/i2c.h"

/**
 * Bring up the I2C block on the SIOD/SIOC pins and remember it for the
 * SCCB_* calls below.
 *
 * @param i2c   I2C instance wired to the sensor
 * @param sda   SIOD pin
 * @param scl   SIOC pin
 * @param baud  bus rate in Hz
 */
void cam_sccb_init(i2c_inst_t *i2c, uint sda, uint scl, uint baud);

uint8_t SCCB_Read16(uint8_t slv_addr, uint16_t reg);
int     SCCB_Write16(uint8_t slv_addr, uint16_t reg, uint8_t data);

#endif /* _CAM_SCCB_H_ */
