/*
 * W55RP20_EVB_PICO -- L4 Dual-Stack TCP Gateway
 *
 * Hardware:
 *   RP2040 + internal W5500 (pio1, GPIO 20-25)
 *   RP2040 PIO MII -> external YT8111 PHY (pio0, GPIO 2-15)
 *
 * Network:
 *   W5500  (SPE side, pio1) : 192.168.11.13 -- listens on GW_W5500_LISTEN_PORT
 *   MII/lwIP (RJ45, pio0)   : 192.168.11.12 -- connects out to GW_MII_TARGET_IP
 *
 * Data flow:
 *   SPE client -> W5500 recv -> g_rb_w2m -> lwIP tcp_write -> RJ45 target
 *   RJ45 target -> lwIP recv -> g_rb_m2w -> W5500 send    -> SPE client
 *
 * Core 0: W5500 server + session FSM
 * Core 1: MII poll + lwIP + ring-buffer drain
 */

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/timeouts.h"

#include "rmii_ethernet/netif.h"

#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "gw_ringbuf.h"
#include "gw_session.h"

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */
#define GW_W5500_LISTEN_PORT  5000
#define GW_W5500_SOCKET       0
#define GW_MII_TARGET_IP      "192.168.11.5"   /* RJ45 target server IP */
#define GW_MII_TARGET_PORT    5001

#define MII_IP_ADDR           "192.168.11.12"
#define MII_NETMASK           "255.255.255.0"
#define MII_GATEWAY           "192.168.11.1"

static wiz_NetInfo g_net_info = {
    .mac  = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x99},
    .ip   = {192, 168, 11, 13},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 11, 1},
    .dns  = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC,
};

/* ------------------------------------------------------------------ */
/*  Shared state                                                       */
/* ------------------------------------------------------------------ */
static gateway_session_t g_session;
static gw_rb_t g_rb_w2m;
static gw_rb_t g_rb_m2w;

static uint8_t g_w5500_rxbuf[GW_RB_SLOT_SIZE];

/* ------------------------------------------------------------------ */
/*  YT8111 PHY init                                                    */
/* ------------------------------------------------------------------ */
static void mdio_write_ext(uint16_t reg_addr, uint16_t data)
{
    int phy = netif_rmii_ethernet_get_phy_address();
    netif_rmii_ethernet_mdio_write(phy, 0x1E, reg_addr);
    netif_rmii_ethernet_mdio_write(phy, 0x1F, data);
}

static void YT8111_init(void)
{
    printf("[YT8111] Starting PHY initialization...\n");

    mdio_write_ext(0x0221, 0x2a2a);
    mdio_write_ext(0x2506, 0x0255);
    mdio_write_ext(0x0510, 0x64f0);
    mdio_write_ext(0x0511, 0x70f0);
    mdio_write_ext(0x0512, 0x78f0);
    mdio_write_ext(0x0507, 0xff80);
    mdio_write_ext(0xA401, 0x0A04);
    mdio_write_ext(0xA400, 0x0A04);
    mdio_write_ext(0xA108, 0x0300);
    mdio_write_ext(0xA109, 0x0800);
    mdio_write_ext(0xA304, 0x0004);
    mdio_write_ext(0xA301, 0x0810);
    mdio_write_ext(0x0500, 0x002f);
    mdio_write_ext(0xA206, 0x1500);
    mdio_write_ext(0xA203, 0x1414);
    mdio_write_ext(0xA208, 0x1515);
    mdio_write_ext(0xA209, 0x1314);
    mdio_write_ext(0xA20B, 0x2D04);
    mdio_write_ext(0xA20C, 0x1500);
    mdio_write_ext(0x0522, 0x0FFF);
    mdio_write_ext(0x0403, 0x00FF);
    mdio_write_ext(0xA51F, 0x1070);
    mdio_write_ext(0x051A, 0x06F0);
    mdio_write_ext(0xA403, 0x1C00);
    mdio_write_ext(0x0506, 0xFFE0);
    mdio_write_ext(0x2513, 0x3C1A);
    mdio_write_ext(0x0528, 0x0020);
    mdio_write_ext(0x0516, 0x0050);
    mdio_write_ext(0x00,   0x9100);

    printf("[YT8111] PHY reset issued. Waiting...\n");
    sleep_ms(500);

    int phy = netif_rmii_ethernet_get_phy_address();
    netif_rmii_ethernet_mdio_write(phy, 4, 0x0061);
    netif_rmii_ethernet_mdio_write(phy, 0, 0x1100);
    sleep_ms(200);

    printf("[YT8111] Initialization complete.\n");
}

