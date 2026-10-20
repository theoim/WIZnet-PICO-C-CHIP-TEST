/**
 * @file    uart_link.h
 * @brief   Line-based data input on a hardware UART (UART0, GP0 TX / GP1 RX).
 *
 * The same "$DATA:<ch1>,<ch2>,<ch3>" frames the USB console accepts, from an
 * external device or a USB-TTL adapter. It exists to drive the OPC UA update
 * path - UART -> data table -> address space -> subscriptions - with real
 * traffic, so a soak test exercises notifications and not an idle server.
 *
 * Reception is interrupt-driven into a ring buffer. That matters here: one RSA
 * handshake blocks the main loop for about 1.7 s, the UART FIFO is 32 bytes,
 * and at 115200 baud a polled receiver would drop everything after the first
 * few milliseconds of every handshake. Bytes the ring could not take are
 * counted, not hidden.
 */

#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UART_LINK_BAUD
#define UART_LINK_BAUD    115200u
#endif
#ifndef UART_LINK_TX_PIN
#define UART_LINK_TX_PIN  0u
#endif
#ifndef UART_LINK_RX_PIN
#define UART_LINK_RX_PIN  1u
#endif

void uart_link_init(void);

/* Call from the main loop: assembles lines from the ring and applies frames. */
void uart_link_poll(void);

void uart_link_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_LINK_H */
