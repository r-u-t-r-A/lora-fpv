// SPDX-License-Identifier: GPL-3.0-or-later
// FLRC airtime estimate. The bit counts and the per-packet gap are design
// guesses until M0 measures them on the LR2021 (tools/, docs/reports/).
#ifndef OLV_AIRTIME_H
#define OLV_AIRTIME_H

#include <stddef.h>
#include <stdint.h>

struct flrc_phy {
    uint32_t bitrate_bps; // gross FLRC bit rate
    uint8_t cr;           // 2 = 1/2, 3 = 3/4, 4 = 1 (uncoded), 5 = 2/3
    uint16_t preamble_bits;
    uint16_t sync_bits;
    uint16_t header_bits; // coded together with payload and CRC
    uint16_t crc_bits;
    uint16_t tx_gap_us; // UNMEASURED: end of one packet to start of the next
};

static inline struct flrc_phy flrc_phy_default(uint32_t bitrate_bps, uint8_t cr) {
    struct flrc_phy p = {bitrate_bps, cr, 32, 32, 16, 16, 150};
    return p;
}

// Time on air of one packet with len bytes of payload (OTA header included).
static inline uint32_t flrc_airtime_us(const struct flrc_phy *p, size_t len) {
    uint64_t coded = (uint64_t)p->header_bits + 8u * len + p->crc_bits + 6u; // + encoder tail
    switch (p->cr) {
    case 2:
        coded *= 2;
        break;
    case 3:
        coded = (coded * 4 + 2) / 3;
        break;
    case 5:
        coded = (coded * 3 + 1) / 2;
        break;
    default:
        break;
    }
    uint64_t bits = (uint64_t)p->preamble_bits + p->sync_bits + coded;
    return (uint32_t)((bits * 1000000u + p->bitrate_bps - 1) / p->bitrate_bps);
}

#endif
