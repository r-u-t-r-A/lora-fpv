// SPDX-License-Identifier: GPL-3.0-or-later
#include "airsched.h"
#include "olv_util.h"
#include "t.h"

#include <string.h>

static struct airsched as;

static struct airsched_cfg cfg_for(uint32_t bps, uint8_t n) {
    return (struct airsched_cfg){.phy = flrc_phy_default(bps, 3),
                                 .hop_us = 10000,
                                 .retune_us = 200,
                                 .beacon_every = 1,
                                 .seed = 0xC0FFEE,
                                 .n_channels = n,
                                 .link_id = 0x1111,
                                 .fec_k = 8,
                                 .fec_n = 12,
                                 .queue_max = 1024};
}

static void test_airtime(void) {
    struct flrc_phy p = flrc_phy_default(520000, 3);
    // 32 + 32 + (16 + 2040 + 16 + 6) * 4/3 = 2835 bits at 520 kbit/s.
    CHECK(flrc_airtime_us(&p, 255) == 5452);
    p.cr = 4;
    CHECK(flrc_airtime_us(&p, 255) == (64 + 2078) * 1000000u / 520000 + 1);
}

static void test_schedule_rules(void) {
    struct airsched_cfg c = cfg_for(520000, 10);
    airsched_init(&as, &c, 0);
    uint8_t pkt[OTA_MAX_PACKET] = {0};
    for (int i = 0; i < 600; i++) {
        size_t len = 60 + (size_t)(i * 37) % 196;
        olv_put_u16(pkt, 0x1111);
        pkt[10] = 1; // k
        pkt[11] = 1; // n
        REQUIRE(airsched_enqueue(&as, pkt, len) == 0);
    }
    struct fhss_seq ref;
    fhss_seq_init(&ref, c.seed, c.n_channels);
    uint64_t now = 0, last_hop = UINT64_MAX;
    uint16_t expect_seq = 0;
    bool first_in_hop = false;
    struct airsched_action a;
    while (airsched_queued(&as) && now < 10000000) {
        airsched_next(&as, now, &a);
        if (as.abs_hop != last_hop) {
            last_hop = as.abs_hop;
            first_in_hop = true;
        }
        CHECK(a.channel == fhss_seq_channel(&ref, airsched_cycle(&as), airsched_hop(&as)));
        if (!a.send) {
            CHECK(a.wake_us > now);
            now = a.wake_us;
            continue;
        }
        uint64_t hop_start = as.hop_start_us;
        CHECK(now >= hop_start + c.retune_us);
        CHECK(now + a.airtime_us <= hop_start + c.hop_us);
        struct ota_hdr h;
        REQUIRE(ota_hdr_unpack(a.out, a.len, 0x1111, &h) == OTA_OK);
        CHECK(h.seq == expect_seq++);
        CHECK(h.hop == airsched_hop(&as));
        if (first_in_hop) {
            CHECK(ota_hdr_type(&h) == OTA_TYPE_BEACON);
            struct ota_beacon b;
            REQUIRE(ota_beacon_unpack(a.out + OTA_HDR_LEN, a.len - OTA_HDR_LEN, &b) == OTA_OK);
            CHECK(b.cycle == airsched_cycle(&as) && b.offset_us == c.retune_us);
            first_in_hop = false;
        }
        now += a.airtime_us + c.phy.tx_gap_us;
    }
    CHECK(airsched_queued(&as) == 0);
    CHECK(as.st.sent == 600 && as.st.dropped == 0);
}

static void test_queue_full(void) {
    struct airsched_cfg c = cfg_for(2600000, 40);
    c.queue_max = 4;
    airsched_init(&as, &c, 0);
    uint8_t pkt[100] = {0};
    for (int i = 0; i < 4; i++)
        CHECK(airsched_enqueue(&as, pkt, sizeof(pkt)) == 0);
    CHECK(airsched_enqueue(&as, pkt, sizeof(pkt)) == -1);
    CHECK(as.st.dropped == 1);
}

// Capacity printout: shard bytes per second each mode can carry.
static void test_print_capacity(void) {
    const uint32_t rates[4] = {520000, 260000, 2600000, 1300000};
    const uint8_t chans[4] = {10, 10, 40, 40};
    const char *names[4] = {"LR 520k", "LR 260k", "HQ 2.6M", "HQ 1.3M"};
    for (int m = 0; m < 4; m++) {
        struct airsched_cfg c = cfg_for(rates[m], chans[m]);
        airsched_init(&as, &c, 0);
        uint8_t pkt[OTA_MAX_PACKET] = {0};
        uint64_t now = 0, payload = 0;
        struct airsched_action a;
        while (now < 1000000) {
            while (airsched_queued(&as) < 8)
                airsched_enqueue(&as, pkt, OTA_MAX_PACKET);
            airsched_next(&as, now, &a);
            if (!a.send) {
                now = a.wake_us;
                continue;
            }
            if (a.len == OTA_MAX_PACKET)
                payload += OTA_MAX_PAYLOAD;
            now += a.airtime_us + c.phy.tx_gap_us;
        }
        unsigned per_hop;
        uint32_t best_bps;
        size_t best = airsched_best_packet(&c, &per_hop, &best_bps);
        // The best size must actually fit per_hop times into a hop.
        CHECK(best == 0 || c.retune_us + flrc_airtime_us(&c.phy, 24) + c.phy.tx_gap_us +
                                   per_hop * (flrc_airtime_us(&c.phy, best) + c.phy.tx_gap_us) -
                                   c.phy.tx_gap_us <=
                               c.hop_us);
        printf("  %-8s best packet %3zu B x %u/hop: %4u kbit/s of shards, %4u kbit/s video at "
               "RS 8/12\n",
               names[m], best, per_hop, best_bps / 1000, best_bps / 1000 * 8 / 12);
        printf("  %-8s CR3/4 255-byte packets: %3u pkt/s, %4.0f kbit/s of shards, %4.0f kbit/s "
               "video at RS 8/12\n",
               names[m], as.st.sent, (double)payload * 8 / 1000,
               (double)payload * 8 / 1000 * 8 / 12);
    }
}

int main(void) {
    T_RUN(test_airtime);
    T_RUN(test_schedule_rules);
    T_RUN(test_queue_full);
    T_RUN(test_print_capacity);
    T_DONE();
}
