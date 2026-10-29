// SPDX-License-Identifier: GPL-3.0-or-later
// Frequency hopping: per-cycle Fisher-Yates shuffle of the channel list,
// driven by xorshift32 seeded with FNV-1a(bind phrase) XOR cycle number.
// Every channel appears exactly once per cycle. Shared by the Pico firmware
// and host code; no dynamic allocation, no libc beyond <string.h>.
#ifndef OLV_FHSS_H
#define OLV_FHSS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FHSS_MAX_CHANNELS 64

struct fhss_band {
    uint32_t start_hz;
    uint32_t step_hz;
    uint8_t n;
};

// Defaults from the design doc; both are configurable at runtime.
#define FHSS_BAND_LR_868 ((struct fhss_band){863500000u, 650000u, 10})
#define FHSS_BAND_HQ_2G4 ((struct fhss_band){2402000000u, 2000000u, 40})

static inline uint32_t fhss_freq_hz(const struct fhss_band *b, uint8_t ch) {
    return b->start_hz + (uint32_t)ch * b->step_hz;
}

uint32_t fhss_seed(const char *bind_phrase);

// Writes the channel order for one cycle into perm[0..n-1]. n <= FHSS_MAX_CHANNELS.
void fhss_permutation(uint32_t seed, uint32_t cycle, uint8_t n, uint8_t *perm);

// Caches the permutation of the most recently used cycle.
struct fhss_seq {
    uint32_t seed;
    uint32_t cycle;
    uint8_t n;
    bool valid;
    uint8_t perm[FHSS_MAX_CHANNELS];
};

void fhss_seq_init(struct fhss_seq *s, uint32_t seed, uint8_t n);
uint8_t fhss_seq_channel(struct fhss_seq *s, uint32_t cycle, uint8_t hop);

// ---- Receiver synchronisation ----
//
// PARKED:   listen on one channel until a beacon arrives. Every channel is
//           used once per cycle, but cycles are reshuffled, so the wait is
//           up to 2n-1 hops (LR: 190 ms, HQ: 790 ms at 100 Hz). The beacon gives the
//           cycle number and hop index; the caller converts its RX timestamp
//           into the start time of that hop.
// LOCKED:   hop on the local clock; every valid packet on the expected hop
//           counts as "heard"; every beacon re-aligns the clock.
// COASTING: after miss_limit consecutive hops with nothing heard, keep
//           hopping for coast_us; any packet on the expected hop re-locks.
//           After coast_us, go back to PARKED on the next park channel.
// While PARKED, after park_us without a beacon the park channel advances and
// park_timeouts increments; the firmware uses that to alternate bands.

enum fhss_rx_state { FHSS_RX_PARKED = 0, FHSS_RX_LOCKED = 1, FHSS_RX_COASTING = 2 };

struct fhss_rx_cfg {
    uint32_t hop_us;    // dwell per hop (10000)
    uint8_t n;          // channels
    uint8_t miss_limit; // hops with nothing heard before coasting (3)
    uint32_t coast_us;  // how long to coast before re-parking (1000000)
    uint32_t park_us;   // park timeout per channel ((2n+1) hops)
};

struct fhss_rx_cfg fhss_rx_default_cfg(uint8_t n, uint32_t hop_us);

struct fhss_rx {
    struct fhss_rx_cfg cfg;
    struct fhss_seq seq;
    enum fhss_rx_state state;
    uint8_t park_ch;
    uint32_t park_start_us;
    uint32_t cycle;
    uint8_t hop;
    uint32_t hop_start_us;
    uint8_t missed;
    bool heard;
    uint32_t coast_start_us;
    // statistics
    uint32_t locks;
    uint32_t park_timeouts;
    uint32_t hops_heard;
    uint32_t hops_missed;
};

void fhss_rx_init(struct fhss_rx *rx, uint32_t seed, const struct fhss_rx_cfg *cfg,
                  uint32_t now_us);

// A valid beacon arrived. hop_start_us is the local time the air unit's hop
// started (RX done time minus beacon airtime minus beacon offset).
// Returns false if the beacon is inconsistent with the parked channel.
bool fhss_rx_on_beacon(struct fhss_rx *rx, uint32_t cycle, uint8_t hop, uint32_t hop_start_us);

// Any other valid packet arrived, carrying the hop index in its header.
void fhss_rx_on_packet(struct fhss_rx *rx, uint8_t hop);

// Advances the state machine to now_us. Returns the channel index the radio
// must be on; *changed is set when it differs from the previous call.
uint8_t fhss_rx_poll(struct fhss_rx *rx, uint32_t now_us, bool *changed);

// Channel the receiver currently expects (no state change).
uint8_t fhss_rx_channel(struct fhss_rx *rx);

// Local time of the next hop boundary or park timeout, for timer scheduling.
uint32_t fhss_rx_next_event_us(const struct fhss_rx *rx);

#ifdef __cplusplus
}
#endif

#endif