/* ------------------------------------------------------------------ */
/*  MII netif                                                          */
/* ------------------------------------------------------------------ */
static struct netif g_mii_netif;

static void mii_link_callback(struct netif *netif)
{
    printf("[MII] Link %s\n", netif_is_link_up(netif) ? "UP" : "DOWN");
}

static void mii_status_callback(struct netif *netif)
{
    printf("[MII] IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
}

/* ------------------------------------------------------------------ */
/*  lwIP TCP callbacks (Core 1)                                        */
/* ------------------------------------------------------------------ */
static err_t mii_tcp_recv(void *arg, struct tcp_pcb *pcb,
                           struct pbuf *p, err_t err)
{
    (void)arg;
    if (p == NULL || err != ERR_OK) {
        printf("[GW-MII] Connection closed by target\n");
        if (p) pbuf_free(p);
        g_session.mii_pcb = NULL;
        multicore_fifo_push_blocking(GW_FIFO_PACK(GW_EVT_MII_CLOSED));
        return ERR_OK;
    }

    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        if (!gw_rb_push(&g_rb_m2w, (const uint8_t *)q->payload, (uint16_t)q->len))
            printf("[GW-MII] m2w ring full -- %u bytes dropped\n", (unsigned)q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void mii_tcp_err(void *arg, err_t err)
{
    (void)arg;
    printf("[GW-MII] TCP error %d\n", (int)err);
    g_session.mii_pcb = NULL;
    multicore_fifo_push_blocking(GW_FIFO_PACK(GW_EVT_MII_ERROR));
}

static err_t mii_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) {
        printf("[GW-MII] Connect failed: %d\n", (int)err);
        g_session.mii_pcb = NULL;
        multicore_fifo_push_blocking(GW_FIFO_PACK(GW_EVT_MII_ERROR));
        return err;
    }

    printf("[GW-MII] Connected to %s:%d\n", GW_MII_TARGET_IP, GW_MII_TARGET_PORT);

    tcp_recv(pcb, mii_tcp_recv);
    tcp_err(pcb,  mii_tcp_err);
    g_session.mii_pcb = pcb;
    multicore_fifo_push_blocking(GW_FIFO_PACK(GW_EVT_MII_CONNECTED));
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  Core 1: MII poll + gateway forward                                 */
/* ------------------------------------------------------------------ */
static c1_state_t g_c1_state = C1_STATE_IDLE;

static void c1_handle_fifo_event(uint32_t word)
{
    gw_event_t evt = GW_FIFO_UNPACK(word);
    switch (evt) {
        case GW_EVT_W5500_CONNECTED:
            if (g_c1_state != C1_STATE_IDLE) break;
            printf("[GW-C1] W5500 connected -- connecting MII to %s:%d\n",
                   GW_MII_TARGET_IP, GW_MII_TARGET_PORT);
            {
                struct tcp_pcb *pcb = tcp_new();
                if (!pcb) {
                    multicore_fifo_push_blocking(GW_FIFO_PACK(GW_EVT_MII_ERROR));
                    break;
                }
                ip4_addr_t target;
                ip4addr_aton(GW_MII_TARGET_IP, &target);
                err_t err = tcp_connect(pcb, &target, GW_MII_TARGET_PORT,
                                        mii_tcp_connected);
                if (err != ERR_OK) {
                    tcp_abort(pcb);
                    multicore_fifo_push_blocking(GW_FIFO_PACK(GW_EVT_MII_ERROR));
                    break;
                }
                g_c1_state = C1_STATE_CONNECTING;
            }
            break;

        case GW_EVT_W5500_CLOSED:
            printf("[GW-C1] W5500 closed -- closing MII side\n");
            if (g_session.mii_pcb) {
                tcp_recv(g_session.mii_pcb, NULL);
                tcp_close(g_session.mii_pcb);
                g_session.mii_pcb = NULL;
            }
            g_c1_state = C1_STATE_IDLE;
            break;

        default:
            break;
    }
}

static void c1_drain_w2m(void)
{
    if (!g_session.mii_pcb) return;
    static uint8_t tmp[GW_RB_SLOT_SIZE];
    uint16_t len;
    while (gw_rb_pop(&g_rb_w2m, tmp, &len)) {
        if (tcp_sndbuf(g_session.mii_pcb) < len) {
            printf("[GW-C1] lwIP sndbuf too small -- chunk dropped\n");
            continue;
        }
        if (tcp_write(g_session.mii_pcb, tmp, len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            printf("[GW-C1] tcp_write() failed\n");
            break;
        }
    }
    tcp_output(g_session.mii_pcb);
}

static void core1_entry(void)
{
    while (1) {
        netif_rmii_ethernet_poll();
        sys_check_timeouts();

        if (multicore_fifo_rvalid())
            c1_handle_fifo_event(multicore_fifo_pop_blocking());

        if (g_c1_state == C1_STATE_CONNECTING && g_session.mii_pcb != NULL)
            g_c1_state = C1_STATE_ESTABLISHED;
        if (g_c1_state == C1_STATE_ESTABLISHED && g_session.mii_pcb == NULL)
            g_c1_state = C1_STATE_IDLE;

        if (g_c1_state == C1_STATE_ESTABLISHED)
            c1_drain_w2m();
    }
}

/* ------------------------------------------------------------------ */
/*  Core 0: W5500 gateway loop                                         */
/* ------------------------------------------------------------------ */
static void c0_handle_fifo_event(uint32_t word)
{
    gw_event_t evt = GW_FIFO_UNPACK(word);
    switch (evt) {
        case GW_EVT_MII_CONNECTED:
            printf("[GW-C0] MII connected -- gateway ESTABLISHED\n");
            g_session.state = GW_STATE_ESTABLISHED;
            break;

        case GW_EVT_MII_CLOSED:
        case GW_EVT_MII_ERROR:
            printf("[GW-C0] MII side closed/error -- disconnecting W5500\n");
            g_session.state = GW_STATE_CLOSING;
            break;

        default:
            break;
    }
}

static void c0_drain_m2w(void)
{
    uint16_t len;
    while (gw_rb_pop(&g_rb_m2w, g_w5500_rxbuf, &len)) {
        uint16_t sent = 0;
        while (sent < len) {
            int32_t ret = send(g_session.w5500_sn,
                               g_w5500_rxbuf + sent, len - sent);
            if (ret < 0) {
                printf("[GW-C0] W5500 send error %d\n", (int)ret);
                g_session.state = GW_STATE_CLOSING;
                return;
            }
            sent += (uint16_t)ret;
        }
    }
}

static void core0_gateway_loop(void)
{
    while (1) {
        if (multicore_fifo_rvalid())
            c0_handle_fifo_event(multicore_fifo_pop_blocking());

        uint8_t sr = getSn_SR(g_session.w5500_sn);

        switch (g_session.state) {

            case GW_STATE_IDLE:
                gw_rb_init(&g_rb_w2m);
                gw_rb_init(&g_rb_m2w);
                switch (sr) {
                    case SOCK_CLOSED:
                        socket(g_session.w5500_sn, Sn_MR_TCP,
                               GW_W5500_LISTEN_PORT, 0x00);
                        break;
                    case SOCK_INIT:
                        listen(g_session.w5500_sn);
                        printf("[GW-C0] Listening on W5500 port %d\n",
                               GW_W5500_LISTEN_PORT);
                        break;
                    case SOCK_ESTABLISHED:
                        if (getSn_IR(g_session.w5500_sn) & Sn_IR_CON) {
                            setSn_IR(g_session.w5500_sn, Sn_IR_CON);
                            uint8_t dip[4];
                            getSn_DIPR(g_session.w5500_sn, dip);
                            uint16_t dport = getSn_DPORT(g_session.w5500_sn);
                            printf("[GW-C0] W5500 accepted %d.%d.%d.%d:%d\n",
                                   dip[0], dip[1], dip[2], dip[3], dport);
                        }
                        g_session.state = GW_STATE_W5500_CONN;
                        multicore_fifo_push_blocking(
                            GW_FIFO_PACK(GW_EVT_W5500_CONNECTED));
                        break;
                    default:
                        break;
                }
                break;

            case GW_STATE_W5500_CONN:
                switch (sr) {
                    case SOCK_ESTABLISHED: {
                        uint16_t avail = getSn_RX_RSR(g_session.w5500_sn);
                        if (avail > 0) {
                            if (avail > GW_RB_SLOT_SIZE) avail = GW_RB_SLOT_SIZE;
                            int32_t n = recv(g_session.w5500_sn, g_w5500_rxbuf, avail);
                            if (n > 0)
                                gw_rb_push(&g_rb_w2m, g_w5500_rxbuf, (uint16_t)n);
                        }
                        break;
                    }
                    case SOCK_CLOSE_WAIT:
                        disconnect(g_session.w5500_sn);
                        multicore_fifo_push_blocking(
                            GW_FIFO_PACK(GW_EVT_W5500_CLOSED));
                        g_session.state = GW_STATE_IDLE;
                        break;
                    default:
                        break;
                }
                break;

            case GW_STATE_ESTABLISHED:
                switch (sr) {
                    case SOCK_ESTABLISHED: {
                        uint16_t avail = getSn_RX_RSR(g_session.w5500_sn);
                        if (avail > 0) {
                            if (avail > GW_RB_SLOT_SIZE) avail = GW_RB_SLOT_SIZE;
                            int32_t n = recv(g_session.w5500_sn, g_w5500_rxbuf, avail);
                            if (n > 0) {
                                if (!gw_rb_push(&g_rb_w2m, g_w5500_rxbuf, (uint16_t)n))
                                    printf("[GW-C0] w2m ring full -- %d bytes dropped\n", (int)n);
                            }
                        }
                        c0_drain_m2w();
                        break;
                    }
                    case SOCK_CLOSE_WAIT:
                        printf("[GW-C0] W5500 CLOSE_WAIT -- closing session\n");
                        disconnect(g_session.w5500_sn);
                        multicore_fifo_push_blocking(
                            GW_FIFO_PACK(GW_EVT_W5500_CLOSED));
                        g_session.state = GW_STATE_CLOSING;
                        break;
                    default:
                        break;
                }
                break;

            case GW_STATE_CLOSING:
                switch (sr) {
                    case SOCK_CLOSE_WAIT:
                        disconnect(g_session.w5500_sn);
                        break;
                    case SOCK_CLOSED:
                        printf("[GW-C0] Session closed -- back to IDLE\n");
                        g_session.state = GW_STATE_IDLE;
                        break;
                    default:
                        break;
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    printf("============================================================\n");
    printf("  W55RP20_EVB_PICO Dual-Stack TCP Gateway\n");
    printf("  W5500  (SPE)   : 192.168.11.13:%d  [pio1]\n",
           GW_W5500_LISTEN_PORT);
    printf("  MII/lwIP (RJ45): %s -> %s:%d  [pio0]\n",
           MII_IP_ADDR, GW_MII_TARGET_IP, GW_MII_TARGET_PORT);
    printf("============================================================\n");

    memset(&g_session, 0, sizeof(g_session));
    g_session.state    = GW_STATE_IDLE;
    g_session.w5500_sn = GW_W5500_SOCKET;
    gw_rb_init(&g_rb_w2m);
    gw_rb_init(&g_rb_m2w);

    printf("[W5500] Initializing...\n");
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();
    network_initialize(g_net_info);
    print_network_information(g_net_info);
    printf("[W5500] Ready.\n");

    struct netif_rmii_ethernet_config mii_config = {
        .pio            = pio0,
        .pio_sm_start   = 0,
        .rx_pin_start   = 10,
        .tx_pin_start   = 4,
        .tx_clk_pin     = 9,
        .mdio_pin_start = 2,
        .mac_addr       = NULL,
    };

    lwip_init();

    ip4_addr_t ip, mask, gw;
    ip4addr_aton(MII_IP_ADDR, &ip);
    ip4addr_aton(MII_NETMASK, &mask);
    ip4addr_aton(MII_GATEWAY, &gw);

    netif_rmii_ethernet_init(&g_mii_netif, &mii_config);

    YT8111_init();

    netif_set_addr(&g_mii_netif, &ip, &mask, &gw);
    netif_set_link_callback(&g_mii_netif,   mii_link_callback);
    netif_set_status_callback(&g_mii_netif, mii_status_callback);
    netif_set_default(&g_mii_netif);
    netif_set_up(&g_mii_netif);

    printf("[GW] Init complete -- launching Core 1\n");

    multicore_launch_core1(core1_entry);

    core0_gateway_loop();

    return 0;
}
