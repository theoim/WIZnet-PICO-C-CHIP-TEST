#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/watchdog.h"

#include "data_table.h"
#include "opcua_security.h" /* open62541.h: UA_ENABLE_ENCRYPTION for the banner */
#include "opcua_server.h"
#include "opcua_settings.h"
#include "reset_cause.h"
#include "sntp_client.h"
#include "usb_stdio_link.h"
#include "wiznet_network.h"

static opcua_settings_t g_settings;

/* The longest the loop may legitimately go without coming round.
 *
 * The worst known stall is one RSA private-key operation inside a handshake:
 * about 1.7 s on the RP2350 (5 s on the RP2040, which is why that part is not
 * the target). A settings save erases a flash sector with interrupts off for
 * tens of milliseconds. Everything else - sends, closes, SNTP - is bounded or
 * non-blocking. Eight seconds is over four times the RSA case on the target
 * part and under half the hardware maximum (about 16.7 s). */
#ifndef OPCUA_WATCHDOG_MS
#define OPCUA_WATCHDOG_MS 8000u
#endif

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    for(uint32_t i = 0; i < 50u && !stdio_usb_connected(); i++)
        sleep_ms(100);

    opcua_settings_load(&g_settings);
    data_table_init(&g_settings);
    usb_stdio_link_init(&g_settings);

    printf("\r\n=== WIZnet OPC UA Node (Phase A) ===\r\n");

    /* Which image is running.
     *
     * Without this line every flash is followed by inferring from behaviour
     * whether the new build actually took - and an admission-control change
     * that produced identical timings was read as "the logic does not fire"
     * when "the image did not change" was equally consistent with what was on
     * screen. crypto_bench has carried the same banner since the first stale
     * image slipped through there. */
    printf("build %s %s  security=%s\r\n", OPCUA_BUILD_GIT, OPCUA_BUILD_UTC,
#if defined(UA_ENABLE_ENCRYPTION)
           "on"
#else
           "off"
#endif
    );
    /* A reset the watchdog caused must be visible, or a hang that recovers
     * itself every few minutes looks like a healthy board. Unplanned resets
     * are also counted in flash for the RebootCount node. */
    reset_cause_record(&g_settings);
    printf("Device: %s  channels: %u\r\n",
           g_settings.device_name, (unsigned)g_settings.channel_count);
    /* 보드에 따라 MCU 가 다르다 — W55RP20 은 RP2040, EVB-Pico2 계열은 RP2350.
     * 로그가 실제 하드웨어와 어긋나면 어느 보드에서 잰 값인지 나중에 못 가린다. */
#if PICO_RP2040
    printf("USB CDC console active on the RP2040 USB port.\r\n");
#else
    printf("USB CDC console active on the RP2350 USB port.\r\n");
#endif

    wiznet_network_init(&g_settings);
    opcua_server_init(&g_settings);
    sntp_client_init(&g_settings);
    opcua_server_print_status();
    usb_stdio_print_help();

    /* Armed only now: initialisation may be slow, the loop may not.
     * pause_on_debug so a halted debugger does not reset the target. */
    watchdog_enable(OPCUA_WATCHDOG_MS, true);

    while(1) {
        watchdog_update();
        usb_stdio_link_poll();
        wiznet_network_poll();
        sntp_client_poll();
        opcua_server_poll();
        sleep_ms(1);
    }
}
