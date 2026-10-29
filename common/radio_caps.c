// SPDX-License-Identifier: GPL-3.0-or-later
// FLRC capabilities of the supported chips, from the datasheets and the
// RadioLib 7.8.1 drivers:
//   LR2021: sub-GHz + 2.4 GHz, 2600..260 kbit/s, CR 1/2 3/4 2/3 1, 255 B,
//           -9..+22 dBm sub-GHz, -19..+12 dBm 2.4 GHz.
//   SX1280: 2.4 GHz only, 1300..260 kbit/s, CR 1/2 3/4 1, FLRC payload
//           6..127 B (datasheet rev 3.2, "payload length" packet parameter),
//           -18..+13 dBm.
#include "radio.h"

#include <stdio.h>
#include <string.h>

#define CR(c) (1u << (c))

static const struct radio_caps caps[RADIO_CHIPS] = {
    [RADIO_CHIP_LR2021] = {.name = "lr2021",
                           .sub_ghz = true,
                           .ghz_2g4 = true,
                           .max_packet = 255,
                           .lf_dbm = {-9, 22},
                           .hf_dbm = {-19, 12},
                           .cr_mask = CR(2) | CR(3) | CR(4) | CR(5),
                           .bitrates = {2600000, 2080000, 1300000, 1040000, 650000, 520000, 325000,
                                        260000, 0}},
    [RADIO_CHIP_SX1280] = {.name = "sx1280",
                           .sub_ghz = false,
                           .ghz_2g4 = true,
                           .max_packet = 127,
                           .lf_dbm = {0, 0},
                           .hf_dbm = {-18, 13},
                           .cr_mask = CR(2) | CR(3) | CR(4),
                           .bitrates = {1300000, 1040000, 650000, 520000, 325000, 260000, 0}},
};

const struct radio_caps *radio_chip_caps(enum radio_chip chip) {
    return (unsigned)chip < RADIO_CHIPS ? &caps[chip] : NULL;
}

int radio_chip_parse(const char *name) {
    for (int i = 0; i < RADIO_CHIPS; i++)
        if (!strcmp(caps[i].name, name))
            return i;
    return -1;
}

static bool is_2g4(uint32_t hz) {
    return hz >= 2400000000u && hz <= 2500000000u;
}

bool radio_caps_freq_ok(const struct radio_caps *c, uint32_t hz) {
    if (is_2g4(hz))
        return c->ghz_2g4;
    return c->sub_ghz && hz >= 150000000u && hz <= 1060000000u;
}

bool radio_caps_bitrate_ok(const struct radio_caps *c, uint32_t bps) {
    for (const uint32_t *b = c->bitrates; *b; b++)
        if (*b == bps)
            return true;
    return false;
}

const char *radio_check_power(const struct radio_pins *p, uint32_t freq_hz, int8_t dbm) {
    static char msg[128];
    const struct radio_caps *c = radio_chip_caps(p->chip);
    const int8_t *r = is_2g4(freq_hz) ? c->hf_dbm : c->lf_dbm;
    if (dbm < r[0] || dbm > r[1]) {
        snprintf(msg, sizeof(msg), "%s output %d dBm is outside %d..%d dBm", c->name, dbm, r[0],
                 r[1]);
        return msg;
    }
    if (p->has_max_power && dbm > p->max_power_dbm) {
        snprintf(msg, sizeof(msg), "%d dBm is above max_power_dbm=%d for this module's PA", dbm,
                 p->max_power_dbm);
        return msg;
    }
    return NULL;
}

const char *radio_check(const struct radio_pins *p, const struct radio_flrc *f) {
    static char msg[128];
    const struct radio_caps *c = radio_chip_caps(p->chip);
    if (!c)
        return "unknown radio chip";
    if (!radio_caps_freq_ok(c, f->freq_hz)) {
        snprintf(msg, sizeof(msg), "%s cannot tune to %.3f MHz%s", c->name, f->freq_hz / 1e6,
                 c->sub_ghz ? "" : " (2.4 GHz only)");
        return msg;
    }
    if (!radio_caps_bitrate_ok(c, f->bitrate_bps)) {
        snprintf(msg, sizeof(msg), "%s has no FLRC %u bit/s", c->name, f->bitrate_bps);
        return msg;
    }
    if (f->cr > 7 || !(c->cr_mask & CR(f->cr))) {
        snprintf(msg, sizeof(msg), "%s has no FLRC coding rate %u", c->name, f->cr);
        return msg;
    }
    return radio_check_power(p, f->freq_hz, f->power_dbm);
}
