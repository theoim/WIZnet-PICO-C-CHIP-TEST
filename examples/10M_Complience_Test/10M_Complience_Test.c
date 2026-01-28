#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "wizchip_qspi_pio.h"
#include "socket.h"
#include "timer.h"
#include "w6300.h"
#include "pico/rand.h"

#define MACRAW_SOCKET       0
#define BUFFER_SIZE         10
#define TEST_FRAME_SIZE     1514
#define PLL_SYS_KHZ         (133 * 1000)

// PHY control constants
#define PHY_MODE            0
#define SPEED               1
#define DUPLEX              2
#define PHY_INIT            0

#define PACKET_ALL_0        0
#define PACKET_ALL_1        1
#define PACKET_TOGGLE       2
#define PACKET_RANDOM       3


const char *status_labels[] = { "Mode", "Speed", "Duplex" };
const char *packet_labels[] = { "ALL 0", "ALL 1", "ALL Toggle", "RANDOM" };

// PHY config values
uint8_t mode_val   = 0;  // 0: AUTO, 1: MANUAL
uint8_t speed_val  = 100;
uint8_t duplex_val = 1;  // 1: FULL, 0: HALF


extern wiznet_spi_handle_t spi_handle;

static void set_clock_khz(void);
static void send_test_frame(int packet_type);
void set_force_mdi_and_link(void);
void complience_test_initialize(void);
void W6300_PHY_CONFIG(uint8_t mode, uint8_t speed, uint8_t duplex);

int main() {
    set_clock_khz();
    stdio_init_all();
    sleep_ms(3000);

    printf("==========================================================\n");
    printf("Compiled @ %s, %s\n", __DATE__, __TIME__);
    printf("==========================================================\n");

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();

    printf("System Ready. Waiting for command...\n");

    char buf[BUFFER_SIZE] = {0};
    int idx = 0;
    int phy_command_status = PHY_MODE;

    while (phy_command_status <= DUPLEX) {
        idx = 0;
        memset(buf, 0, sizeof(buf));

        printf("\r\n*******************\r\n");
        switch (phy_command_status) {
            case PHY_MODE:
                printf("*** SELECT MODE ***\r\n");
                printf("[MANUAL] MA\r\n[AUTO] AU\r\n");
                break;
            case SPEED:
                printf("*** SELECT SPEED ***\r\n");
                printf("[10M] 10\r\n[100M] 100\r\n");
                break;
            case DUPLEX:
                printf("*** SELECT DUPLEX ***\r\n");
                printf("[FULL] FU\r\n[HALF] HA\r\n");
                break;
        }
        printf("*******************\r\n");
        printf("%s SETTING COMMAND: ", status_labels[phy_command_status]);

        while (idx < BUFFER_SIZE - 1) {
            char ch = getchar();
            if (ch == '\r' || ch == '\n') break;
            buf[idx++] = ch;
            printf("%c", ch);
        }
        buf[idx] = '\0';
        printf("\r\n");

        switch (phy_command_status) {
            case PHY_MODE:
                if (strcmp(buf, "MA") == 0) mode_val = PHY_MODE_MANUAL;
                else if (strcmp(buf, "AU") == 0){
                    mode_val = PHY_MODE_AUTONEGO;
                     phy_command_status = DUPLEX + 1;  // skip remaining steps
                }
                    else break;
                phy_command_status++;
                break;

            case SPEED:
                if (strcmp(buf, "10") == 0) speed_val = PHY_SPEED_10;
                else if (strcmp(buf, "100") == 0) speed_val = PHY_SPEED_100;
                else break;
                phy_command_status++;
                break;

            case DUPLEX:
                if (strcmp(buf, "HA") == 0) duplex_val = PHY_DUPLEX_HALF;
                else if (strcmp(buf, "FU") == 0) duplex_val = PHY_DUPLEX_FULL;
                else break;
                phy_command_status++;
                break;
        }
    }

    complience_test_initialize();
    set_force_mdi_and_link();
    wizchip_check();

    while (1) {
        char cmd_buf[2] = {0};
        int cmd_idx = 0;
        printf("\r\n****************************\r\n");
        printf("***** SELECT PACKET TYPE *****\r\n");
        printf("[RANDOM] r  [ALL 0] 0  [ALL 1] 1  [TOGGLE] t\r\n");
        printf("Input: ");

        while (cmd_idx < 1) {
            char ch = getchar();
            if (ch == '\r' || ch == '\n') break;
            cmd_buf[cmd_idx++] = ch;
            printf("%c", ch);
        }
        cmd_buf[1] = '\0';
        printf("\r\n");

        while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT);  // flush stdin

        int select_packet_type = -1;
        if (cmd_buf[0] == 'r') select_packet_type = PACKET_RANDOM;
        else if (cmd_buf[0] == '0') select_packet_type = PACKET_ALL_0;
        else if (cmd_buf[0] == '1') select_packet_type = PACKET_ALL_1;
        else if (cmd_buf[0] == 't') select_packet_type = PACKET_TOGGLE;

        if (select_packet_type != -1) {
            printf("Sending %s packets... (press 'x' to stop)\r\n", packet_labels[select_packet_type]);

            while (1) {
                send_test_frame(select_packet_type);
                if (getchar_timeout_us(0) == 'x') {
                    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT);
                    printf("\r\nStopped sending. Returning to menu.\r\n");
                    break;
                }
            }
        } else {
            printf("Invalid input: '%c'\n", cmd_buf[0]);
        }
    }
}

