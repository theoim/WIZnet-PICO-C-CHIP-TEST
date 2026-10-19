/**
 * @file    sntp_client.c
 * @brief   Minimal non-blocking SNTP client (RFC 4330) on a WIZnet UDP socket.
 *
 * ioLibrary ships an SNTP client, but it is built around a calendar struct and
 * a timezone table and is driven by blocking calls. The server only needs one
 * number - the current UTC as a UA_DateTime - so this sends one 48-byte request,
 * waits for the answer without blocking the loop, and hands the result to
 * wiz_clock_set_utc(). That function moves only the wall clock; the monotonic
 * clock that open62541's timers run on is untouched, so a sync does not fire
 * every pending timer at once.
 */

#include "sntp_client.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "socket.h"
#include "wizchip_conf.h"

#include "wiznet_eventloop_config.h"
#include "wiznet_network.h"

/* Socket 6 is the DNS/SNTP slot in the buffer plan (wiznet_network.c). There is
 * no DNS client in this firmware, so the slot is SNTP's alone for now. */
#ifndef OPCUA_SNTP_SOCKET
#define OPCUA_SNTP_SOCKET 6u
#endif

/* time.google.com (anycast). Overridable at build time or with the NTP command;
 * a plant network will usually point this at its own server. */
#ifndef OPCUA_SNTP_SERVER_IP
#define OPCUA_SNTP_SERVER_IP {216, 239, 35, 4}
#endif

#define SNTP_SERVER_PORT     123u
#define SNTP_LOCAL_PORT      50123u
#define SNTP_PACKET_LEN      48u

#define SNTP_TIMEOUT_MS      2000u
#define SNTP_RETRY_FIRST_MS  10000u      /* until the first sync */
#define SNTP_RETRY_LATER_MS  60000u      /* a resync failed; the clock still runs */
#define SNTP_RESYNC_MS       3600000u    /* drift of the crystal is small at 1 h */

/* NTP counts seconds from 1900, UA_DateTime counts 100 ns ticks from 1601. */
#define NTP_TO_UNIX_SECONDS  2208988800LL
#define UA_TICKS_PER_SECOND  10000000LL
#define UA_UNIX_EPOCH_TICKS  116444736000000000LL

typedef enum {
    SNTP_IDLE = 0,       /* waiting for the next scheduled request */
    SNTP_WAITING,        /* request sent, answer not in yet */
} sntp_state_t;

static struct {
    uint8_t      server[4];
    sntp_state_t state;
    bool         socket_open;
    uint32_t     next_ms;        /* when to send the next request */
    uint32_t     sent_ms;
    uint64_t     sent_us;
    uint8_t      nonce[8];       /* our transmit timestamp, echoed as originate */

    uint32_t     ok_count;
    uint32_t     fail_count;
    uint32_t     last_ok_ms;
    uint32_t     last_rtt_ms;
    uint8_t      last_stratum;
    const char  *last_error;
    bool         announce;       /* log the next success (boot, server change) */
} s;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* An NTP timestamp (seconds since 1900 + 32-bit fraction) as UA_DateTime.
 * Seconds below 2^31 are taken as era 1 (after 2036-02-07); valid until 2104. */
static int64_t ntp_to_ua(const uint8_t *p) {
    int64_t sec = (int64_t)rd32(p);
    uint32_t frac = rd32(p + 4);
    if(sec < 0x80000000LL)
        sec += 0x100000000LL;
    int64_t ticks = (sec - NTP_TO_UNIX_SECONDS) * UA_TICKS_PER_SECOND;
    ticks += (int64_t)(((uint64_t)frac * (uint64_t)UA_TICKS_PER_SECOND) >> 32);
    return UA_UNIX_EPOCH_TICKS + ticks;
}

static void schedule(uint32_t delay_ms) {
    s.state = SNTP_IDLE;
    s.next_ms = now_ms() + delay_ms;
}

