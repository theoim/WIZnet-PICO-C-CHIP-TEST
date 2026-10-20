/**
 * @file    reset_cause.h
 * @brief   Why the board last reset, and how many unplanned resets it has had.
 *
 * A soak test that ends with the board running says nothing if it rebooted
 * twice on the way. This reads the chip's reset-cause register once at boot,
 * keeps the result for the diagnostics nodes, and counts unplanned resets in
 * the settings image so the number survives power cycles.
 *
 * What the hardware can and cannot tell apart (RP2350, POWMAN CHIP_RESET):
 *
 *   power-on        POR. Also what a slow supply sag below the POR threshold
 *                   looks like - indistinguishable from switching the board on.
 *   brown-out       BOR, only if the dip crossed the brown-out threshold
 *                   without going below POR, and only while the detector is
 *                   enabled (it is at reset).
 *   supply glitch   GLITCH_DETECT: a short spike the glitch detector caught.
 *   watchdog        not in CHIP_RESET: the SDK watchdog resets through the PSM,
 *                   so it is read from the watchdog's REASON register instead.
 *                   Split in two below: our own watchdog firing (the loop
 *                   stalled) versus a deliberate reboot through the watchdog -
 *                   the bootrom after a UF2 copy, picotool, watchdog_reboot().
 *                   The SDK marks its own watchdog_enable() in a scratch
 *                   register; that mark is what separates them.
 *   RUN pin         reset button or an external supervisor.
 *   debugger        SWD reset, rescue or hazard request.
 *
 * A power problem that stays above the brown-out threshold leaves no mark at
 * all; one that takes the supply away completely reads as power-on.
 */

#ifndef RESET_CAUSE_H
#define RESET_CAUSE_H

#include <stdbool.h>
#include <stdint.h>

#include "opcua_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stored in opcua_settings_t.reboot_reason. 0..2 keep their earlier meaning. */
typedef enum {
    RESET_CAUSE_POWER_ON = 0,
    RESET_CAUSE_WATCHDOG = 1,       /* our watchdog fired: the main loop stalled */
    RESET_CAUSE_SOFTWARE = 2,       /* deliberate reboot via the watchdog        */
    RESET_CAUSE_BROWNOUT = 3,
    RESET_CAUSE_GLITCH   = 4,
    RESET_CAUSE_RUN_PIN  = 5,
    RESET_CAUSE_DEBUGGER = 6,
    RESET_CAUSE_UNKNOWN  = 7,
} reset_cause_t;

/* Reads the cause, prints it, and for an unplanned reset (watchdog,
 * brown-out, glitch) increments cfg->reboot_count, stores the cause in
 * cfg->reboot_reason and saves the settings. Planned resets do not write
 * flash, so power cycling does not wear the settings sector.
 * Call once, after opcua_settings_load(). */
void reset_cause_record(opcua_settings_t *cfg);

/* This boot's cause, for the diagnostics nodes. */
reset_cause_t reset_cause_this_boot(void);

const char *reset_cause_name(reset_cause_t c);

#ifdef __cplusplus
}
#endif

#endif /* RESET_CAUSE_H */
