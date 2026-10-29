// SPDX-License-Identifier: GPL-3.0-or-later
// VRX receive path: radio + hop synchronisation (common/fhss) + per-channel
// statistics; good packets become VRX_REC_RX_PACKET records for the GUI.
// With more than one candidate mode (vrx.conf mode=auto), each park timeout
// without a beacon moves on to the next mode (band, bit rate, coding rate),
// so the VRX finds whatever the air unit is sending.
#ifndef OLV_RXLINK_H
#define OLV_RXLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airtime.h"
#include "fhss.h"
#include "modes.h"
#include "radio.h"
#include "vrx_proto.h"

struct rxlink_ch {
    uint32_t hops;    // hops spent on this channel while locked/coasting
    uint32_t beacons; // beacons heard on it (mode->beacons_per_hop are sent per hop)
    uint32_t pkts;    // good packets
    uint32_t crc;     // CRC failures
};

#define RXLINK_MAX_MODES 4

typedef void (*rxlink_emit_cb)(void *ctx, const uint8_t *msg, size_t len);

struct rxlink {
    struct radio *r;
    const struct olv_mode *mode; // current
    const struct olv_mode *modes[RXLINK_MAX_MODES];
    size_t n_modes, cur;
    uint8_t cr_override; // 0 = each mode's default
    uint32_t seed;
    uint32_t park_timeouts_seen;
    uint32_t mode_switches;
    const struct olv_mode *stats_mode; // mode the per-channel table belongs to
    struct flrc_phy phy;
    struct fhss_rx sync;
    uint16_t link_id;
    uint8_t ch;
    uint64_t last_abs_hop; // cycle * n + hop last counted, while locked
    bool counting;
    struct rxlink_ch per_ch[FHSS_MAX_CHANNELS];
    uint32_t pkts_ok, pkts_crc, pkts_foreign, radio_errors;
    uint64_t first_lock_us, start_us;
    uint8_t msg[VRX_MSG_MAX];
    size_t msg_len;
    uint32_t msg_t_us;
    FILE *capture; // optional vrxlog of everything received
};

// Opens the radio on modes[0]; flrc->cr != 0 overrides every mode's coding rate.
int rxlink_open(struct rxlink *l, const struct olv_mode *const *modes, size_t n_modes,
                const char *bind, const struct radio_pins *pins, struct radio_flrc *flrc);
void rxlink_close(struct rxlink *l);

// Does radio work for at most max_wait_us. Complete WebSocket messages go to
// emit (batched; flushed at least every 5 ms).
void rxlink_step(struct rxlink *l, uint32_t max_wait_us, rxlink_emit_cb emit, void *ctx);

// Per-channel statistics as JSON into out.
void rxlink_stats_json(const struct rxlink *l, char *out, size_t cap);

#endif
