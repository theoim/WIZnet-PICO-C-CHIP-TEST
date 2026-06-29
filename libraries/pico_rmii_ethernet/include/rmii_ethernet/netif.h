/*
 * Copyright (c) 2021 Sandeep Mistry
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_MII_ETHERNET_NETIF_H_
#define _PICO_MII_ETHERNET_NETIF_H_

#include "hardware/pio.h"

#include "lwip/netif.h"

/*
 * MII pin layout (W55RP20_EVB_PICO default):
 *
 *   rx_pin_start + 0 = RXD0    (GPIO 10)
 *   rx_pin_start + 1 = RXD1    (GPIO 11)
 *   rx_pin_start + 2 = RXD2    (GPIO 12)
 *   rx_pin_start + 3 = RXD3    (GPIO 13)
 *   rx_pin_start + 4 = RX-DV   (GPIO 14)  <- wait pin 4; order matters
 *   rx_pin_start + 5 = RX-CLK  (GPIO 15)  <- wait pin 5; order matters
 *
 *   tx_pin_start + 0 = TXD0    (GPIO  4)
 *   tx_pin_start + 1 = TXD1    (GPIO  5)
 *   tx_pin_start + 2 = TXD2    (GPIO  6)
 *   tx_pin_start + 3 = TXD3    (GPIO  7)
 *   tx_pin_start + 4 = TX-EN   (GPIO  8)
 *   tx_clk_pin       = TX-CLK  (GPIO  9)
 *
 *   mdio_pin_start + 0 = MDIO  (GPIO  2)
 *   mdio_pin_start + 1 = MDC   (GPIO  3)
 */
struct netif_rmii_ethernet_config {
    PIO  pio;
    uint pio_sm_start;   /* uses 2 consecutive state machines */
    uint rx_pin_start;   /* 6 consecutive pins: RXD0-3, RX-DV, RX-CLK */
    uint tx_pin_start;   /* 5 consecutive pins: TXD0-3, TX-EN */
    uint tx_clk_pin;     /* TX-CLK input from PHY */
    uint mdio_pin_start; /* 2 pins: MDIO, MDC */
    uint8_t *mac_addr;   /* 6 bytes, or NULL to auto-generate from flash unique ID */
};

#define NETIF_RMII_ETHERNET_DEFAULT_CONFIG() { \
    .pio            = pio0, \
    .pio_sm_start   = 0,    \
    .rx_pin_start   = 0,    \
    .tx_pin_start   = 6,    \
    .tx_clk_pin     = 11,   \
    .mdio_pin_start = 12,   \
    .mac_addr       = NULL  \
}

err_t netif_rmii_ethernet_init(struct netif *netif, struct netif_rmii_ethernet_config *config);

void netif_rmii_ethernet_mdio_write(int addr, int reg, int val);

int netif_rmii_ethernet_get_phy_address(void);

void netif_rmii_ethernet_poll();

void netif_rmii_ethernet_loop();

#endif
