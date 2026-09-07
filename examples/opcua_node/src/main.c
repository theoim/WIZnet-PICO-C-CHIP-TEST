#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "data_table.h"
#include "opcua_server.h"
#include "opcua_settings.h"
#include "usb_stdio_link.h"
#include "wiznet_network.h"

static opcua_settings_t g_settings;

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    for(uint32_t i = 0; i < 50u && !stdio_usb_connected(); i++)
        sleep_ms(100);

    opcua_settings_load(&g_settings);
    data_table_init(&g_settings);
    usb_stdio_link_init(&g_settings);

    printf("\r\n=== WIZnet OPC UA Node (Phase A) ===\r\n");
    printf("Device: %s  channels: %u\r\n",
           g_settings.device_name, (unsigned)g_settings.channel_count);
    printf("USB CDC console active on the RP2350 USB-C port.\r\n");

    wiznet_network_init(&g_settings);
    opcua_server_init(&g_settings);
    opcua_server_print_status();
    usb_stdio_print_help();

    while(1) {
        usb_stdio_link_poll();
        wiznet_network_poll();
        opcua_server_poll();
        sleep_ms(1);
    }
}
