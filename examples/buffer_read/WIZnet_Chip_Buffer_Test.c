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
int main() 
{
    // 변수 정의
    uint8_t WRITE_DATA_PATTERN1[100] = {0,};
    uint8_t READ_DATA_1[100] = {0,};
    uint8_t WRITE_DATA_PATTERN2[100] = {0,};
    uint8_t READ_DATA_2[100] = {0,};
    
    int match_cnt_1 = 0;
    int unmatch_cnt_1 = 0;
    int match_cnt_2 = 0;
    int unmatch_cnt_2 = 0;
    
    uint16_t tx_ptr = 0;
    uint32_t tx_addrsel = 0;
    uint8_t write_value = 0xFF;
    int i = 0;
    
    // 초기화
    // set_clock_khz();
    stdio_init_all();
    sleep_ms(3000);
    
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    
    sleep_ms(35);
    
    // TX 버퍼 설정 (소켓 1번)

    sleep_ms(35);
    printf("#%d socket TX buf Size : %dKB\r\n", 1, getSn_TXBUF_SIZE(1));
    sleep_ms(35);
    
    
    /* ================= WRITE TX BUFFER ================= */
    
    ///////////////////////////////////////////////////////////
    // (1) 0~99번지 패턴1 Write (0xFF 감소)
    ///////////////////////////////////////////////////////////
    printf("\r\n========== Write Pattern 1 (0~99) ==========\r\n");
    
    tx_ptr = getSn_TX_WR(1);
    write_value = 0xFF;
    
    for(i = 0; i < 100; i++) {
        tx_addrsel = ((uint32_t)tx_ptr << 8) | WIZCHIP_TXBUF_BLOCK(1);
        WRITE_DATA_PATTERN1[i] = write_value;
        WIZCHIP_WRITE(tx_addrsel, WRITE_DATA_PATTERN1[i]);
        
        if(write_value >= 0xFF) 
            write_value = 0x00;
        write_value++;
        printf("TX loaded (HW view): %d bytes\r\n", getSn_TX_FSR(1));
        
        tx_ptr = tx_ptr + 1;
    }
    
    setSn_TX_WR(1, tx_ptr);
    printf("Pattern 1 Write Complete\r\n");
    sleep_ms(50);
    
    
    /* ================= READ TX BUFFER ================= */
    
    ///////////////////////////////////////////////////////////
    // (2) 0~99번지 Read 및 패턴1과 비교
    ///////////////////////////////////////////////////////////
    printf("\r\n========== Read & Compare Pattern 1 (0~99) ==========\r\n");
    
    tx_ptr = getSn_TX_WR(1);
    
    for(i = 0; i < 100; i++) {
        tx_addrsel = ((uint32_t)tx_ptr << 8) | WIZCHIP_TXBUF_BLOCK(1);
        READ_DATA_1[i] = WIZCHIP_READ(tx_addrsel);
        tx_ptr = tx_ptr + 1;
    }
    
    for(i = 0; i < 100; i++) {
        if(READ_DATA_1[i] != WRITE_DATA_PATTERN1[i]) {
            unmatch_cnt_1++;
            printf("[UNMATCH] Index:%d Write:0x%02X Read:0x%02X\r\n", 
                   i, WRITE_DATA_PATTERN1[i], READ_DATA_1[i]);
        } else {
            match_cnt_1++;
        }
    }
    
    printf("\r\n[Pattern 1 Result] Match:%d / Unmatch:%d\r\n", match_cnt_1, unmatch_cnt_1);
    sleep_ms(50);
    
    
    // /* ================= WRITE TX BUFFER ================= */
    
    // ///////////////////////////////////////////////////////////
    // // (3) 0~99번지 패턴2 Write (0x00 증가)
    // ///////////////////////////////////////////////////////////
    // printf("\r\n========== Write Pattern 2 (0~99) ==========\r\n");
    
    // tx_ptr = getSn_TX_WR(1) - 100;
    // write_value = 0x00;
    
    // for(i = 0; i < 100; i++) {
    //     tx_addrsel = ((uint32_t)tx_ptr << 8) + WIZCHIP_TXBUF_BLOCK(1);
    //     WRITE_DATA_PATTERN2[i] = write_value;
    //     WIZCHIP_WRITE(tx_addrsel, WRITE_DATA_PATTERN2[i]);
        
    //     if(write_value >= 0xFF)
    //         write_value = 0x00;
    //     write_value++;
    //     tx_ptr = tx_ptr + 1;
    // }
    
    // setSn_TX_WR(1, tx_ptr);
    // printf("Pattern 2 Write Complete\r\n");
    // sleep_ms(50);
    
    
    // /* ================= READ TX BUFFER ================= */
    
    // ///////////////////////////////////////////////////////////
    // // (4) 0~99번지 Read 및 패턴2와 비교
    // ///////////////////////////////////////////////////////////
    // printf("\r\n========== Read & Compare Pattern 2 (0~99) ==========\r\n");
    
    // tx_ptr = getSn_TX_WR(1) - 100;
    
    // for(i = 0; i < 100; i++) {
    //     tx_addrsel = ((uint32_t)tx_ptr << 8) + WIZCHIP_TXBUF_BLOCK(1);
    //     READ_DATA_2[i] = WIZCHIP_READ(tx_addrsel);
    //     tx_ptr = tx_ptr + 1;
    // }
    
    // for(i = 0; i < 100; i++) {
    //     if(READ_DATA_2[i] != WRITE_DATA_PATTERN2[i]) {
    //         unmatch_cnt_2++;
    //         printf("[UNMATCH] Index:%d Write:0x%02X Read:0x%02X\r\n", 
    //                i, WRITE_DATA_PATTERN2[i], READ_DATA_2[i]);
    //     } else {
    //         match_cnt_2++;
    //     }
    // }
    
    // printf("\r\n[Pattern 2 Result] Match:%d / Unmatch:%d\r\n", match_cnt_2, unmatch_cnt_2);
    
    
    ///////////////////////////////////////////////////////////
    // 최종 결과
    ///////////////////////////////////////////////////////////
    printf("\r\n\r\n========== FINAL RESULT ==========\r\n");
    printf("Pattern 1 (0xFF decrement): Match %d / Unmatch %d\r\n", match_cnt_1, unmatch_cnt_1);
    // printf("Pattern 2 (0x00 increment): Match %d / Unmatch %d\r\n", match_cnt_2, unmatch_cnt_2);
    printf("==================================\r\n");
    
    
    while(1) {
        sleep_ms(1000);
    }
    
    return 0;
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