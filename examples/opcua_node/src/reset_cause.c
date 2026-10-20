/**
 * @file    reset_cause.c
 * @brief   Reset cause at boot, unplanned-reset counter in settings.
 *
 * See reset_cause.h for what each cause can and cannot mean.
 */

#include "reset_cause.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#if PICO_RP2350
#include "hardware/structs/powman.h"
#else
#include "hardware/structs/vreg_and_chip_reset.h"
#endif

static reset_cause_t s_cause = RESET_CAUSE_UNKNOWN;
static uint32_t s_raw;

static reset_cause_t classify(void) {
    /* The SDK writes a magic value to a watchdog scratch register in
     * watchdog_enable() and clears it in watchdog_reboot(). It survives a
     * watchdog reset, so it tells our stalled loop apart from a reboot that
     * was asked for (bootrom after a UF2 copy, picotool, watchdog_reboot). */
    bool our_watchdog = watchdog_enable_caused_reboot();

    /* Checked first, on both parts. The SDK's watchdog resets through the PSM
     * (psm_hw->wdsel, hardware_watchdog/watchdog.c), below the power manager,
     * so on the RP2350 it leaves POWMAN CHIP_RESET untouched: after WDTEST
     * that register still said RUN_LOW from an earlier button reset (measured
     * 2026-10-20, raw 0x00040000). The watchdog's own REASON register is the
     * record, and a chip-level reset clears it. */
    if(watchdog_hw->reason) {
        s_raw = watchdog_hw->reason;
        return our_watchdog ? RESET_CAUSE_WATCHDOG : RESET_CAUSE_SOFTWARE;
    }

#if PICO_RP2350
    uint32_t r = powman_hw->chip_reset;
    s_raw = r;

    /* Only a watchdog configured to reset the power manager lands here. */
    if(r & (POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_RSM_BITS |
            POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS |
            POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS |
            POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS))
        return our_watchdog ? RESET_CAUSE_WATCHDOG : RESET_CAUSE_SOFTWARE;
    if(r & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)
        return RESET_CAUSE_GLITCH;
    if(r & POWMAN_CHIP_RESET_HAD_BOR_BITS)
        return RESET_CAUSE_BROWNOUT;
    if(r & POWMAN_CHIP_RESET_HAD_POR_BITS)
        return RESET_CAUSE_POWER_ON;
    if(r & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS)
        return RESET_CAUSE_RUN_PIN;
    if(r & (POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS |
            POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS |
            POWMAN_CHIP_RESET_HAD_RESCUE_BITS))
        return RESET_CAUSE_DEBUGGER;
    return RESET_CAUSE_UNKNOWN;
#else
    /* RP2040: the chip-reset register knows only power-on, RUN and debugger;
     * a watchdog reset leaves it untouched and has its own flag. There is no
     * brown-out or glitch report on this part. */
    uint32_t r = vreg_and_chip_reset_hw->chip_reset;
    s_raw = r;

    if(r & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS)
        return RESET_CAUSE_POWER_ON;
    if(r & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS)
        return RESET_CAUSE_RUN_PIN;
    if(r & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS)
        return RESET_CAUSE_DEBUGGER;
    return RESET_CAUSE_UNKNOWN;
#endif
}

static bool is_unplanned(reset_cause_t c) {
    return c == RESET_CAUSE_WATCHDOG || c == RESET_CAUSE_BROWNOUT ||
           c == RESET_CAUSE_GLITCH;
}

const char *reset_cause_name(reset_cause_t c) {
    switch(c) {
    case RESET_CAUSE_POWER_ON: return "power";
    case RESET_CAUSE_WATCHDOG: return "watchdog";
    case RESET_CAUSE_SOFTWARE: return "user";
    case RESET_CAUSE_BROWNOUT: return "brownout";
    case RESET_CAUSE_GLITCH:   return "glitch";
    case RESET_CAUSE_RUN_PIN:  return "run-pin";
    case RESET_CAUSE_DEBUGGER: return "debugger";
    default:                   return "unknown";
    }
}

reset_cause_t reset_cause_this_boot(void) {
    return s_cause;
}

void reset_cause_record(opcua_settings_t *cfg) {
    s_cause = classify();

    if(is_unplanned(s_cause)) {
        /* Written only for these, so ordinary power cycling never erases the
         * settings sector. The save runs before the network and server start,
         * where a short interrupts-off erase disturbs nothing. */
        cfg->reboot_count++;
        cfg->reboot_reason = (uint8_t)s_cause;
        opcua_settings_save(cfg);
    }

    printf("reset cause: %s%s (%s 0x%08lx)  unplanned resets: %lu, last: %s\r\n",
           reset_cause_name(s_cause),
           is_unplanned(s_cause) ? " - UNPLANNED" : "",
           (s_cause == RESET_CAUSE_WATCHDOG || s_cause == RESET_CAUSE_SOFTWARE)
               ? "wd_reason" : "chip_reset",
           (unsigned long)s_raw, (unsigned long)cfg->reboot_count,
           cfg->reboot_count ? reset_cause_name((reset_cause_t)cfg->reboot_reason)
                             : "-");
}
