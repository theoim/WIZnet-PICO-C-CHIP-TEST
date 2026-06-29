/*
 * gw_ringbuf.h -- SPSC fixed-slot ring buffer for inter-core data forwarding
 *
 * Producer and consumer run on different RP2040 cores.
 * All accesses use DMB barriers to ensure SRAM visibility across the bus.
 *
 * Rules:
 *   - Only ONE producer core may call gw_rb_push().
 *   - Only ONE consumer core may call gw_rb_pop().
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GW_RB_SLOTS      4
#define GW_RB_SLOT_SIZE  1514   /* max Ethernet payload without FCS */

typedef struct {
    uint8_t  data[GW_RB_SLOTS][GW_RB_SLOT_SIZE];
    uint16_t len[GW_RB_SLOTS];
    volatile uint32_t head;
    volatile uint32_t tail;
} gw_rb_t;

void gw_rb_init(gw_rb_t *rb);
bool gw_rb_push(gw_rb_t *rb, const uint8_t *data, uint16_t len);
bool gw_rb_pop(gw_rb_t *rb, uint8_t *out, uint16_t *out_len);
bool gw_rb_empty(const gw_rb_t *rb);
bool gw_rb_full(const gw_rb_t *rb);