static void send_test_frame(int packet_type) {
    if (getSn_SR(MACRAW_SOCKET) != SOCK_MACRAW) {
        close(MACRAW_SOCKET);
        if (socket(MACRAW_SOCKET, Sn_MR_MACRAW, 0, 0) != MACRAW_SOCKET) {
            printf("Failed to open MACRAW socket\n");
            return;
        }
        printf("MACRAW socket opened\n");
    }

    uint8_t frame[TEST_FRAME_SIZE];
    for (int i = 0; i < 6; i++) frame[i] = 0xFF; // Destination MAC (Broadcast)

    // Source MAC
    uint8_t src_mac[6] = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56};
    memcpy(frame + 6, src_mac, 6);

    // EtherType
    uint16_t ethertype = 0x0800;
    frame[12] = (ethertype >> 8) & 0xFF;
    frame[13] = ethertype & 0xFF;

    int payload_len = 1500;
    for (int i = 14; i < 14 + payload_len; i++) {
        switch (packet_type) {
            case PACKET_RANDOM:  frame[i] = get_rand_32() & 0xFF; break;
            case PACKET_ALL_0:   frame[i] = 0x00; break;
            case PACKET_ALL_1:   frame[i] = 0xFF; break;
            case PACKET_TOGGLE:  frame[i] = 0x01; break;
        }
    }

    int total_len = 14 + payload_len;
    wiz_send_data(MACRAW_SOCKET, frame, total_len);
    setSn_CR(MACRAW_SOCKET, Sn_CR_SEND);
    while (getSn_CR(MACRAW_SOCKET));  // wait for complete

    printf("MACRAW %s packet sent (%d bytes, EtherType=0x%04X)\n",
           packet_labels[packet_type], total_len, ethertype);
    printf("dest MAC : ");
    for(int i = 0; i < 6; i++){
    printf("0x%02x ",frame[i]);
    }
    printf("\r\n");

    printf("src MAC : ");
    for(int i = 6; i < 12; i++){
    printf("0x%02x ",frame[i]);
    }
    printf("\r\n");

    printf("ether type : ");
    for(int i = 12; i < 14; i++){
    printf("0x%02x ",frame[i]);
    }
    printf("\r\n");

    printf("payload : ");
    for(int i = 14; i < 24; i++){
    printf("0x%02x ",frame[i]);
    }
    printf("\r\n"); 
}

static void set_clock_khz(void) {
    set_sys_clock_khz(PLL_SYS_KHZ, true);
    clock_configure(clk_peri, 0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        PLL_SYS_KHZ * 1000,
        PLL_SYS_KHZ * 1000);
}

void set_force_mdi_and_link(void) {
    uint16_t val;

    wiz_mdio_write(0x16, 0x4706);  // Force MDI
    val = wiz_mdio_read(0x16);
    printf("Force MDI %s: 0x%04X\n", (val == 0x4706) ? "Success" : "Fail", val);

    wiz_mdio_write(0x1D, 0x8037);  // Force Link
    val = wiz_mdio_read(0x1D);
    printf("Force Link %s: 0x%04X\n", (val == 0x8037) ? "Success" : "Fail", val);
}

void W6300_PHY_CONFIG(uint8_t mode, uint8_t speed, uint8_t duplex)
{
    uint8_t phycr0 = 0;

    wiz_PhyConf phyconf;
    phyconf.mode = mode;
    if(mode == PHY_MODE_MANUAL){
        phyconf.speed = speed;
        phyconf.duplex = duplex;
    }
    printf("before PHY Config Applied: CR0=0x%02X\r\n", phycr0);
    wizphy_setphyconf(&phyconf);

    printf("PHY Config Applied: CR0=0x%02X\r\n", phycr0);
}


void complience_test_initialize(void)
{
    wiz_PhyConf phyconf;

    (*spi_handle)->frame_end();
    reg_wizchip_qspi_cbfunc((*spi_handle)->read_byte, (*spi_handle)->write_byte);
    reg_wizchip_cs_cbfunc((*spi_handle)->frame_start, (*spi_handle)->frame_end);
    /* W5x00, W6x00 initialize */
    uint8_t temp;
    uint8_t memsize[2][8] = {{16, 16, 0, 0, 0, 0, 0, 0}, {16, 16, 0, 0, 0, 0, 0, 0}};

  /* Initialize WIZchip */
  if (ctlwizchip(CW_INIT_WIZCHIP, (void *)memsize) == -1)
  {
      printf("W6x00 initialized fail\n");
      return;
  }

  W6300_PHY_CONFIG(mode_val, speed_val, duplex_val);

  /* Check PHY link status */
  do
  {
      if (ctlwizchip(CW_GET_PHYLINK, (void *)&temp) == -1)
      {
          printf("Unknown PHY link status\n");
          return;
      }
  } while (temp == PHY_LINK_OFF);


  wizphy_getphyconf(&phyconf);
  printf("Link OK of Internal PHY.\r\n");
  /* Display PHY configuration */
  printf("the %d Mbtis speed of Internal PHY.\r\n", phyconf.speed == PHY_SPEED_100 ? 100 : 10);
  printf("The %s Duplex Mode of the Internal PHY.\r\n", phyconf.duplex == PHY_DUPLEX_FULL ? "Full-Duplex" : "Half-Duplex");

}