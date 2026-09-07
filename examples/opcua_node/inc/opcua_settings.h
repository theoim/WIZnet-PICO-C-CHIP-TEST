#ifndef OPCUA_SETTINGS_H
#define OPCUA_SETTINGS_H

/*
 * opcua_settings.h — persisted configuration for the WIZnet OPC UA node.
 *
 * Single source of truth shared by:
 *   - opcua_server.c   (builds the address space from the channel table)
 *   - the input drivers (modbus / uart_raw / usb_stdio)
 *   - the provisioning surfaces (web dashboard + USB-CDC CLI, Phase A-4)
 *
 * The whole struct is the on-flash image. It is stored in the last flash
 * sector with an integrity envelope: magic + schema_version + CRC32. On load,
 * a mismatch on any of the three rolls back to compiled defaults.
 *
 * Flash-write concurrency: main.c currently runs a single-core cooperative
 * loop, so opcua_settings_save() only disables interrupts. If a future phase
 * launches Core1 (e.g. A-5), the save path MUST additionally bracket the flash
 * op with multicore_lockout_start_blocking()/end_blocking(). See the note in
 * opcua_settings.c.
 */

#include <stdbool.h>
#include <stdint.h>

#define OPCUA_SETTINGS_MAGIC     0x4F504E31u   /* 'O','P','N','1' */
#define OPCUA_SETTINGS_VERSION   1u

#define OPCUA_MAX_CHANNELS       32u

/* 동시에 받을 OPC UA 클라이언트 수 = 트랜스포트가 LISTEN 하는 하드웨어 소켓 수.
 * 소켓 버퍼 배분(wiznet_network.c 의 memsize)과 반드시 일치해야 한다.
 * docs/product_direction.md D-5 / D-7 */
#define OPCUA_MAX_SOCKETS         4u
#define OPCUA_NAME_MAX           32u
#define OPCUA_UNIT_MAX           16u

/* Channel data source. */
typedef enum {
    OPCUA_SRC_USB    = 0,   /* USB-CDC $DATA frames (debug / education)      */
    OPCUA_SRC_UART   = 1,   /* UART_RAW delimiter-based text frames          */
    OPCUA_SRC_MODBUS = 2    /* Modbus RTU master poll                        */
} opcua_channel_source_t;

/* OPC UA value type exposed for the channel. */
typedef enum {
    OPCUA_DT_FLOAT  = 0,
    OPCUA_DT_INT32  = 1,
    OPCUA_DT_BOOL   = 2,
    OPCUA_DT_STRING = 3
} opcua_channel_datatype_t;

/* Modbus register word order for multi-register (32-bit) values. */
typedef enum {
    OPCUA_WORD_ORDER_BIG    = 0,   /* high word first (ABCD)                 */
    OPCUA_WORD_ORDER_LITTLE = 1    /* low word first  (CDAB)                 */
} opcua_word_order_t;

/*
 * Per-channel definition. Persisted verbatim inside opcua_settings_t.
 * Layout is fixed at 80 bytes with no implicit padding (see static_assert in
 * opcua_settings.c). If you add/remove a field, bump OPCUA_SETTINGS_VERSION and
 * extend opcua_settings_migrate().
 *
 * Extensions beyond the original TASK_BRIEF schema (documented in
 * docs/node_map.md): eu_low, eu_high (EURange property source), enabled,
 * poll_ms (per-channel Modbus poll override).
 */
typedef struct {
    uint16_t id;                    /* stable channel id (also numeric NodeId base) */
    char     name[OPCUA_NAME_MAX];  /* browse/display name; must be unique          */
    uint8_t  source;                /* opcua_channel_source_t                        */
    uint8_t  datatype;              /* opcua_channel_datatype_t                      */
    char     unit[OPCUA_UNIT_MAX];  /* EngineeringUnits text (e.g. "degC")           */
    float    scale;                 /* engineering = raw * scale + offset            */
    float    offset;
    float    eu_low;                /* EURange low  (display hint)                   */
    float    eu_high;               /* EURange high                                  */
    uint8_t  enabled;               /* 0 = defined but not instantiated              */
    /* ── Modbus-only ─────────────────────────────────────────────── */
    uint8_t  slave_addr;
    uint8_t  function_code;         /* 3|4 (read reg/input), 1|2 (read coil/discrete)*/
    uint8_t  word_order;            /* opcua_word_order_t                            */
    uint16_t register_addr;
    uint16_t register_count;        /* 1 = 16-bit, 2 = 32-bit                        */
    uint16_t poll_ms;               /* 0 = use modbus_poll_ms_default                */
    uint16_t _pad;
} channel_def_t;

