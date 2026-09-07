#include "opcua_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

/* Layout guards — any field change that breaks these must bump the version. */
_Static_assert(sizeof(channel_def_t) == 80,
               "channel_def_t must be exactly 80 bytes");
_Static_assert(sizeof(opcua_settings_t) == 2816,
               "opcua_settings_t must be exactly 2816 bytes");
_Static_assert(sizeof(opcua_settings_t) % FLASH_PAGE_SIZE == 0,
               "settings size must be a multiple of FLASH_PAGE_SIZE");
_Static_assert(offsetof(opcua_settings_t, crc32) ==
                   sizeof(opcua_settings_t) - sizeof(uint32_t),
               "crc32 must be the last field");

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)   /* RP2350 Pico2 default 4MB */
#endif

/* Last sector of flash. Erase is sector-granular; program is page-granular. */
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_FLASH_XIP \
    ((const opcua_settings_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET))

#define CRC_COVER_LEN (sizeof(opcua_settings_t) - sizeof(uint32_t))

uint32_t opcua_settings_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for(int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void opcua_settings_defaults(opcua_settings_t *out) {
    memset(out, 0, sizeof(*out));

    out->magic = OPCUA_SETTINGS_MAGIC;
    out->schema_version = OPCUA_SETTINGS_VERSION;

    /* Static network default keeps the node reachable at a known address until
     * A-4 wires DHCP; once DHCP is implemented, flip use_dhcp default to 1. */
    out->ip[0] = 192; out->ip[1] = 168; out->ip[2] = 11; out->ip[3] = 2;
    out->gw[0] = 192; out->gw[1] = 168; out->gw[2] = 11; out->gw[3] = 1;
    out->sn[0] = 255; out->sn[1] = 255; out->sn[2] = 255; out->sn[3] = 0;
    out->dns[0] = 8;  out->dns[1] = 8;  out->dns[2] = 8;  out->dns[3] = 8;
    out->use_dhcp = 0;

    strncpy(out->device_name, "WIZnet-OPCUA-Node",
            sizeof(out->device_name) - 1u);

    out->en_modbus = 0;
    out->en_uart = 0;
    out->en_usb = 1;   /* USB $DATA demo path on by default (A-3 regression) */

    out->modbus_baud = 9600u;
    out->modbus_parity = 0u;
    out->modbus_stopbits = 1u;
    out->modbus_poll_ms_default = 1000u;
    out->modbus_timeout_ms = 200u;
    out->modbus_retries = 1u;

    strncpy(out->uart_start_token, "$DATA:",
            sizeof(out->uart_start_token) - 1u);
    strncpy(out->uart_delim, ",", sizeof(out->uart_delim) - 1u);
    out->uart_terminator = 0u;

    strncpy(out->admin_password, "admin", sizeof(out->admin_password) - 1u);
    out->password_is_default = 1u;

    out->sec_policy = 0u;       /* None */
    out->sec_user_token = 0u;   /* Anonymous */

    /* Three demo channels mirroring the original fixed Channel_1..3 floats. */
    out->channel_count = 3u;
    for(uint16_t i = 0; i < 3u; i++) {
        channel_def_t *c = &out->channels[i];
        c->id = (uint16_t)(i + 1u);
        snprintf(c->name, sizeof(c->name), "Channel_%u", (unsigned)(i + 1u));
        c->source = OPCUA_SRC_USB;
        c->datatype = OPCUA_DT_FLOAT;
        c->unit[0] = '\0';
        c->scale = 1.0f;
        c->offset = 0.0f;
        c->eu_low = 0.0f;
        c->eu_high = 100.0f;
        c->enabled = 1u;
        c->function_code = 3u;
        c->register_count = 1u;
    }
}

bool opcua_settings_migrate(opcua_settings_t *cfg, uint16_t from_version) {
    (void)cfg;
    (void)from_version;
    /* Only v1 exists in Phase A. When v2 lands, translate v1→v2 fields here,
     * set cfg->schema_version = OPCUA_SETTINGS_VERSION, and return true. */
    return false;
}

bool opcua_settings_load(opcua_settings_t *out) {
    const opcua_settings_t *flash = SETTINGS_FLASH_XIP;

    if(flash->magic != OPCUA_SETTINGS_MAGIC) {
        opcua_settings_defaults(out);
        printf("[cfg] flash empty/invalid magic — using defaults\r\n");
        return false;
    }

    uint32_t want = opcua_settings_crc32(flash, CRC_COVER_LEN);
    if(want != flash->crc32) {
        opcua_settings_defaults(out);
        printf("[cfg] CRC mismatch (flash=%08lx calc=%08lx) — using defaults\r\n",
               (unsigned long)flash->crc32, (unsigned long)want);
        return false;
    }

    memcpy(out, flash, sizeof(*out));

    if(out->schema_version != OPCUA_SETTINGS_VERSION) {
        uint16_t from = out->schema_version;
        if(from < OPCUA_SETTINGS_VERSION &&
           opcua_settings_migrate(out, from)) {
            printf("[cfg] migrated schema v%u -> v%u\r\n",
                   (unsigned)from, (unsigned)OPCUA_SETTINGS_VERSION);
        } else {
            opcua_settings_defaults(out);
            printf("[cfg] unsupported schema v%u — using defaults\r\n",
                   (unsigned)from);
            return false;
        }
    }

    if(out->channel_count > OPCUA_MAX_CHANNELS)
        out->channel_count = OPCUA_MAX_CHANNELS;

    printf("[cfg] loaded from flash: %u channel(s)\r\n",
           (unsigned)out->channel_count);
    return true;
}

/*
 * Concurrency: single-core cooperative loop → interrupt disable is sufficient.
 * If Core1 is ever launched, bracket the erase/program with
 * multicore_lockout_start_blocking()/multicore_lockout_end_blocking() (Core1
 * must have called multicore_lockout_victim_init()).
 */
static bool flash_write(const opcua_settings_t *img) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET,
                        (const uint8_t *)img, sizeof(*img));
    restore_interrupts(ints);

    const opcua_settings_t *chk = SETTINGS_FLASH_XIP;
    return chk->magic == img->magic && chk->crc32 == img->crc32;
}

bool opcua_settings_save(opcua_settings_t *cfg) {
    cfg->magic = OPCUA_SETTINGS_MAGIC;
    cfg->schema_version = OPCUA_SETTINGS_VERSION;
    if(cfg->channel_count > OPCUA_MAX_CHANNELS)
        cfg->channel_count = OPCUA_MAX_CHANNELS;
    cfg->crc32 = opcua_settings_crc32(cfg, CRC_COVER_LEN);

    bool ok = flash_write(cfg);
    printf("[cfg] save %s (%u bytes, crc=%08lx)\r\n",
           ok ? "ok" : "FAILED",
           (unsigned)sizeof(*cfg), (unsigned long)cfg->crc32);
    return ok;
}

bool opcua_settings_factory_reset(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    bool ok = SETTINGS_FLASH_XIP->magic != OPCUA_SETTINGS_MAGIC;
    printf("[cfg] factory reset %s\r\n", ok ? "ok" : "FAILED");
    return ok;
}
