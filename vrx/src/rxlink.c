// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L

#include "rxlink.h"
#include "ota.h"
#include "vrxlog.h"

#include <string.h>

static int32_t tdiff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

// Moves to channel ch: standby, retune, receive. (Whether the LR2021 can be
// retuned while in RX without the standby step is an M0 measurement.)
static void tune(struct rxlink *l, uint8_t ch) {
    l->ch = ch;
    radio_standby(l->r);
    radio_set_freq(l->r, fhss_freq_hz(&l->mode->band, ch));
    radio_rx(l->r);
}

int rxlink_open(struct rxlink *l, const struct olv_mode *mode, const char *bind,
                const struct radio_pins *pins, struct radio_flrc *flrc) {
    memset(l, 0, sizeof(*l));
    l->mode = mode;
    l->link_id = ota_link_id(bind);
    flrc->freq_hz = olv_band_center(&mode->band);
    flrc->bitrate_bps = mode->bitrate_bps;
    if (!flrc->cr)
        flrc->cr = mode->cr;
    l->phy = flrc_phy_default(flrc->bitrate_bps, flrc->cr);
    l->r = radio_open(pins, flrc);
    if (!l->r)
        return -1;
    uint32_t now = radio_now_us();
    struct fhss_rx_cfg fc = fhss_rx_default_cfg(mode->band.n, 10000);
    fhss_rx_init(&l->sync, fhss_seed(bind), &fc, now);
    l->start_us = now;
    tune(l, fhss_rx_channel(&l->sync));
    return 0;
}

void rxlink_close(struct rxlink *l) {
    if (l->r)
        radio_close(l->r);
    l->r = NULL;
}

static void flush(struct rxlink *l, rxlink_emit_cb emit, void *ctx) {
    if (l->msg_len)
        emit(ctx, l->msg, l->msg_len);
    l->msg_len = 0;
}

static void on_packet(struct rxlink *l, const uint8_t *buf, size_t len,
                      const struct radio_rx_info *info, rxlink_emit_cb emit, void *ctx) {
    struct rxlink_ch *pc = &l->per_ch[l->ch];
    if (!info->crc_ok) {
        l->pkts_crc++;
        pc->crc++;
        return; // an erasure: never passed up
    }
    struct ota_hdr h;
    if (ota_hdr_unpack(buf, len, l->link_id, &h) != OTA_OK) {
        l->pkts_foreign++; // other link (bind phrase) or garbage
        return;
    }
    l->pkts_ok++;
    pc->pkts++;
    if (ota_hdr_type(&h) == OTA_TYPE_BEACON) {
        struct ota_beacon b;
        if (ota_beacon_unpack(buf + OTA_HDR_LEN, len - OTA_HDR_LEN, &b) == OTA_OK) {
            uint32_t hop_start = info->t_us - flrc_airtime_us(&l->phy, len) - b.offset_us;
            if (fhss_rx_on_beacon(&l->sync, b.cycle, h.hop, hop_start)) {
                pc->beacons++;
                if (!l->first_lock_us)
                    l->first_lock_us = (uint64_t)(uint32_t)(info->t_us - l->start_us) + 1;
            }
        }
    } else {
        fhss_rx_on_packet(&l->sync, h.hop);
    }

    struct vrx_rx_packet rp = {.ota_len = (uint16_t)len,
                               .rssi = info->rssi_dbm,
                               .snr = info->snr_q4,
                               .hop = h.hop,
                               .t_us = info->t_us};
    memcpy(rp.ota, buf, len);
    uint8_t rec[OTA_MAX_PACKET + VRX_RX_TRAILER];
    size_t rl = vrx_rx_packet_pack(&rp, rec, sizeof(rec));
    if (l->capture)
        vrxlog_write(l->capture, info->t_us, rec, (uint16_t)rl);
    if (ota_hdr_type(&h) == OTA_TYPE_BEACON)
        return; // the GUI does not need beacons
    size_t nl = vrx_rec_append(l->msg, l->msg_len, sizeof(l->msg), VRX_REC_RX_PACKET, rec, rl);
    if (!nl) {
        flush(l, emit, ctx);
        nl = vrx_rec_append(l->msg, 0, sizeof(l->msg), VRX_REC_RX_PACKET, rec, rl);
    }
    if (l->msg_len == 0)
        l->msg_t_us = info->t_us;
    l->msg_len = nl;
}

