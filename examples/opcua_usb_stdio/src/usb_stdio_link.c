#include "usb_stdio_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/error.h"
#include "pico/stdlib.h"

#include "data_table.h"
#include "opcua_server.h"
#include "wiznet_network.h"

#define USB_LINE_MAX 96u

static char s_line[USB_LINE_MAX];
static size_t s_line_len;
static bool s_discarding;

static char *trim(char *s) {
    while(*s == ' ' || *s == '\t')
        s++;

    char *end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    return s;
}

static bool parse_float_token(const char **cursor, float *out) {
    char *end = NULL;
    float value = strtof(*cursor, &end);
    if(end == *cursor)
        return false;

    while(*end == ' ' || *end == '\t')
        end++;

    *cursor = end;
    *out = value;
    return true;
}

static bool parse_data_payload(const char *payload,
                               float *ch1, float *ch2, float *ch3) {
    const char *p = payload;

    while(*p == ' ' || *p == '\t')
        p++;

    if(!parse_float_token(&p, ch1))
        return false;
    if(*p++ != ',')
        return false;
    while(*p == ' ' || *p == '\t')
        p++;

    if(!parse_float_token(&p, ch2))
        return false;
    if(*p++ != ',')
        return false;
    while(*p == ' ' || *p == '\t')
        p++;

    if(!parse_float_token(&p, ch3))
        return false;

    return *p == '\0';
}

void usb_stdio_print_help(void) {
    printf("\r\nCommands:\r\n");
    printf("  $DATA:<ch1>,<ch2>,<ch3>\r\n");
    printf("  GET\r\n");
    printf("  NET\r\n");
    printf("  OPCUA\r\n");
    printf("  CLEAR\r\n");
    printf("  HELP\r\n");
    printf("Example: $DATA:23.50,101.32,65.20\r\n\r\n");
}

void usb_stdio_print_state(void) {
    DataTable snapshot;
    data_table_snapshot(&snapshot);

    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t age = snapshot.last_update_ms ?
        (uint32_t)(now - snapshot.last_update_ms) : 0u;

    printf("[STATE] frames=%lu errors=%lu ch1=%.3f ch2=%.3f ch3=%.3f "
           "age_ms=%lu raw=\"%s\"\r\n",
           (unsigned long)snapshot.frame_count,
           (unsigned long)snapshot.parse_error_count,
           snapshot.ch1, snapshot.ch2, snapshot.ch3,
           (unsigned long)age,
           snapshot.raw);
}

static void process_line(char *line) {
    char *cmd = trim(line);
    if(cmd[0] == '\0')
        return;

    if(strcmp(cmd, "HELP") == 0 || strcmp(cmd, "?") == 0) {
        usb_stdio_print_help();
        return;
    }

    if(strcmp(cmd, "GET") == 0) {
        usb_stdio_print_state();
        return;
    }

    if(strcmp(cmd, "NET") == 0) {
        wiznet_network_print_status();
        return;
    }

    if(strcmp(cmd, "OPCUA") == 0) {
        opcua_server_print_status();
        return;
    }

    if(strcmp(cmd, "CLEAR") == 0) {
        data_table_clear();
        printf("[DATA] cleared\r\n");
        return;
    }

    const char *payload = NULL;
    if(strncmp(cmd, "$DATA:", 6u) == 0)
        payload = cmd + 6u;
    else if(strncmp(cmd, "DATA:", 5u) == 0)
        payload = cmd + 5u;

    if(payload) {
        float ch1 = 0.0f;
        float ch2 = 0.0f;
        float ch3 = 0.0f;

        if(!parse_data_payload(payload, &ch1, &ch2, &ch3)) {
            data_table_record_parse_error();
            printf("[DATA] parse error: %s\r\n", cmd);
            return;
        }

        data_table_set_channels(ch1, ch2, ch3, cmd);

        DataTable snapshot;
        data_table_snapshot(&snapshot);
        printf("[DATA] ok frame=%lu ch1=%.3f ch2=%.3f ch3=%.3f\r\n",
               (unsigned long)snapshot.frame_count,
               snapshot.ch1, snapshot.ch2, snapshot.ch3);
        return;
    }

    data_table_record_parse_error();
    printf("[USB] unknown command: %s\r\n", cmd);
}

void usb_stdio_link_init(void) {
    s_line_len = 0u;
    s_discarding = false;
}

void usb_stdio_link_poll(void) {
    int ch = getchar_timeout_us(0);

    while(ch != PICO_ERROR_TIMEOUT) {
        char c = (char)ch;

        if(c == '\r' || c == '\n') {
            if(s_discarding) {
                s_discarding = false;
                s_line_len = 0u;
                printf("[USB] input too long, discarded\r\n");
            } else if(s_line_len > 0u) {
                s_line[s_line_len] = '\0';
                process_line(s_line);
                s_line_len = 0u;
            }
        } else if(c == '\b' || c == 0x7f) {
            if(s_line_len > 0u)
                s_line_len--;
        } else if(c >= 0x20 && c <= 0x7e) {
            if(!s_discarding) {
                if(s_line_len < sizeof(s_line) - 1u) {
                    s_line[s_line_len++] = c;
                } else {
                    data_table_record_parse_error();
                    s_discarding = true;
                }
            }
        }

        ch = getchar_timeout_us(0);
    }
}
