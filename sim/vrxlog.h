// SPDX-License-Identifier: GPL-3.0-or-later
// Packet capture of one receiver, for replay to the GUI (sim/fake_vrx).
//   file:   "OLVL" | version u32 (1)
//   record: t_us u64 (capture time) | len u16 | vrx_rx_packet bytes[len]
#ifndef OLV_SIM_VRXLOG_H
#define OLV_SIM_VRXLOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "olv_util.h"

static inline void vrxlog_write_header(FILE *f) {
    uint8_t b[8] = {'O', 'L', 'V', 'L', 1, 0, 0, 0};
    fwrite(b, 1, sizeof(b), f);
}

static inline void vrxlog_write(FILE *f, uint64_t t_us, const uint8_t *rec, uint16_t len) {
    uint8_t b[10];
    olv_put_u32(b, (uint32_t)t_us);
    olv_put_u32(b + 4, (uint32_t)(t_us >> 32));
    olv_put_u16(b + 8, len);
    fwrite(b, 1, sizeof(b), f);
    fwrite(rec, 1, len, f);
}

static inline bool vrxlog_check_header(FILE *f) {
    uint8_t b[8];
    return fread(b, 1, 8, f) == 8 && !memcmp(b, "OLVL", 4) && olv_get_u32(b + 4) == 1;
}

// Returns false at end of file. rec must hold 512 bytes.
static inline bool vrxlog_read(FILE *f, uint64_t *t_us, uint8_t *rec, uint16_t *len) {
    uint8_t b[10];
    if (fread(b, 1, 10, f) != 10)
        return false;
    *t_us = olv_get_u32(b) | ((uint64_t)olv_get_u32(b + 4) << 32);
    *len = olv_get_u16(b + 8);
    return *len <= 512 && fread(rec, 1, *len, f) == *len;
}

#endif
