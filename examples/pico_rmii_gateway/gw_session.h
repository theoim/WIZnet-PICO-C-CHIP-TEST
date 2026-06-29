/*
 * gw_session.h -- Gateway session FSM types
 *
 * Session state is owned by Core 0.
 * Core 1 maintains a local mirror state updated on FIFO events.
 * The RP2040 HW FIFO (multicore_fifo_*) is the synchronization channel.
 *
 * State transitions (Core 0):
 *
 *   IDLE -(W5500 accept)-> W5500_CONN
 *     sends GW_EVT_W5500_CONNECTED to Core 1
 *
 *   W5500_CONN -(recv GW_EVT_MII_CONNECTED)-> ESTABLISHED
 *             -(recv GW_EVT_MII_ERROR)-------> IDLE (close W5500)
 *
 *   ESTABLISHED -(W5500 CLOSE_WAIT)--------> CLOSING
 *              -(recv GW_EVT_MII_CLOSED)---> CLOSING
 *
 *   CLOSING -(drain + close)-> IDLE
 */

#pragma once

#include <stdint.h>
#include "lwip/tcp.h"

typedef enum {
    GW_STATE_IDLE        = 0,
    GW_STATE_W5500_CONN  = 1,
    GW_STATE_ESTABLISHED = 2,
    GW_STATE_CLOSING     = 3,
} gw_state_t;

typedef enum {
    C1_STATE_IDLE        = 0,
    C1_STATE_CONNECTING  = 1,
    C1_STATE_ESTABLISHED = 2,
} c1_state_t;

typedef enum {
    GW_EVT_W5500_CONNECTED = 0x01,
    GW_EVT_W5500_CLOSED    = 0x02,
    GW_EVT_MII_CONNECTED   = 0x03,
    GW_EVT_MII_CLOSED      = 0x04,
    GW_EVT_MII_ERROR       = 0x05,
} gw_event_t;

#define GW_FIFO_PACK(evt)    ((uint32_t)(evt))
#define GW_FIFO_UNPACK(word) ((gw_event_t)((word) & 0xFF))

typedef struct {
    volatile gw_state_t state;
    struct tcp_pcb *mii_pcb;
    uint8_t w5500_sn;
} gateway_session_t;
