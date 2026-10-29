// SPDX-License-Identifier: GPL-3.0-or-later
// Air-side transmit scheduler: owns the hop clock, sends a beacon at the start
// of each hop, then drains the packet queue, never letting a packet run past
// the end of its hop. Writes the over-the-air `seq` and `hop` fields at
// transmit time. Used by air/ in real time and by sim/ in virtual time.
#ifndef OLV_AIRSCHED_H
#define OLV_AIRSCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airtime.h"
#include "fhss.h"
#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIRSCHED_QMAX 1024

struct airsched_cfg {
    struct flrc_phy phy;
    uint32_t hop_us;
    uint16_t retune_us;      // UNMEASURED: from hop start until the radio can transmit
    uint8_t beacon_every;    // beacon every N hops (1 = every hop, 0 = never)
    uint8_t beacons_per_hop; // 1, or 2 = a second beacon from mid-hop (faster acquisition)
    uint32_t seed;           // fhss_seed(bind phrase)
    uint8_t n_channels;
    uint16_t link_id;
    uint8_t mode;
    uint8_t fec_k, fec_n;
    int8_t tx_power;
    uint16_t queue_max; // <= AIRSCHED_QMAX
};

struct airsched_stats {
    uint32_t sent, beacons, dropped, hops;
    uint64_t busy_us; // time on air
};

struct airsched {
    struct airsched_cfg cfg;
    struct fhss_seq seq;
    uint64_t hop_start_us;
    uint64_t abs_hop;
    uint8_t beacons_sent; // in the current hop
    uint16_t ota_seq;
    uint16_t q_head, q_count;
    uint16_t q_len[AIRSCHED_QMAX];
    uint8_t q[AIRSCHED_QMAX][OTA_MAX_PACKET];
    struct airsched_stats st;
};

void airsched_init(struct airsched *as, const struct airsched_cfg *cfg, uint64_t now_us);

// Queues one OTA packet (header + payload). Returns -1 and counts a drop if
// the queue is full.
int airsched_enqueue(struct airsched *as, const uint8_t *pkt, size_t len);

static inline unsigned airsched_queued(const struct airsched *as) {
    return as->q_count;
}

struct airsched_action {
    bool send;           // true: transmit out[0..len) now
    uint8_t channel;     // channel index for this hop
    bool retune;         // the hop changed since the last call
    uint32_t airtime_us; // when send: time on air
    uint64_t wake_us;    // when !send: call again at this time (or on enqueue)
    size_t len;
    uint8_t out[OTA_MAX_PACKET];
};

// Decides what to do at now_us. After a send, call again at
// now_us + airtime_us + phy.tx_gap_us (the TX-done event).
void airsched_next(struct airsched *as, uint64_t now_us, struct airsched_action *a);

// Packet size (header included, <= OTA_MAX_PACKET) that carries the most
// payload per hop given the beacon, retune time and per-packet gap. Writes
// packets per hop and payload bits per second if the pointers are non-NULL.
size_t airsched_best_packet(const struct airsched_cfg *cfg, unsigned *per_hop, uint32_t *bps);

static inline uint32_t airsched_cycle(const struct airsched *as) {
    return (uint32_t)(as->abs_hop / as->cfg.n_channels);
}

static inline uint8_t airsched_hop(const struct airsched *as) {
    return (uint8_t)(as->abs_hop % as->cfg.n_channels);
}

#ifdef __cplusplus
}
#endif

#endif
