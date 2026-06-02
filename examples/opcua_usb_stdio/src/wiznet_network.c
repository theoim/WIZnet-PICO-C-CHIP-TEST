#include "wiznet_network.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_qspi_pio.h"
#include "wizchip_spi.h"

#define LINK_POLL_INTERVAL_MS 1000u

static wiz_NetInfo s_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip = {192, 168, 11, 2},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 11, 1},
    .dns = {8, 8, 8, 8},
#if _WIZCHIP_ > W5500
    .lla = {
        0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0x08, 0xdc, 0xff,
        0xfe, 0x57, 0x57, 0x25
    },
    .gua = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },
    .sn6 = {
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },
    .gw6 = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },
    .dns6 = {
        0x20, 0x01, 0x48, 0x60,
        0x48, 0x60, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x88, 0x88
    },
    .ipmode = NETINFO_STATIC_ALL
#else
    .dhcp = NETINFO_STATIC
#endif
};

static WiznetNetworkStatus s_status;
static uint32_t s_last_link_poll_ms;

static const char *state_name(WiznetNetworkState state) {
    switch(state) {
    case WIZNET_NETWORK_STATE_DISABLED:
        return "DISABLED";
    case WIZNET_NETWORK_STATE_INIT:
        return "INIT";
    case WIZNET_NETWORK_STATE_LINK_DOWN:
        return "LINK_DOWN";
    case WIZNET_NETWORK_STATE_LINK_UP:
        return "LINK_UP";
    case WIZNET_NETWORK_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void copy_net_info_to_status(void) {
    memcpy(s_status.mac, s_net_info.mac, sizeof(s_status.mac));
    memcpy(s_status.ip, s_net_info.ip, sizeof(s_status.ip));
    memcpy(s_status.subnet, s_net_info.sn, sizeof(s_status.subnet));
    memcpy(s_status.gateway, s_net_info.gw, sizeof(s_status.gateway));
}

static void update_link_status(bool print_on_change) {
    bool old_link_up = s_status.link_up;
    uint8_t link = wizphy_getphylink();

    s_status.link_up = (link == PHY_LINK_ON);
    s_status.state = s_status.link_up ?
        WIZNET_NETWORK_STATE_LINK_UP : WIZNET_NETWORK_STATE_LINK_DOWN;
    s_status.status_text = s_status.link_up ?
        "WIZnet network configured; PHY link is up" :
        "WIZnet network configured; PHY link is down";

    if(print_on_change && old_link_up != s_status.link_up) {
        printf("[WIZnet] PHY link %s\r\n", s_status.link_up ? "up" : "down");
    }
}

static void print_phy_details(void) {
    wiz_PhyConf phyconf;

    if(!s_status.link_up)
        return;

    wizphy_getphyconf(&phyconf);
    printf("[WIZnet] PHY speed=%sMbps duplex=%s\r\n",
           phyconf.speed == PHY_SPEED_100 ? "100" : "10",
           phyconf.duplex == PHY_DUPLEX_FULL ? "full" : "half");
}

static int wiznet_chip_initialize_nonblocking(void) {
#if _WIZCHIP_ == W6300
    extern wiznet_spi_handle_t spi_handle;
    uint8_t memsize[2][8] = {
        {4, 4, 4, 4, 4, 4, 4, 4},
        {4, 4, 4, 4, 4, 4, 4, 4}
    };

    /*
     * This mirrors the chip-specific path inside wizchip_initialize(), but skips its
     * infinite PHY-link wait so USB CDC commands still work with no LAN cable.
     */
    (*spi_handle)->frame_end();
    reg_wizchip_qspi_cbfunc((*spi_handle)->read_byte, (*spi_handle)->write_byte);
    reg_wizchip_cs_cbfunc((*spi_handle)->frame_start, (*spi_handle)->frame_end);

    if(ctlwizchip(CW_INIT_WIZCHIP, (void *)memsize) == -1) {
        printf("[WIZnet] WIZnet chip initialization failed\r\n");
        return -1;
    }

    return 0;
#else
    wizchip_initialize();
    return 0;
#endif
}

int wiznet_network_init(void) {
    memset(&s_status, 0, sizeof(s_status));
    copy_net_info_to_status();
    s_status.state = WIZNET_NETWORK_STATE_INIT;
    s_status.status_text = "initializing WIZnet network";

    printf("[WIZnet] initializing WIZnet network\r\n");

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    if(wiznet_chip_initialize_nonblocking() != 0) {
        s_status.state = WIZNET_NETWORK_STATE_ERROR;
        s_status.status_text = "WIZnet chip initialization failed";
        wiznet_network_print_status();
        return -1;
    }
    wizchip_check();

    network_initialize(s_net_info);
    print_network_information(s_net_info);

    s_status.initialized = true;
    copy_net_info_to_status();
    update_link_status(false);
    print_phy_details();
    wiznet_network_print_status();

    return 0;
}

void wiznet_network_poll(void) {
    if(!s_status.initialized)
        return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if((uint32_t)(now - s_last_link_poll_ms) < LINK_POLL_INTERVAL_MS)
        return;

    s_last_link_poll_ms = now;
    update_link_status(true);
}

void wiznet_network_get_status(WiznetNetworkStatus *status) {
    if(status)
        *status = s_status;
}

void wiznet_network_print_status(void) {
    printf("[WIZnet] state=%s link=%s ip=%u.%u.%u.%u port=%u\r\n",
           state_name(s_status.state),
           s_status.link_up ? "up" : "down",
           s_status.ip[0], s_status.ip[1],
           s_status.ip[2], s_status.ip[3],
           (unsigned)WIZNET_OPCUA_TCP_PORT);
    printf("[WIZnet] mac=%02X:%02X:%02X:%02X:%02X:%02X gw=%u.%u.%u.%u sn=%u.%u.%u.%u\r\n",
           s_status.mac[0], s_status.mac[1], s_status.mac[2],
           s_status.mac[3], s_status.mac[4], s_status.mac[5],
           s_status.gateway[0], s_status.gateway[1],
           s_status.gateway[2], s_status.gateway[3],
           s_status.subnet[0], s_status.subnet[1],
           s_status.subnet[2], s_status.subnet[3]);
    printf("[WIZnet] %s\r\n",
           s_status.status_text ? s_status.status_text : "(no status)");
}

bool wiznet_network_is_ready(void) {
    return s_status.initialized && s_status.link_up;
}
