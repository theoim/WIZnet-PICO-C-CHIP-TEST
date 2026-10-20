#include "usb_stdio_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/error.h"
#include "pico/stdlib.h"

#include "data_table.h"
#include "opcua_server.h"
#include "opcua_settings.h"
#include "sntp_client.h"
#include "uart_link.h"
#include "wiznet_network.h"

#define USB_LINE_MAX 96u

static char s_line[USB_LINE_MAX];
static size_t s_line_len;
static bool s_discarding;
static const opcua_settings_t *s_cfg;

static char *trim(char *s) {
    while(*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}


void usb_stdio_print_help(void) {
    printf("\r\nCommands:\r\n");
    printf("  $DATA:<ch1>,<ch2>,<ch3>\r\n");
    printf("  GET\r\n");
    printf("  NET\r\n");
    printf("  OPCUA\r\n");
    printf("  UART            UART0 (GP0 TX, GP1 RX) input counters\r\n");
    printf("  NTP [a.b.c.d]   clock sync status / set+save server (0.0.0.0 = default)\r\n");
    printf("  CLEAR\r\n");
    printf("  HELP\r\n");
    printf("Example: $DATA:23.50,101.32,65.20\r\n\r\n");
}

void usb_stdio_print_state(void) {
    diag_counters_t d;
    data_table_diag_snapshot(&d);

    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint16_t n = s_cfg ? s_cfg->channel_count : 0u;

    printf("[STATE] usb_frames=%lu usb_err=%lu uart_frames=%lu "
           "modbus_polls=%lu modbus_to=%lu raw=\"%s\"\r\n",
           (unsigned long)d.usb_frame_count,
           (unsigned long)d.usb_error_count,
           (unsigned long)d.uart_frame_count,
           (unsigned long)d.modbus_poll_count,
           (unsigned long)d.modbus_timeout_count,
           d.last_raw);

    for(uint16_t i = 0; i < n; i++) {
        channel_value_t v;
        if(!data_table_get(i, &v))
            continue;
        uint32_t age = v.update_ms ? (uint32_t)(now - v.update_ms) : 0u;
        printf("  [%u] %-16s val=%.3f status=0x%08lx age_ms=%lu\r\n",
               (unsigned)i, s_cfg->channels[i].name, v.value,
               (unsigned long)v.status, (unsigned long)age);
    }
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
    if(strcmp(cmd, "WDTEST") == 0) {
        /* Proves the watchdog end to end: stop feeding it and the board must
         * come back with "reset cause: WATCHDOG". Not in HELP on purpose. */
        printf("[wdt] stalling the main loop; expect a watchdog reset\r\n");
        stdio_flush();
        for(;;)
            tight_loop_contents();
    }
    if(strcmp(cmd, "UART") == 0) {
        uart_link_print_status();
        return;
    }
    if(strcmp(cmd, "NTP") == 0) {
        sntp_client_print_status();
        return;
    }
    if(strncmp(cmd, "NTP ", 4u) == 0) {
        unsigned a, b, c, d;
        char extra;
        if(sscanf(cmd + 4, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
           a > 255u || b > 255u || c > 255u || d > 255u) {
            printf("[sntp] usage: NTP a.b.c.d\r\n");
            return;
        }
        const uint8_t ip[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
        bool saved = sntp_client_set_server(ip);
        printf("[sntp] server set to %u.%u.%u.%u%s, %s, syncing\r\n", a, b, c, d,
               (a | b | c | d) ? "" : " (built-in default)",
               saved ? "saved" : "NOT saved - lost on reboot");
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
        float v[3] = {0.0f, 0.0f, 0.0f};
        if(!data_table_parse_triplet(payload, v)) {
            data_table_note_usb_error();
            printf("[DATA] parse error: %s\r\n", cmd);
            return;
        }
        data_table_feed_usb_floats(v, 3u, cmd);
        printf("[DATA] ok ch1=%.3f ch2=%.3f ch3=%.3f\r\n", v[0], v[1], v[2]);
        return;
    }

    data_table_note_usb_error();
    printf("[USB] unknown command: %s\r\n", cmd);
}

void usb_stdio_link_init(const opcua_settings_t *cfg) {
    s_cfg = cfg;
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
                    data_table_note_usb_error();
                    s_discarding = true;
                }
            }
        }
        ch = getchar_timeout_us(0);
    }
}