// Credits every hop that has ended since the last call to its channel, so a
// late loop iteration that spans two hops still counts both.
static void count_hops(struct rxlink *l) {
    const struct fhss_rx *x = &l->sync;
    if (x->state == FHSS_RX_PARKED) {
        l->counting = false;
        return;
    }
    uint64_t abs = (uint64_t)x->cycle * l->mode->band.n + x->hop;
    if (!l->counting || abs < l->last_abs_hop || abs - l->last_abs_hop > 1000) {
        l->counting = true; // (re)locked: start counting from here
        l->last_abs_hop = abs;
        return;
    }
    struct fhss_seq *seq = (struct fhss_seq *)&x->seq;
    for (; l->last_abs_hop < abs; l->last_abs_hop++) {
        uint64_t h = l->last_abs_hop;
        uint8_t c =
            fhss_seq_channel(seq, (uint32_t)(h / l->mode->band.n), (uint8_t)(h % l->mode->band.n));
        l->per_ch[c].hops++;
    }
}

void rxlink_step(struct rxlink *l, uint32_t max_wait_us, rxlink_emit_cb emit, void *ctx) {
    uint32_t now = radio_now_us();
    int32_t until = tdiff(fhss_rx_next_event_us(&l->sync), now);
    uint32_t wait = until <= 0 ? 0 : (uint32_t)until < max_wait_us ? (uint32_t)until : max_wait_us;

    uint8_t buf[OTA_MAX_PACKET];
    size_t len = 0;
    struct radio_rx_info info;
    enum radio_event ev = radio_poll(l->r, wait, buf, sizeof(buf), &len, &info);
    if (ev == RADIO_EV_RX_DONE)
        on_packet(l, buf, len, &info, emit, ctx);
    else if (ev == RADIO_EV_ERROR) {
        l->radio_errors++;
        tune(l, l->ch);
    }

    now = radio_now_us();
    bool changed = false;
    uint8_t ch = fhss_rx_poll(&l->sync, now, &changed);
    count_hops(l);
    if (changed)
        tune(l, ch);
    if (l->msg_len && tdiff(now, l->msg_t_us) >= 5000)
        flush(l, emit, ctx);
}

void rxlink_stats_json(const struct rxlink *l, char *out, size_t cap) {
    static const char *st[] = {"parked", "locked", "coasting"};
    size_t n = (size_t)snprintf(out, cap,
                                "{\"mode\":\"%s\",\"sync\":\"%s\",\"cycle\":%u,\"hop\":%u,"
                                "\"locks\":%u,\"park_timeouts\":%u,\"first_lock_ms\":%.1f,"
                                "\"pkts_ok\":%u,\"pkts_crc\":%u,\"foreign\":%u,\"radio_errors\":%u,"
                                "\"channels\":[",
                                l->mode->name, st[l->sync.state], l->sync.cycle, l->sync.hop,
                                l->sync.locks, l->sync.park_timeouts,
                                l->first_lock_us ? (double)(l->first_lock_us - 1) / 1000.0 : -1.0,
                                l->pkts_ok, l->pkts_crc, l->pkts_foreign, l->radio_errors);
    for (int c = 0; c < l->mode->band.n && n < cap; c++) {
        const struct rxlink_ch *p = &l->per_ch[c];
        double sent = (double)p->hops * l->mode->beacons_per_hop;
        double per = p->hops ? 100.0 * (1.0 - (double)p->beacons / sent) : -1.0;
        if (per < 0 && p->hops)
            per = 0;
        n += (size_t)snprintf(out + n, cap - n,
                              "%s{\"ch\":%d,\"mhz\":%.3f,\"hops\":%u,\"beacons\":%u,\"pkts\":%u,"
                              "\"crc\":%u,\"beacon_loss_pct\":%.1f}",
                              c ? "," : "", c, fhss_freq_hz(&l->mode->band, (uint8_t)c) / 1e6,
                              p->hops, p->beacons, p->pkts, p->crc, per);
    }
    if (n < cap)
        snprintf(out + n, cap - n, "]}");
}