/* ── Top-level persisted settings ──────────────────────────────────── */
typedef struct {
    uint32_t magic;                 /* OPCUA_SETTINGS_MAGIC                          */
    uint16_t schema_version;        /* OPCUA_SETTINGS_VERSION                        */
    uint16_t channel_count;         /* number of valid entries in channels[]         */

    /* ── Network ──────────────────────────────────────────────────── */
    uint8_t  ip[4];
    uint8_t  gw[4];
    uint8_t  sn[4];
    uint8_t  dns[4];
    uint8_t  use_dhcp;              /* 0 = static, 1 = DHCP (A-4)                     */
    uint8_t  _pad_net[3];

    char     device_name[OPCUA_NAME_MAX];   /* OPC UA root object name               */

    /* ── Input-mode enables ───────────────────────────────────────── */
    uint8_t  en_modbus;
    uint8_t  en_uart;
    uint8_t  en_usb;
    uint8_t  _pad_in;

    /* ── Modbus RTU global (A-2) ──────────────────────────────────── */
    uint32_t modbus_baud;           /* default 9600                                  */
    uint8_t  modbus_parity;         /* 0=N,1=E,2=O                                   */
    uint8_t  modbus_stopbits;       /* 1 or 2                                         */
    uint16_t modbus_poll_ms_default;/* default 1000                                  */
    uint16_t modbus_timeout_ms;     /* default 200                                   */
    uint8_t  modbus_retries;        /* default 1                                     */
    uint8_t  _pad_mb;

    /* ── UART_RAW parser (A-3) ────────────────────────────────────── */
    char     uart_start_token[16];  /* default "$DATA:"                              */
    char     uart_delim[4];         /* default ","                                   */
    uint8_t  uart_terminator;       /* 0 = CR/LF                                      */
    uint8_t  _pad_uart[3];

    /* ── Dashboard auth (A-4) ─────────────────────────────────────── */
    char     admin_password[32];    /* plaintext for Phase A; hash in Phase B        */
    uint8_t  password_is_default;   /* force change on first login                   */
    uint8_t  _pad_auth[3];

    /* ── Phase B security placeholders (reserved, inactive) ───────── */
    uint8_t  sec_policy;            /* 0 = None (only supported value in Phase A)    */
    uint8_t  sec_user_token;        /* 0 = Anonymous                                 */
    uint16_t sec_cert_slot_off;     /* reserved: offset of cert blob (Phase B)       */
    uint8_t  sec_reserved[28];

    /* ── Reliability record (A-5) ─────────────────────────────────── */
    uint32_t reboot_count;
    uint8_t  reboot_reason;         /* 0=power,1=watchdog,2=user/save                */
    uint8_t  _pad_rel[3];

    /* ── Channel table ────────────────────────────────────────────── */
    channel_def_t channels[OPCUA_MAX_CHANNELS];

    uint8_t  _pad_tail[76];
    uint32_t crc32;                 /* over bytes [0, sizeof-4)                       */
} opcua_settings_t;

/* Fill *out with compiled defaults (3 USB Float channels → demo parity). */
void     opcua_settings_defaults(opcua_settings_t *out);

/* Load from flash. Falls back to defaults on magic/version/CRC mismatch.
 * Returns true if a valid image was loaded, false if defaults were used. */
bool     opcua_settings_load(opcua_settings_t *out);

/* Recompute CRC and persist to flash. Returns false on flash error. */
bool     opcua_settings_save(opcua_settings_t *cfg);

/* Erase the settings sector (factory reset). Next load returns defaults. */
bool     opcua_settings_factory_reset(void);

/* CRC32 (reflected, poly 0xEDB88320) over len bytes. */
uint32_t opcua_settings_crc32(const void *data, uint32_t len);

/* Migration hook. Called by load() when a valid magic/CRC image has an older
 * schema_version. Upgrade *cfg in place and return true, or false to reject
 * (caller then rolls back to defaults). Stub for Phase A (only v1 exists). */
bool     opcua_settings_migrate(opcua_settings_t *cfg, uint16_t from_version);

#endif /* OPCUA_SETTINGS_H */
