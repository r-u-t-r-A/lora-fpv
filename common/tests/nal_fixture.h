// SPDX-License-Identifier: GPL-3.0-or-later
// Builds synthetic H.265 Annex-B access units for tests. NAL bodies are
// pseudo-random bytes with no 00 00 0x sequences (as emulation prevention
// guarantees in real streams).
#ifndef OLV_NAL_FIXTURE_H
#define OLV_NAL_FIXTURE_H

#include "olv_util.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Appends a NAL with a 4-byte start code. For VCL NALs, first sets
// first_slice_segment_in_pic_flag.
static size_t fx_nal(uint8_t *buf, size_t pos, uint8_t type, size_t body, bool first_slice,
                     uint32_t *rng) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    memcpy(buf + pos, sc, 4);
    pos += 4;
    buf[pos++] = (uint8_t)(type << 1);
    buf[pos++] = 1; // nuh_temporal_id_plus1
    for (size_t i = 0; i < body; i++) {
        uint8_t b = (uint8_t)(olv_xorshift32(rng) | 0x10);
        if (i == 0 && type < 32)
            b = first_slice ? (uint8_t)(b | 0x80) : (uint8_t)(b & 0x7F);
        if (b == 0)
            b = 1;
        buf[pos++] = b;
    }
    buf[pos - 1] |= 0x01; // never end on a zero byte
    return pos;
}

#endif