static void fail(const char *why) {
    s.fail_count++;
    s.last_error = why;
    printf("[sntp] %s (server %u.%u.%u.%u)\r\n", why,
           s.server[0], s.server[1], s.server[2], s.server[3]);
    schedule(wiz_clock_is_synced() ? SNTP_RETRY_LATER_MS : SNTP_RETRY_FIRST_MS);
}

static bool open_socket(void) {
    if(s.socket_open)
        return true;
    if(socket(OPCUA_SNTP_SOCKET, Sn_MR_UDP, SNTP_LOCAL_PORT, SF_IO_NONBLOCK) !=
       (int8_t)OPCUA_SNTP_SOCKET)
        return false;
    s.socket_open = true;
    return true;
}

static void send_request(void) {
    if(!open_socket()) {
        fail("socket open failed");
        return;
    }

    /* Throw away anything left from an earlier, timed-out exchange so a late
     * reply is not mistaken for the answer to this request. */
    uint8_t drain[SNTP_PACKET_LEN];
    while(getSn_RX_RSR(OPCUA_SNTP_SOCKET) > 0) {
        uint8_t ip[16];
        uint16_t port;
#if (_WIZCHIP_ == 6100) || (_WIZCHIP_ == 6300)
        uint8_t alen;
        if(recvfrom(OPCUA_SNTP_SOCKET, drain, sizeof(drain), ip, &port, &alen) <= 0)
            break;
#else
        if(recvfrom(OPCUA_SNTP_SOCKET, drain, sizeof(drain), ip, &port) <= 0)
            break;
#endif
    }

    uint8_t pkt[SNTP_PACKET_LEN] = {0};
    pkt[0] = 0x23;                       /* LI 0, version 4, mode 3 (client) */

    /* The transmit timestamp only has to come back unchanged in the reply's
     * originate field; it proves the reply answers this request. The local
     * microsecond counter is unique enough. */
    s.sent_us = time_us_64();
    for(unsigned i = 0; i < 8u; i++)
        s.nonce[i] = (uint8_t)(s.sent_us >> (56u - 8u * i));
    memcpy(&pkt[40], s.nonce, 8);

#if (_WIZCHIP_ == 6100) || (_WIZCHIP_ == 6300)
    int32_t rc = sendto(OPCUA_SNTP_SOCKET, pkt, sizeof(pkt), s.server,
                        SNTP_SERVER_PORT, 4);
#else
    int32_t rc = sendto(OPCUA_SNTP_SOCKET, pkt, sizeof(pkt), s.server,
                        SNTP_SERVER_PORT);
#endif
    if(rc != (int32_t)sizeof(pkt)) {
        fail("send failed");
        return;
    }

    s.sent_ms = now_ms();
    s.state = SNTP_WAITING;
}

static void receive_reply(void) {
    if(getSn_RX_RSR(OPCUA_SNTP_SOCKET) == 0) {
        if((uint32_t)(now_ms() - s.sent_ms) >= SNTP_TIMEOUT_MS)
            fail("no reply");
        return;
    }

    uint64_t recv_us = time_us_64();
    uint8_t pkt[SNTP_PACKET_LEN];
    uint8_t ip[16];
    uint16_t port;
#if (_WIZCHIP_ == 6100) || (_WIZCHIP_ == 6300)
    uint8_t alen;
    int32_t n = recvfrom(OPCUA_SNTP_SOCKET, pkt, sizeof(pkt), ip, &port, &alen);
#else
    int32_t n = recvfrom(OPCUA_SNTP_SOCKET, pkt, sizeof(pkt), ip, &port);
#endif
    if(n < (int32_t)SNTP_PACKET_LEN)
        return;                          /* runt; keep waiting until timeout */
    if(memcmp(ip, s.server, 4) != 0 || port != SNTP_SERVER_PORT)
        return;                          /* not from our server */
    if(memcmp(&pkt[24], s.nonce, 8) != 0)
        return;                          /* answers some earlier request */

    uint8_t li = pkt[0] >> 6;
    uint8_t mode = pkt[0] & 0x07u;
    uint8_t stratum = pkt[1];
    if(mode != 4u || li == 3u || stratum == 0u || stratum > 15u) {
        /* stratum 0 is a kiss-of-death (e.g. RATE), li 3 an unsynchronised
         * server: neither carries a usable time. */
        fail("server not usable");
        return;
    }

    /* Server receive (T2) and transmit (T3) times, and our own round trip.
     * The network delay is the round trip minus the server's hold time; the
     * reply spent about half of it in flight, so now = T3 + delay / 2. */
    int64_t t2 = ntp_to_ua(&pkt[32]);
    int64_t t3 = ntp_to_ua(&pkt[40]);
    int64_t rtt = (int64_t)(recv_us - s.sent_us) * 10;    /* us -> 100 ns */
    int64_t delay = rtt - (t3 - t2);
    if(delay < 0)
        delay = 0;

    wiz_clock_set_utc(t3 + delay / 2);

    s.ok_count++;
    s.last_ok_ms = now_ms();
    s.last_rtt_ms = (uint32_t)((recv_us - s.sent_us) / 1000u);
    s.last_stratum = stratum;
    s.last_error = NULL;
    if(s.announce)
        printf("[sntp] clock set from %u.%u.%u.%u (stratum %u, rtt %lu ms)\r\n",
               s.server[0], s.server[1], s.server[2], s.server[3],
               (unsigned)stratum, (unsigned long)s.last_rtt_ms);
    s.announce = false;
    schedule(SNTP_RESYNC_MS);
}

