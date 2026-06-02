#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "data_table.h"
#include "opcua_server.h"
#include "usb_stdio_link.h"
#include "wiznet_network.h"

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    for(uint32_t i = 0; i < 50u && !stdio_usb_connected(); i++)
        sleep_ms(100);

    data_table_init();
    usb_stdio_link_init();

    printf("\r\n=== WIZnet OPC UA USB stdio prototype ===\r\n");
    printf("USB CDC input is active on the RP2350 USB-C port.\r\n");
    wiznet_network_init();
    opcua_server_init();  
    opcua_server_print_status();
    usb_stdio_print_help();

    while(1) {
        usb_stdio_link_poll();  
        wiznet_network_poll();
        opcua_server_poll();
        sleep_ms(1);  
    }
}
