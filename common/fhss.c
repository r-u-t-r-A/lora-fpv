// SPDX-License-Identifier: GPL-3.0-or-later
#include "fhss.h"
#include "olv_util.h"

#include <string.h>

static inline int32_t tdiff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

uint32_t fhss_seed(const char *bind_phrase) {
    return olv_fnv1a32(bind_phrase);
}

void fhss_permutation(uint32_t seed, uint32_t cycle, uint8_t n, uint8_t *perm) {
    if (n > FHSS_MAX_CHANNELS)
        n = FHSS_MAX_CHANNELS;
    uint32_t state = seed ^ cycle;
    if (state == 0)
        state = 0x9E3779B9u; // xorshift32 must not start at zero
    for (uint8_t i = 0; i < n; i++)
        perm[i] = i;
    for (int i = n - 1; i > 0; i--) {
        uint32_t j = olv_xorshift32(&state) % (uint32_t)(i + 1);
        uint8_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
}

void fhss_seq_init(struct fhss_seq *s, uint32_t seed, uint8_t n) {
    memset(s, 0, sizeof(*s));
    s->seed = seed;
    s->n = n > FHSS_MAX_CHANNELS ? FHSS_MAX_CHANNELS : n;
}

uint8_t fhss_seq_channel(struct fhss_seq *s, uint32_t cycle, uint8_t hop) {
    if (!s->valid || s->cycle != cycle) {
        fhss_permutation(s->seed, cycle, s->n, s->perm);
        s->cycle = cycle;
        s->valid = true;
    }
    return s->perm[hop % s->n];
}

struct fhss_rx_cfg fhss_rx_default_cfg(uint8_t n, uint32_t hop_us) {
    struct fhss_rx_cfg c = {
        .hop_us = hop_us,
        .n = n,
        .miss_limit = 3,
        .coast_us = 1000000u,
        // Each cycle is reshuffled, so a channel can be first in one cycle and
        // last in the next: the gap between two uses is up to 2n-1 hops.
        // Parking for 2n+1 hops guarantees it comes up at least once.
        .park_us = hop_us * (2u * (uint32_t)n + 1u),
    };
    return c;
}

static void enter_parked(struct fhss_rx *rx, uint32_t now_us) {
    rx->state = FHSS_RX_PARKED;
    rx->park_start_us = now_us;
    rx->missed = 0;
    rx->heard = false;
}

void fhss_rx_init(struct fhss_rx *rx, uint32_t seed, const struct fhss_rx_cfg *cfg,
                  uint32_t now_us) {
    memset(rx, 0, sizeof(*rx));
    rx->cfg = *cfg;
    fhss_seq_init(&rx->seq, seed, cfg->n);
    rx->park_ch = 0;
    enter_parked(rx, now_us);
}

uint8_t fhss_rx_channel(struct fhss_rx *rx) {
    if (rx->state == FHSS_RX_PARKED)
        return rx->park_ch;
    return fhss_seq_channel(&rx->seq, rx->cycle, rx->hop);
}

bool fhss_rx_on_beacon(struct fhss_rx *rx, uint32_t cycle, uint8_t hop, uint32_t hop_start_us) {
    if (hop >= rx->cfg.n)
        return false;
    if (rx->state == FHSS_RX_PARKED) {
        if (fhss_seq_channel(&rx->seq, cycle, hop) != rx->park_ch)
            return false;
        rx->locks++;
    }
    rx->state = FHSS_RX_LOCKED;
    rx->cycle = cycle;
    rx->hop = hop;
    rx->hop_start_us = hop_start_us;
    rx->heard = true;
    rx->missed = 0;
    return true;
}

void fhss_rx_on_packet(struct fhss_rx *rx, uint8_t hop) {
    if (rx->state == FHSS_RX_PARKED || hop != rx->hop)
        return;
    rx->heard = true;
    rx->missed = 0;
    rx->state = FHSS_RX_LOCKED;
}

static void advance_hop(struct fhss_rx *rx) {
    if (rx->heard) {
        rx->hops_heard++;
        rx->missed = 0;
    } else {
        rx->hops_missed++;
        if (rx->missed < 255)
            rx->missed++;
    }
    rx->heard = false;
    rx->hop_start_us += rx->cfg.hop_us;
    if (++rx->hop >= rx->cfg.n) {
        rx->hop = 0;
        rx->cycle++;
    }
}

uint8_t fhss_rx_poll(struct fhss_rx *rx, uint32_t now_us, bool *changed) {
    uint8_t before = fhss_rx_channel(rx);
    if (rx->state == FHSS_RX_PARKED) {
        if (tdiff(now_us, rx->park_start_us) >= (int32_t)rx->cfg.park_us) {
            rx->park_timeouts++;
            rx->park_ch = (uint8_t)((rx->park_ch + 1) % rx->cfg.n);
            rx->park_start_us = now_us;
        }
    } else {
        while (tdiff(now_us, rx->hop_start_us) >= (int32_t)rx->cfg.hop_us) {
            advance_hop(rx);
            if (rx->state == FHSS_RX_LOCKED && rx->missed >= rx->cfg.miss_limit) {
                rx->state = FHSS_RX_COASTING;
                rx->coast_start_us = rx->hop_start_us;
            }
            if (rx->state == FHSS_RX_COASTING &&
                tdiff(now_us, rx->coast_start_us) >= (int32_t)rx->cfg.coast_us) {
                // Park on the channel we would have used next, so repeated
                // losses do not keep us parked on one dead channel.
                rx->park_ch = fhss_seq_channel(&rx->seq, rx->cycle, rx->hop);
                enter_parked(rx, now_us);
                break;
            }
        }
    }
    uint8_t after = fhss_rx_channel(rx);
    if (changed)
        *changed = after != before;
    return after;
}

uint32_t fhss_rx_next_event_us(const struct fhss_rx *rx) {
    if (rx->state == FHSS_RX_PARKED)
        return rx->park_start_us + rx->cfg.park_us;
    return rx->hop_start_us + rx->cfg.hop_us;
}
