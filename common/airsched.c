// SPDX-License-Identifier: GPL-3.0-or-later
#include "airsched.h"
#include "olv_util.h"

#include <string.h>

void airsched_init(struct airsched *as, const struct airsched_cfg *cfg, uint64_t now_us) {
    memset(as, 0, sizeof(*as));
    as->cfg = *cfg;
    if (as->cfg.queue_max == 0 || as->cfg.queue_max > AIRSCHED_QMAX)
        as->cfg.queue_max = AIRSCHED_QMAX;
    fhss_seq_init(&as->seq, cfg->seed, cfg->n_channels);
    as->hop_start_us = now_us;
}

int airsched_enqueue(struct airsched *as, const uint8_t *pkt, size_t len) {
    if (len < OTA_HDR_LEN || len > OTA_MAX_PACKET || as->q_count >= as->cfg.queue_max) {
        as->st.dropped++;
        return -1;
    }
    uint16_t slot = (uint16_t)((as->q_head + as->q_count) % AIRSCHED_QMAX);
    memcpy(as->q[slot], pkt, len);
    as->q_len[slot] = (uint16_t)len;
    as->q_count++;
    return 0;
}

static void stamp(struct airsched *as, uint8_t *pkt) {
    olv_put_u16(pkt + 2, as->ota_seq++);
    pkt[4] = airsched_hop(as);
}

void airsched_next(struct airsched *as, uint64_t now_us, struct airsched_action *a) {
    const struct airsched_cfg *c = &as->cfg;
    a->send = false;
    a->retune = false;
    while (now_us >= as->hop_start_us + c->hop_us) {
        as->hop_start_us += c->hop_us;
        as->abs_hop++;
        as->beacons_sent = 0;
        as->st.hops++;
        a->retune = true;
    }
    a->channel = fhss_seq_channel(&as->seq, airsched_cycle(as), airsched_hop(as));
    uint64_t hop_end = as->hop_start_us + c->hop_us;
    uint64_t ready = as->hop_start_us + c->retune_us;
    if (now_us < ready) {
        a->wake_us = ready;
        return;
    }

    uint8_t per_hop = c->beacons_per_hop > 1 ? 2 : 1;
    uint64_t mid = as->hop_start_us + c->hop_us / 2;
    bool beacon_hop = c->beacon_every && (as->abs_hop % c->beacon_every) == 0;
    bool want_beacon = beacon_hop && (as->beacons_sent == 0 ||
                                      (as->beacons_sent == 1 && per_hop == 2 && now_us >= mid));
    if (want_beacon) {
        as->beacons_sent++;
        struct ota_hdr h = {.link_id = c->link_id,
                            .flags = ota_flags(OTA_TYPE_BEACON, (enum ota_mode)c->mode, 0)};
        struct ota_beacon b = {.cycle = airsched_cycle(as),
                               .hop_us = (uint16_t)c->hop_us,
                               .n_channels = c->n_channels,
                               .fec_k = c->fec_k,
                               .fec_n = c->fec_n,
                               .tx_power = c->tx_power,
                               .offset_us = (uint16_t)(now_us - as->hop_start_us)};
        ota_hdr_pack(&h, a->out);
        ota_beacon_pack(&b, a->out + OTA_HDR_LEN);
        a->len = OTA_HDR_LEN + OTA_BEACON_LEN;
        a->airtime_us = flrc_airtime_us(&c->phy, a->len);
        if (now_us + a->airtime_us <= hop_end) {
            stamp(as, a->out);
            a->send = true;
            as->st.beacons++;
            as->st.busy_us += a->airtime_us;
            return;
        }
    }

    if (as->q_count) {
        uint16_t slot = as->q_head;
        uint32_t air = flrc_airtime_us(&c->phy, as->q_len[slot]);
        if (now_us + air <= hop_end) {
            a->len = as->q_len[slot];
            memcpy(a->out, as->q[slot], a->len);
            as->q_head = (uint16_t)((as->q_head + 1) % AIRSCHED_QMAX);
            as->q_count--;
            stamp(as, a->out);
            a->airtime_us = air;
            a->send = true;
            as->st.sent++;
            as->st.busy_us += air;
            return;
        }
    }
    // Nothing fits in this hop (or nothing to send): wait for the next hop,
    // or for mid-hop if the second beacon is still due.
    a->wake_us =
        (beacon_hop && per_hop == 2 && as->beacons_sent == 1 && now_us < mid) ? mid : hop_end;
}

size_t airsched_best_packet(const struct airsched_cfg *c, unsigned *per_hop, uint32_t *bps) {
    uint32_t avail = c->hop_us > c->retune_us ? c->hop_us - c->retune_us : 0;
    if (c->beacon_every == 1) {
        uint32_t b = flrc_airtime_us(&c->phy, OTA_HDR_LEN + OTA_BEACON_LEN) + c->phy.tx_gap_us;
        if (c->beacons_per_hop > 1)
            b *= 2;
        avail = avail > b ? avail - b : 0;
    }
    size_t best_len = 0;
    uint64_t best_payload = 0;
    unsigned best_n = 0;
    for (size_t len = OTA_HDR_LEN + 1; len <= OTA_MAX_PACKET; len++) {
        // The last packet of the hop needs no trailing gap.
        uint32_t one = flrc_airtime_us(&c->phy, len);
        if (one > avail)
            break;
        unsigned n = 1 + (avail - one) / (one + c->phy.tx_gap_us);
        uint64_t payload = (uint64_t)n * (len - OTA_HDR_LEN);
        if (payload >= best_payload) {
            best_payload = payload;
            best_len = len;
            best_n = n;
        }
    }
    if (per_hop)
        *per_hop = best_n;
    if (bps)
        *bps = (uint32_t)(best_payload * 8u * 1000000u / c->hop_us);
    return best_len;
}
