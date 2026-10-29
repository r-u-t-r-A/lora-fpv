// SPDX-License-Identifier: GPL-3.0-or-later
// The four link presets (CLAUDE.md "Two modes only"), shared by air, vrx and sim.
#ifndef OLV_MODES_H
#define OLV_MODES_H

#include <stddef.h>
#include <string.h>

#include "fhss.h"
#include "ota.h"

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

static inline const struct olv_mode *olv_mode_find(const char *name) {
    for (size_t i = 0; i < sizeof(OLV_MODES) / sizeof(OLV_MODES[0]); i++)
        if (!strcmp(OLV_MODES[i].name, name))
            return &OLV_MODES[i];
    return NULL;
}

static inline uint32_t olv_band_center(const struct fhss_band *b) {
    return fhss_freq_hz(b, (uint8_t)(b->n / 2));
}

#endif
