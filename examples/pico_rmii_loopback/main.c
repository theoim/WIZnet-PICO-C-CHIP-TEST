/*
 * W55RP20_EVB_PICO — Dual-Stack TCP Loopback
 *
 * Hardware:
 *   RP2040 + internal W5500 (pio1, GPIO 20-25)
 *   RP2040 PIO MII -> external YT8111 PHY (pio0, GPIO 2-15)
 *
 * MII pin layout (pio0):
 *   GPIO  2 = MDIO
 *   GPIO  3 = MDC
 *   GPIO  4 = TXD0
 *   GPIO  5 = TXD1
 *   GPIO  6 = TXD2
 *   GPIO  7 = TXD3
 *   GPIO  8 = TX-EN
 *   GPIO  9 = TX-CLK  (PHY -> RP2040, 2.5 MHz)
 *   GPIO 10 = RXD0
 *   GPIO 11 = RXD1
 *   GPIO 12 = RXD2
 *   GPIO 13 = RXD3
 *   GPIO 14 = RX-DV
 *   GPIO 15 = RX-CLK  (PHY -> RP2040, 2.5 MHz)
 *
 * W5500 SPI pin layout (pio1):
 *   GPIO 20 = CS
 *   GPIO 21 = SCK
 *   GPIO 22 = MISO
 *   GPIO 23 = MOSI
 *   GPIO 24 = INT
 *   GPIO 25 = RST
 *
 * Network:
 *   MII  stack (lwIP)  : 192.168.11.12 / TCP port 5000
 *   W5500 stack (TOE)  : 192.168.11.13 / TCP port 5000
 *
 * Core 0: W5500 init + MII init + YT8111 init -> W5500 loopback_tcps() loop
 * Core 1: MII ethernet poll (PIO + DMA + lwIP)
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
#include "loopback.h"

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
/*  Network config                                                     */
/* ------------------------------------------------------------------ */
#define MII_IP_ADDR         "192.168.11.12"
#define MII_NETMASK         "255.255.255.0"
#define MII_GATEWAY         "192.168.11.1"
#define MII_TCP_PORT        5000

#define WIZ_TCP_PORT        5000
#define WIZ_SOCKET_NUM      0
#define WIZ_BUF_SIZE        (1024 * 2)

static wiz_NetInfo g_net_info = {
    .mac  = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x99},
    .ip   = {192, 168, 11, 13},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 11, 1},
    .dns  = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC,
};

static uint8_t g_wiz_buf[WIZ_BUF_SIZE];

static struct netif mii_netif;

/* ------------------------------------------------------------------ */
/*  MII link / status callbacks                                        */
/* ------------------------------------------------------------------ */
static void mii_link_callback(struct netif *netif)
{
    printf("[MII] Link %s\n", netif_is_link_up(netif) ? "UP" : "DOWN");
}

static void mii_status_callback(struct netif *netif)
{
    printf("[MII] IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
}

/* ------------------------------------------------------------------ */
/*  MII TCP loopback server                                            */
/* ------------------------------------------------------------------ */
static err_t tcp_loopback_recv(void *arg, struct tcp_pcb *pcb,
                               struct pbuf *p, err_t err)
{
    if (p == NULL) {
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        if (tcp_sndbuf(pcb) < q->len) {
            printf("[MII-TCP] TX buf full, skip chunk\n");
            continue;
        }
        tcp_write(pcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(pcb);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_loopback_err(void *arg, err_t err)
{
    printf("[MII-TCP] error: %d\n", (int)err);
}

static err_t tcp_loopback_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL)
        return ERR_VAL;

    printf("[MII-TCP] client: %s\n", ip4addr_ntoa(&newpcb->remote_ip));

    tcp_recv(newpcb, tcp_loopback_recv);
    tcp_err(newpcb,  tcp_loopback_err);
    return ERR_OK;
}

static void mii_tcp_server_init(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { printf("[MII-TCP] PCB alloc failed\n"); return; }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, MII_TCP_PORT);
    if (err != ERR_OK) {
        printf("[MII-TCP] bind failed: %d\n", (int)err);
        tcp_abort(pcb);
        return;
    }

    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        printf("[MII-TCP] listen failed\n");
        tcp_abort(pcb);
        return;
    }

    tcp_accept(lpcb, tcp_loopback_accept);
    printf("[MII-TCP] server ready -> %s:%d\n", MII_IP_ADDR, MII_TCP_PORT);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    printf("============================================================\n");
    printf("  W55RP20_EVB_PICO Dual-Stack TCP Loopback\n");
    printf("  MII  (lwIP) : %s:%d  [pio0]\n", MII_IP_ADDR, MII_TCP_PORT);
    printf("  W5500 (TOE) : 192.168.11.13:%d  [pio1]\n", WIZ_TCP_PORT);
    printf("============================================================\n");

    /* W5500 init (pio1, GPIO 20-25) */
    printf("[W5500] Initializing...\n");
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();
    network_initialize(g_net_info);
    print_network_information(g_net_info);
    printf("[W5500] Ready. TCP loopback on port %d\n", WIZ_TCP_PORT);

    /* MII pin config */
    struct netif_rmii_ethernet_config mii_config = {
        .pio            = pio0,
        .pio_sm_start   = 0,
        .rx_pin_start   = 10,
        .tx_pin_start   = 4,
        .tx_clk_pin     = 9,
        .mdio_pin_start = 2,
        .mac_addr       = NULL,
    };

    /* lwIP init (NO_SYS) */
    lwip_init();

    /* MII netif register */
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(MII_IP_ADDR, &ip);
    ip4addr_aton(MII_NETMASK, &mask);
    ip4addr_aton(MII_GATEWAY, &gw);

    netif_rmii_ethernet_init(&mii_netif, &mii_config);

    /* YT8111 PHY init */
    YT8111_init();

    netif_set_addr(&mii_netif, &ip, &mask, &gw);
    netif_set_link_callback(&mii_netif,   mii_link_callback);
    netif_set_status_callback(&mii_netif, mii_status_callback);
    netif_set_default(&mii_netif);
    netif_set_up(&mii_netif);

    /* MII TCP loopback server */
    mii_tcp_server_init();

    /* Core 1: MII ethernet poll (pio0) */
    multicore_launch_core1(netif_rmii_ethernet_loop);

    /* Core 0: W5500 TCP loopback (pio1) */
    printf("[Core 0] Ready. W5500 + MII dual TCP loopback running.\n");

    while (1) {
        loopback_tcps(WIZ_SOCKET_NUM, g_wiz_buf, WIZ_TCP_PORT);
    }

    return 0;
}
