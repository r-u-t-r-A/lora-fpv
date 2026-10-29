// SPDX-License-Identifier: GPL-3.0-or-later
// The four link presets (CLAUDE.md "Two modes only"), shared by air, vrx and sim,
// and which of them a radio chip can run.
#ifndef OLV_MODES_H
#define OLV_MODES_H

#include <stddef.h>
#include <string.h>

#include "fhss.h"
#include "ota.h"
#include "radio.h"

struct olv_mode {
    const char *name;
    enum ota_mode mode;
    uint32_t bitrate_bps; // FLRC gross bit rate
    uint8_t cr;           // default inner coding rate (see airtime.h)
    struct fhss_band band;
    int w, h, fps;           // default picture
    double refresh_s;        // default refresh period
    uint8_t beacons_per_hop; // 2 where acquisition would otherwise be slow (40 channels)
};

static const struct olv_mode OLV_MODES[] = {
    {"lr520", OTA_MODE_LR_520, 520000, 3, FHSS_BAND_LR_868, 640, 360, 30, 1.0, 1},
    {"lr260", OTA_MODE_LR_260, 260000, 2, FHSS_BAND_LR_868, 640, 360, 30, 2.0, 1},
    {"hq2600", OTA_MODE_HQ_2600, 2600000, 3, FHSS_BAND_HQ_2G4, 1280, 720, 60, 0.5, 2},
    {"hq1300", OTA_MODE_HQ_1300, 1300000, 3, FHSS_BAND_HQ_2G4, 1280, 720, 30, 0.5, 2},
};

#define OLV_N_MODES (sizeof(OLV_MODES) / sizeof(OLV_MODES[0]))

static inline const struct olv_mode *olv_mode_find(const char *name) {
    for (size_t i = 0; i < OLV_N_MODES; i++)
        if (!strcmp(OLV_MODES[i].name, name))
            return &OLV_MODES[i];
    return NULL;
}

static inline bool olv_mode_is_2g4(const struct olv_mode *m) {
    return m->band.start_hz >= 2400000000u;
}

// True if the chip can run the preset (band, bit rate, coding rate). The
// SX1280 runs only hq1300.
static inline bool olv_mode_supported(const struct olv_mode *m, const struct radio_caps *c) {
    return (olv_mode_is_2g4(m) ? c->ghz_2g4 : c->sub_ghz) &&
           radio_caps_bitrate_ok(c, m->bitrate_bps) && (c->cr_mask & (1u << m->cr));
}

// Band filter for mode selection: "868", "2400" or "auto" (both). Returns -1
// for anything else.
static inline int olv_band_parse(const char *s) {
    return !strcmp(s, "auto") ? 0 : !strcmp(s, "868") ? 1 : !strcmp(s, "2400") ? 2 : -1;
}

static inline bool olv_band_match(int band, const struct olv_mode *m) {
    return band == 0 || (band == 2) == olv_mode_is_2g4(m);
}

// Modes a receiver scans: the named one, or with "auto" every preset in the
// band filter that the chip supports, in OLV_MODES order. Returns the count.
static inline size_t olv_modes_select(const char *mode, int band, const struct radio_caps *c,
                                      const struct olv_mode **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < OLV_N_MODES && n < cap; i++) {
        const struct olv_mode *m = &OLV_MODES[i];
        if ((strcmp(mode, "auto") ? !strcmp(mode, m->name) : olv_band_match(band, m)) &&
            olv_mode_supported(m, c))
            out[n++] = m;
    }
    return n;
}

static inline uint32_t olv_band_center(const struct fhss_band *b) {
    return fhss_freq_hz(b, (uint8_t)(b->n / 2));
}

#endif
