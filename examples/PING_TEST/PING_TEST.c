/**
    Copyright (c) 2021 WIZnet Co.,Ltd

    SPDX-License-Identifier: BSD-3-Clause
*/

/**
    ----------------------------------------------------------------------------------------------------
    Includes
    ----------------------------------------------------------------------------------------------------
*/
#include <stdio.h>

#include "port_common.h"

#include "wizchip_conf.h"
#include "wizchip_spi.h"

#include "loopback.h"
#include "socket.h"

#include "timer.h"

/**
    ----------------------------------------------------------------------------------------------------
    Macros
    ----------------------------------------------------------------------------------------------------
*/

/* Buffer */
#define ETHERNET_BUF_MAX_SIZE (1024 * 2)

/**
    ----------------------------------------------------------------------------------------------------
    Variables
    ----------------------------------------------------------------------------------------------------
*/
/* Network */
static wiz_NetInfo g_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
    .ip = {192, 168, 11, 12},                     // IP address
    .sn = {255, 255, 255, 0},                    // Subnet Mask
    .gw = {192, 168, 11, 1},                     // Gateway
    .dns = {8, 8, 8, 8},                         // DNS server
#if _WIZCHIP_ > W5500
    .lla = {
        0xfe, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0x08, 0xdc, 0xff,
        0xfe, 0x57, 0x57, 0x25
    },             // Link Local Address
    .gua = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },             // Global Unicast Address
    .sn6 = {
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },             // IPv6 Prefix
    .gw6 = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },             // Gateway IPv6 Address
    .dns6 = {
        0x20, 0x01, 0x48, 0x60,
        0x48, 0x60, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x88, 0x88
    },             // DNS6 server
    .ipmode = NETINFO_STATIC_ALL
#else
    .dhcp = NETINFO_STATIC
#endif
};




/* Timer */
static volatile uint16_t g_msec_cnt = 0;

/**
    ----------------------------------------------------------------------------------------------------
    Functions
    ----------------------------------------------------------------------------------------------------
*/

/* Timer */
static void repeating_timer_callback(void);

/**
    ----------------------------------------------------------------------------------------------------
    Main
    ----------------------------------------------------------------------------------------------------
*/
int main() {
    /* Initialize */
    int retval = 0;
    uint8_t dhcp_retry = 0;
    uint8_t dns_retry = 0;



    stdio_init_all();

    sleep_ms(3000);

    printf("==========================================================\n");
    printf("Compiled @ %s, %s\n", __DATE__, __TIME__);
    printf("==========================================================\n");

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    // wizchip_1ms_timer_initialize(repeating_timer_callback);

    network_initialize(g_net_info);
  
    /* Get network information */
    print_network_information(g_net_info);

    socket(1,Sn_MR_TCP4,5000,0x00);
    listen(1);    

    while (1) {
    static uint8_t ping_started = 0;
    uint8_t slir;

    /* 1. TCP Client 연결 감지 (Socket 1) */
    if (getSn_IR(1) & Sn_IR_CON)
    {
        printf("TCP Client Connected (Socket 1)\n");

        /* CON 인터럽트 클리어 */
        setSn_IRCLR(1, 0x01);

        /* PING 시작 플래그 */
        ping_started = 1;
    }

    /* 2. TCP 연결 이후 SOCKET-less IPv4 PING 반복 */
    if (ping_started)
    {
        /* SOCKET-less Retransmission 설정 */
        setSLRTR(0x03E8);   // 100ms (unit: 100us)
        setSLRCR(5);

        /* SOCKET-less Interrupt Mask (PING4 + TIMEOUT) */
        setSLIMR((1<<5) | (1<<7));

        /* Destination IP = 192.168.11.45 */
        {
            uint8_t dst_ip[4] = {192, 168, 11, 45};
            setSLDIP4R(dst_ip);
        }

        /* PING Sequence / ID */
        static uint16_t ping_seq = 0x03E8;
        setPINGSEQR(ping_seq++);
        setPINGIDR(0x0100);

        /* IPv4 PING Command */
        setSLCR(SLCR_PING4);

        /* Command 완료 대기 */
        while (getSLCR() != 0x00);

        /* 결과 확인 */
        slir = getSLIR();

        if (slir & SLIR_PING4)
        {
            printf("PING Reply received\n");
            setSLIRCLR((1<<5));
        }
        else if (slir & (1<<7))
        {
            printf("PING Timeout\n");
            setSLIRCLR((1<<7));
        }

        sleep_ms(1000);
    }

    }
}

/**
    ----------------------------------------------------------------------------------------------------
    Functions
    ----------------------------------------------------------------------------------------------------
*/

/* Timer */
static void repeating_timer_callback(void) {
    g_msec_cnt++;

    if (g_msec_cnt >= 1000 - 1) {
        g_msec_cnt = 0;
    }
}