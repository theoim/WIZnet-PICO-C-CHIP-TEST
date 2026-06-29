/*
 * gw_ringbuf.c -- SPSC fixed-slot ring buffer implementation
 *
 * RP2040 M0+ memory model: stores/loads to SRAM are not guaranteed visible
 * to the other core without a Data Memory Barrier (DMB).
 */

#include "gw_ringbuf.h"
#include <string.h>

#define GW_DMB() __asm volatile ("dmb" ::: "memory")

void gw_rb_init(gw_rb_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
}

bool gw_rb_empty(const gw_rb_t *rb)
{
    GW_DMB();
    return rb->head == rb->tail;
}

bool gw_rb_full(const gw_rb_t *rb)
{
    GW_DMB();
    return (rb->head - rb->tail) >= GW_RB_SLOTS;
}

bool gw_rb_push(gw_rb_t *rb, const uint8_t *data, uint16_t len)
{
    GW_DMB();
    if ((rb->head - rb->tail) >= GW_RB_SLOTS)
        return false;

    if (len > GW_RB_SLOT_SIZE)
        len = GW_RB_SLOT_SIZE;

    uint32_t slot = rb->head % GW_RB_SLOTS;
    rb->len[slot] = len;
    memcpy(rb->data[slot], data, len);

    GW_DMB();
    rb->head++;
    GW_DMB();
    return true;
}

bool gw_rb_pop(gw_rb_t *rb, uint8_t *out, uint16_t *out_len)
{
    GW_DMB();
    if (rb->head == rb->tail)
        return false;

    uint32_t slot = rb->tail % GW_RB_SLOTS;
    uint16_t len = rb->len[slot];
    GW_DMB();
    memcpy(out, rb->data[slot], len);
    *out_len = len;

    GW_DMB();
    rb->tail++;
    GW_DMB();
    return true;
}
