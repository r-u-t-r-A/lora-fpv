// SPDX-License-Identifier: GPL-3.0-or-later
// Per-receiver packet loss model: independent loss, Gilbert-Elliott bursts,
// per-channel extra loss (interference) and outage windows.
#ifndef OLV_SIM_CHANNEL_H
#define OLV_SIM_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "fhss.h"

#define CH_MAX_OUTAGES 8

struct channel_model {
    double loss;                       // independent loss probability per packet
    double ge_p_gb;                    // Gilbert-Elliott: P(good -> bad) per packet (0 = off)
    double ge_p_bg;                    // P(bad -> good) per packet
    double ge_loss_bad;                // loss probability in the bad state
    double ch_loss[FHSS_MAX_CHANNELS]; // extra loss per channel
    double outage_from[CH_MAX_OUTAGES], outage_to[CH_MAX_OUTAGES]; // seconds
    int n_outages;
    uint32_t seed;
    // state
    uint32_t rng;
    bool bad;
};

// Parses "loss=0.2,burst=0.01/0.2,bad=3:0.9+7:0.5,outage=2-2.5,seed=1".
// Returns 0 on success, -1 with a message on stderr.
int channel_parse(struct channel_model *m, const char *spec);

void channel_reset(struct channel_model *m);

// Decides whether one packet on channel ch at time t_s (seconds) is lost.
bool channel_lost(struct channel_model *m, uint8_t ch, double t_s);

#endif
