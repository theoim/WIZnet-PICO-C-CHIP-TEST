/*
 * Shim for freertos/task.h.
 *
 * The sensor driver spells its delays vTaskDelay(ms / portTICK_PERIOD_MS).
 * With the period defined as 1 that expression is already milliseconds, so the
 * delay maps straight onto sleep_ms.
 *
 * This blocks the caller, unlike the FreeRTOS original which yields. Sensor
 * init is the only place the driver delays, and it runs once at startup before
 * the HTTP server is listening, so nothing is waiting on the core.
 */
#ifndef _CAM_SHIM_TASK_H_
#define _CAM_SHIM_TASK_H_

#include "pico/stdlib.h"

#define portTICK_PERIOD_MS  1u

static inline void vTaskDelay(uint32_t ms)
{
    sleep_ms(ms);
}

#endif /* _CAM_SHIM_TASK_H_ */
