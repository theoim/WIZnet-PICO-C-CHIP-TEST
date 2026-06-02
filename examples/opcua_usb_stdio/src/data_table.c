#include "data_table.h"

#include <string.h>

#include "pico/stdlib.h"

static DataTable s_table;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

void data_table_init(void) {
    data_table_clear();
}

void data_table_clear(void) {
    memset(&s_table, 0, sizeof(s_table));
    strncpy(s_table.raw, "(no frame yet)", sizeof(s_table.raw) - 1u);
}

void data_table_set_channels(float ch1, float ch2, float ch3,
                             const char *raw_frame) {
    s_table.ch1 = ch1;
    s_table.ch2 = ch2;
    s_table.ch3 = ch3;
    s_table.frame_count++;
    s_table.last_update_ms = now_ms();

    if(raw_frame) {
        strncpy(s_table.raw, raw_frame, sizeof(s_table.raw) - 1u);
        s_table.raw[sizeof(s_table.raw) - 1u] = '\0';
    }
}

void data_table_record_parse_error(void) {
    s_table.parse_error_count++;
}

void data_table_snapshot(DataTable *out) {
    if(out)
        *out = s_table;
}
