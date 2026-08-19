/*
 * Shim for the two ESP-IDF placement attributes ov3660_settings.h asks for.
 *
 * On ESP32 these move data out of flash so it stays readable while the cache is
 * disabled. RP2350 executes the register tables from flash via XIP with no such
 * hazard, so both expand to nothing and the tables stay in .rodata.
 */
#ifndef _CAM_SHIM_ESP_ATTR_H_
#define _CAM_SHIM_ESP_ATTR_H_

#define DRAM_ATTR
#define IRAM_ATTR

#endif /* _CAM_SHIM_ESP_ATTR_H_ */
