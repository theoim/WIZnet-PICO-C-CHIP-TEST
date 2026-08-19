#include "sccb.h"
#include "hardware/gpio.h"

/*
 * A single bus, fixed at init. The sensor is the only device on it, so there is
 * nothing to arbitrate and no reason to carry the instance through every call -
 * which is also what lets the signatures match the vendor header.
 */
static i2c_inst_t *s_i2c = NULL;

/*
 * Enough for a whole register write in one transfer: address high, address low,
 * value. Sending the address and the value as two transfers would put a STOP
 * between them and the sensor would treat the value as a new address.
 */
#define SCCB_XFER_TIMEOUT_US    2000

void cam_sccb_init(i2c_inst_t *i2c, uint sda, uint scl, uint baud)
{
    s_i2c = i2c;

    i2c_init(i2c, baud);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);

    /*
     * The board has no SCCB pull-ups on the sheet - the camera module carries
     * them across the FFC. The internal pulls are weak (~50k) and are here as a
     * fallback that keeps the bus idle-high if a module without them is fitted;
     * they are not strong enough for a fast bus on their own.
     */
    gpio_pull_up(sda);
    gpio_pull_up(scl);
}

uint8_t SCCB_Read16(uint8_t slv_addr, uint16_t reg)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t value   = 0xFF;

    /*
     * nostop on the address write: the read has to be a repeated START, or the
     * sensor drops the address it was just given and returns the previous
     * register.
     */
    if (i2c_write_timeout_us(s_i2c, slv_addr, addr, 2, true,
                             SCCB_XFER_TIMEOUT_US) != 2) {
        return 0xFF;
    }
    if (i2c_read_timeout_us(s_i2c, slv_addr, &value, 1, false,
                            SCCB_XFER_TIMEOUT_US) != 1) {
        return 0xFF;
    }
    return value;
}

int SCCB_Write16(uint8_t slv_addr, uint16_t reg, uint8_t data)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), data };

    if (i2c_write_timeout_us(s_i2c, slv_addr, buf, 3, false,
                             SCCB_XFER_TIMEOUT_US) != 3) {
        return -1;
    }
    return 0;
}
