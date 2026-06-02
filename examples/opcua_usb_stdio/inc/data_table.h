#ifndef DATA_TABLE_H
#define DATA_TABLE_H

#include <stdint.h>

#define DATA_TABLE_RAW_MAX 96u

typedef struct {
    float ch1;
    float ch2;
    float ch3;
    char raw[DATA_TABLE_RAW_MAX];
    uint32_t frame_count;
    uint32_t parse_error_count;
    uint32_t last_update_ms;
} DataTable;

void data_table_init(void);
void data_table_clear(void);
void data_table_set_channels(float ch1, float ch2, float ch3,
                             const char *raw_frame);
void data_table_record_parse_error(void);
void data_table_snapshot(DataTable *out);

#endif /* DATA_TABLE_H */
