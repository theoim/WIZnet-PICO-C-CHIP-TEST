#ifndef DATA_TABLE_H
#define DATA_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "opcua_settings.h"

#define DATA_TABLE_RAW_MAX 96u

/*
 * Runtime value store, indexed by channel (0 .. channel_count-1). Decoupled
 * from the address space: input drivers write here, the OPC UA DataSource read
 * callback reads here. All numeric channels are stored already scaled
 * (engineering value = raw * scale + offset), applied by data_table_set_raw().
 *
 * status is a UA_StatusCode (0 == UA_STATUSCODE_GOOD). Drivers set a Bad code
 * (e.g. UA_STATUSCODE_BADCOMMUNICATIONERROR) on failure instead of holding a
 * stale value, so quality is exposed honestly.
 */
typedef struct {
    double   value;                    /* scaled numeric (Float/Int32)   */
    bool     bool_value;               /* Bool datatype                  */
    char     str[DATA_TABLE_RAW_MAX];  /* String datatype                */
    uint32_t status;                   /* UA_StatusCode; 0 = Good         */
    uint32_t update_ms;
} channel_value_t;

/* Global input diagnostics (exposed as Device diagnostic nodes). */
typedef struct {
    uint32_t usb_frame_count;
    uint32_t usb_error_count;
    uint32_t uart_frame_count;
    uint32_t uart_error_count;
    uint32_t modbus_poll_count;
    uint32_t modbus_timeout_count;
    char     last_raw[DATA_TABLE_RAW_MAX];
} diag_counters_t;

/* Bind the table to the loaded settings and reset all channels to Bad/no-data.
 * Caches cfg for scale/offset; call again after a settings hot-reload. */
void data_table_init(const opcua_settings_t *cfg);

/* Numeric write: applies channel scale/offset, marks Good, timestamps. */
void data_table_set_raw(uint16_t idx, double raw);

/* Typed writes for non-Float channels. */
void data_table_set_bool(uint16_t idx, bool v);
void data_table_set_string(uint16_t idx, const char *s);

/* Mark a channel's quality (e.g. Bad_CommunicationError) without a value. */
void data_table_set_status(uint16_t idx, uint32_t ua_status_code);

/* Copy the current value of one channel. Returns false if idx out of range. */
bool data_table_get(uint16_t idx, channel_value_t *out);

/* ── Diagnostics ──────────────────────────────────────────────────── */
void data_table_diag_snapshot(diag_counters_t *out);
void data_table_note_usb_frame(const char *raw);
void data_table_note_usb_error(void);
void data_table_note_uart_frame(const char *raw);
void data_table_note_uart_error(void);
void data_table_note_modbus_poll(void);
void data_table_note_modbus_timeout(void);

/* ── Legacy demo helper (A-3 regression) ──────────────────────────────
 * Feed up to 3 comma-separated floats into the first USB-source channels,
 * preserving the original $DATA:<ch1>,<ch2>,<ch3> behavior. */
void data_table_feed_usb_floats(const float *vals, uint16_t n,
                                const char *raw);
void data_table_clear(void);

#endif /* DATA_TABLE_H */
