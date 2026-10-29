// SPDX-License-Identifier: GPL-3.0-or-later
// WebAssembly face of the shared reassembler (common/vpkt.c + zfex). The GUI
// writes each received OTA packet into olv_in_buf(), calls olv_rx_push(),
// then drains recovered frames with olv_frame_*(). No JS imports needed.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vpkt.h"

#define EXPORT __attribute__((visibility("default")))
#define OUTQ 8

struct out_frame {
    uint32_t len;
    struct vpkt_frame_info info;
    uint8_t data[VPKT_MAX_FRAME];
};

static struct vpkt_rx *rx;
static struct out_frame *q;
static unsigned q_head, q_count;
static uint32_t q_dropped;
static uint8_t in_buf[512];

static void on_frame(void *ctx, const uint8_t *au, size_t len, const struct vpkt_frame_info *info) {
    (void)ctx;
    if (q_count == OUTQ) { // GUI fell behind: drop the oldest
        q_head = (q_head + 1) % OUTQ;
        q_count--;
        q_dropped++;
    }
    struct out_frame *f = &q[(q_head + q_count) % OUTQ];
    f->len = (uint32_t)len;
    f->info = *info;
    if (len)
        memcpy(f->data, au, len);
    q_count++;
}

EXPORT int olv_rx_init(uint32_t link_id, uint32_t max_wait_frames, uint32_t timeout_us) {
    if (!rx)
        rx = malloc(sizeof(*rx));
    if (!q)
        q = malloc(sizeof(*q) * OUTQ);
    if (!rx || !q)
        return -1;
    struct vpkt_rx_cfg c = {.link_id = (uint16_t)link_id,
                            .max_wait_frames = (uint8_t)max_wait_frames,
                            .timeout_us = timeout_us};
    vpkt_rx_init(rx, &c, on_frame, NULL);
    q_head = q_count = 0;
    return 0;
}

EXPORT uint8_t *olv_in_buf(void) {
    return in_buf;
}

EXPORT int olv_rx_push(uint32_t len, uint32_t now_us) {
    if (len > sizeof(in_buf))
        return VPKT_BAD;
    return (int)vpkt_rx_push(rx, in_buf, len, now_us);
}

EXPORT void olv_rx_tick(uint32_t now_us) {
    vpkt_rx_tick(rx, now_us);
}

EXPORT void olv_rx_flush(void) {
    vpkt_rx_flush(rx);
}

EXPORT uint32_t olv_frame_count(void) {
    return q_count;
}

EXPORT const uint8_t *olv_frame_data(void) {
    return q_count ? q[q_head].data : NULL;
}

EXPORT uint32_t olv_frame_len(void) {
    return q_count ? q[q_head].len : 0;
}

// frame u16 | flags u8 (bit0 lost, bit1 complete, bit2 idr) | blocks_ok u8 ... packed:
// bits 0-15 frame, 16-18 flags, 24-31 blocks_ok; olv_frame_blocks() for the total.
EXPORT uint32_t olv_frame_info(void) {
    if (!q_count)
        return 0;
    const struct vpkt_frame_info *i = &q[q_head].info;
    uint32_t flags = (i->lost ? 1u : 0u) | (i->complete ? 2u : 0u) | (i->has_idr ? 4u : 0u);
    return i->frame | flags << 16 | (uint32_t)i->blocks_ok << 24;
}

EXPORT uint32_t olv_frame_blocks(void) {
    return q_count ? q[q_head].info.blocks : 0;
}

EXPORT void olv_frame_pop(void) {
    if (q_count) {
        q_head = (q_head + 1) % OUTQ;
        q_count--;
    }
}

// struct vpkt_rx_stats as 12 u32 (pkts, new, dups, late, bad, blocks_ok,
// blocks_lost, frames_complete, frames_partial, frames_lost, restarts, q_dropped).
EXPORT const uint32_t *olv_stats(void) {
    static uint32_t s[12];
    memcpy(s, &rx->st, sizeof(rx->st));
    s[11] = q_dropped;
    return s;
}
