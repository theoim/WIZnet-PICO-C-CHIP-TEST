/**
 * @file    sntp_client.h
 * @brief   Sets the server's wall clock from an NTP server.
 *
 * Without it the board's UA_DateTime_now() starts at 1970. That is not only
 * cosmetic: security tokens then carry CreatedAt = 1970, and clients that judge
 * a token's grace period from the server's timestamp (asyncua does) drop the
 * connection right after a channel renewal.
 *
 * The client is non-blocking and runs from the main loop. It uses the socket
 * that the buffer plan in wiznet_network.c reserves for DNS/SNTP. There is no
 * DNS in this firmware, so the server is given as an IPv4 address.
 */

#ifndef SNTP_CLIENT_H
#define SNTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts syncing as soon as the network is up. @p server may be NULL for the
 * built-in default (OPCUA_SNTP_SERVER_IP). */
void sntp_client_init(const uint8_t server[4]);

/* Call from the main loop. Returns immediately when there is nothing to do. */
void sntp_client_poll(void);

/* Changes the server and requests a sync now. */
void sntp_client_set_server(const uint8_t server[4]);

void sntp_client_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SNTP_CLIENT_H */
