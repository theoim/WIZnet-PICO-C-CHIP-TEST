/**
 * @file    uart_link.c
 * @brief   Interrupt-driven line input on UART0 feeding the data table.
 *
 * See uart_link.h for why reception is interrupt-driven.
 */

#include "uart_link.h"

#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "data_table.h"

#define UART_LINK_UART   uart0
#define UART_LINK_IRQ    UART0_IRQ

/* 4 KB covers about 0.36 s of a saturated 115200 link and far longer at test
 * frame rates (40-byte frames at 10 Hz fill it in 10 s); a 1.7 s handshake
 * stall therefore loses nothing unless the sender runs near line rate. */
#define RING_SIZE        4096u
#define LINE_MAX         128u

static volatile uint8_t  s_ring[RING_SIZE];
static volatile uint32_t s_head;          /* written by the IRQ  */
static volatile uint32_t s_tail;          /* written by the loop */
static volatile uint32_t s_dropped;       /* bytes the ring could not take */
static volatile uint32_t s_hw_errors;     /* framing / parity / break / overrun */

static char     s_line[LINE_MAX];
static uint32_t s_line_len;
static bool     s_discarding;

static uint32_t s_frames_ok;
static uint32_t s_frames_bad;
static uint32_t s_ring_peak;
static char     s_last_bad[48];

static void on_uart_rx(void) {
    while(uart_is_readable(UART_LINK_UART)) {
        uint32_t dr = uart_get_hw(UART_LINK_UART)->dr;
        if(dr & 0xF00u) {                 /* OE | BE | PE | FE */
            s_hw_errors++;
            continue;
        }
        uint32_t next = (s_head + 1u) % RING_SIZE;
        if(next == s_tail) {
            s_dropped++;
            continue;
        }
        s_ring[s_head] = (uint8_t)dr;
        s_head = next;
    }
}

void uart_link_init(void) {
    uart_init(UART_LINK_UART, UART_LINK_BAUD);
    gpio_set_function(UART_LINK_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_LINK_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_LINK_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(UART_LINK_UART, false, false);

    irq_set_exclusive_handler(UART_LINK_IRQ, on_uart_rx);
    irq_set_enabled(UART_LINK_IRQ, true);
    uart_set_irq_enables(UART_LINK_UART, true, false);

    printf("[uart] UART0 %u 8N1, TX GP%u, RX GP%u, $DATA:<ch1>,<ch2>,<ch3>\r\n",
           UART_LINK_BAUD, UART_LINK_TX_PIN, UART_LINK_RX_PIN);
}

static void apply_line(char *line) {
    static const char token[] = "$DATA:";
    const size_t tlen = sizeof(token) - 1u;
    float v[3];

    if(strncmp(line, token, tlen) == 0 && data_table_parse_triplet(line + tlen, v)) {
        data_table_feed_uart_floats(v, 3u, line);
        s_frames_ok++;
        return;
    }

    /* Not echoed per line: a noisy wire would flood the console. The count
     * and the last offender are in the UART command's output. */
    s_frames_bad++;
    data_table_note_uart_error();
    strncpy(s_last_bad, line, sizeof(s_last_bad) - 1u);
    s_last_bad[sizeof(s_last_bad) - 1u] = '\0';
}

void uart_link_poll(void) {
    uint32_t head = s_head;
    uint32_t used = (head + RING_SIZE - s_tail) % RING_SIZE;
    if(used > s_ring_peak)
        s_ring_peak = used;

    while(s_tail != head) {
        char c = (char)s_ring[s_tail];
        s_tail = (s_tail + 1u) % RING_SIZE;

        if(c == '\r' || c == '\n') {
            if(s_discarding) {
                s_discarding = false;
                s_frames_bad++;
                data_table_note_uart_error();
            } else if(s_line_len > 0u) {
                s_line[s_line_len] = '\0';
                apply_line(s_line);
            }
            s_line_len = 0u;
        } else if(c >= 0x20 && c <= 0x7e) {
            if(s_discarding)
                continue;
            if(s_line_len < LINE_MAX - 1u)
                s_line[s_line_len++] = c;
            else
                s_discarding = true;      /* over-long line: drop to newline */
        }
    }
}

void uart_link_print_status(void) {
    printf("[uart] frames ok=%lu bad=%lu  ring dropped=%lu peak=%lu/%u  "
           "hw_errors=%lu",
           (unsigned long)s_frames_ok, (unsigned long)s_frames_bad,
           (unsigned long)s_dropped, (unsigned long)s_ring_peak, RING_SIZE,
           (unsigned long)s_hw_errors);
    if(s_frames_bad)
        printf("  last_bad=\"%s\"", s_last_bad);
    printf("\r\n");
}