static const uint8_t s_default_server[4] = OPCUA_SNTP_SERVER_IP;
static opcua_settings_t *s_cfg;

static bool is_unset(const uint8_t ip[4]) {
    return (ip[0] | ip[1] | ip[2] | ip[3]) == 0u;
}

void sntp_client_init(opcua_settings_t *cfg) {
    memset(&s, 0, sizeof(s));
    s_cfg = cfg;
    const uint8_t *ip = (cfg && !is_unset(cfg->ntp_server)) ? cfg->ntp_server
                                                            : s_default_server;
    memcpy(s.server, ip, 4);
    s.announce = true;
    schedule(0);
}

bool sntp_client_set_server(const uint8_t server[4]) {
    memcpy(s.server, is_unset(server) ? s_default_server : server, 4);
    s.last_error = NULL;
    /* Hourly resyncs stay quiet; the first answer from a newly set server is
     * the confirmation the operator is waiting for. */
    s.announce = true;
    schedule(0);

    if(!s_cfg)
        return false;
    /* Store what was asked for, not what is in use: 0.0.0.0 stays "follow
     * the built-in default" rather than freezing today's default address. */
    memcpy(s_cfg->ntp_server, server, 4);
    /* The flash erase runs with interrupts off for tens of milliseconds. TCP
     * peers retransmit through that; it is a configuration command, not a
     * per-message path. */
    return opcua_settings_save(s_cfg);
}

void sntp_client_poll(void) {
    /* The socket keeps its state across link loss; requests simply wait for
     * the link, and a lost reply is a timeout like any other. */
    if(!wiznet_network_is_ready())
        return;

    if(s.state == SNTP_WAITING) {
        receive_reply();
        return;
    }
    if((int32_t)(now_ms() - s.next_ms) >= 0)
        send_request();
}

void sntp_client_print_status(void) {
    bool from_cfg = s_cfg && !is_unset(s_cfg->ntp_server);
    printf("[sntp] server=%u.%u.%u.%u (%s) synced=%s ok=%lu fail=%lu",
           s.server[0], s.server[1], s.server[2], s.server[3],
           from_cfg ? "saved" : "default",
           wiz_clock_is_synced() ? "yes" : "no",
           (unsigned long)s.ok_count, (unsigned long)s.fail_count);
    if(s.ok_count)
        printf(" last_ok=%lus ago stratum=%u rtt=%lums",
               (unsigned long)((now_ms() - s.last_ok_ms) / 1000u),
               (unsigned)s.last_stratum, (unsigned long)s.last_rtt_ms);
    if(s.last_error)
        printf(" last_error=\"%s\"", s.last_error);
    printf("\r\n");
}
