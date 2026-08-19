#include "data_table.h"

#include <string.h>

#include "pico/stdlib.h"

/* UA_STATUSCODE_GOOD == 0; avoid pulling open62541 into this translation unit. */
#define DT_STATUS_GOOD 0x00000000u

static const opcua_settings_t *s_cfg;
static channel_value_t s_values[OPCUA_MAX_CHANNELS];
static diag_counters_t s_diag;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

void data_table_clear(void) {
    memset(s_values, 0, sizeof(s_values));
    memset(&s_diag, 0, sizeof(s_diag));
    strncpy(s_diag.last_raw, "(no frame yet)", sizeof(s_diag.last_raw) - 1u);
    for(uint16_t i = 0; i < OPCUA_MAX_CHANNELS; i++)
        s_values[i].status = DT_STATUS_GOOD; /* no data yet, but not an error */
}

void data_table_init(const opcua_settings_t *cfg) {
    s_cfg = cfg;
    data_table_clear();
}

static channel_value_t *slot(uint16_t idx) {
    if(!s_cfg || idx >= s_cfg->channel_count || idx >= OPCUA_MAX_CHANNELS)
        return NULL;
    return &s_values[idx];
}

void data_table_set_raw(uint16_t idx, double raw) {
    channel_value_t *v = slot(idx);
    if(!v)
        return;
    const channel_def_t *c = &s_cfg->channels[idx];
    v->value = raw * (double)c->scale + (double)c->offset;
    v->status = DT_STATUS_GOOD;
    v->update_ms = now_ms();
}

void data_table_set_bool(uint16_t idx, bool b) {
    channel_value_t *v = slot(idx);
    if(!v)
        return;
    v->bool_value = b;
    v->value = b ? 1.0 : 0.0;
    v->status = DT_STATUS_GOOD;
    v->update_ms = now_ms();
}

void data_table_set_string(uint16_t idx, const char *s) {
    channel_value_t *v = slot(idx);
    if(!v)
        return;
    strncpy(v->str, s ? s : "", sizeof(v->str) - 1u);
    v->str[sizeof(v->str) - 1u] = '\0';
    v->status = DT_STATUS_GOOD;
    v->update_ms = now_ms();
}

void data_table_set_status(uint16_t idx, uint32_t ua_status_code) {
    channel_value_t *v = slot(idx);
    if(!v)
        return;
    v->status = ua_status_code;
    v->update_ms = now_ms();
}

bool data_table_get(uint16_t idx, channel_value_t *out) {
    channel_value_t *v = slot(idx);
    if(!v || !out)
        return false;
    *out = *v;
    return true;
}

/* ── Diagnostics ──────────────────────────────────────────────────── */
void data_table_diag_snapshot(diag_counters_t *out) {
    if(out)
        *out = s_diag;
}

static void set_last_raw(const char *raw) {
    if(!raw)
        return;
    strncpy(s_diag.last_raw, raw, sizeof(s_diag.last_raw) - 1u);
    s_diag.last_raw[sizeof(s_diag.last_raw) - 1u] = '\0';
}

void data_table_note_usb_frame(const char *raw) {
    s_diag.usb_frame_count++;
    set_last_raw(raw);
}
void data_table_note_usb_error(void) { s_diag.usb_error_count++; }
void data_table_note_uart_frame(const char *raw) {
    s_diag.uart_frame_count++;
    set_last_raw(raw);
}
void data_table_note_uart_error(void) { s_diag.uart_error_count++; }
void data_table_note_modbus_poll(void) { s_diag.modbus_poll_count++; }
void data_table_note_modbus_timeout(void) { s_diag.modbus_timeout_count++; }

/* ── Legacy demo helper ───────────────────────────────────────────── */
void data_table_feed_usb_floats(const float *vals, uint16_t n,
                                const char *raw) {
    if(!s_cfg)
        return;
    uint16_t fed = 0;
    for(uint16_t i = 0; i < s_cfg->channel_count && fed < n; i++) {
        if(s_cfg->channels[i].source != OPCUA_SRC_USB)
            continue;
        data_table_set_raw(i, (double)vals[fed]);
        fed++;
    }
    data_table_note_usb_frame(raw);
}
