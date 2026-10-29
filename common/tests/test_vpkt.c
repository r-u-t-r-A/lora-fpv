// SPDX-License-Identifier: GPL-3.0-or-later
#include "h265.h"
#include "nal_fixture.h"
#include "t.h"
#include "vpkt.h"

#include <stdlib.h>
#include <string.h>

#define LINK 0x5A5A
#define MAX_PKTS 4096

struct pkt {
    uint8_t b[OTA_MAX_PACKET];
    size_t len;
};

static struct pkt pkts[MAX_PKTS];
static int n_pkts;

static void collect_pkt(void *ctx, const uint8_t *p, size_t len) {
    (void)ctx;
    REQUIRE(n_pkts < MAX_PKTS && len <= OTA_MAX_PACKET);
    memcpy(pkts[n_pkts].b, p, len);
    pkts[n_pkts].len = len;
    n_pkts++;
}

struct out_frame {
    uint8_t data[VPKT_MAX_FRAME];
    size_t len;
    struct vpkt_frame_info info;
};

static struct out_frame outs[64];
static int n_outs;

static void collect_frame(void *ctx, const uint8_t *au, size_t len,
                          const struct vpkt_frame_info *info) {
    (void)ctx;
    REQUIRE(n_outs < 64);
    if (len)
        memcpy(outs[n_outs].data, au, len);
    outs[n_outs].len = len;
    outs[n_outs].info = *info;
    n_outs++;
}

static struct vpkt_tx_cfg tx_cfg(void) {
    return (struct vpkt_tx_cfg){.link_id = LINK,
                                .mode = OTA_MODE_LR_520,
                                .k = 8,
                                .n = 12,
                                .max_shard = OTA_MAX_PAYLOAD,
                                .min_shard = 32};
}

static struct vpkt_rx *new_rx(void) {
    static struct vpkt_rx rx;
    struct vpkt_rx_cfg c = {.link_id = LINK, .max_wait_frames = 2};
    vpkt_rx_init(&rx, &c, collect_frame, NULL);
    n_outs = 0;
    return &rx;
}

// An AU: PS + several slices, one of them optionally larger than a block.
static size_t make_au(uint8_t *au, uint32_t seed, int slices, size_t slice_sz, size_t huge) {
    uint32_t rng = seed | 1;
    size_t p = 0;
    p = fx_nal(au, p, H265_NAL_VPS, 20, false, &rng);
    p = fx_nal(au, p, H265_NAL_SPS, 40, false, &rng);
    p = fx_nal(au, p, H265_NAL_PPS, 8, false, &rng);
    for (int i = 0; i < slices; i++)
        p = fx_nal(au, p, 1, slice_sz + (size_t)(olv_xorshift32(&rng) % 64), i == 0, &rng);
    if (huge)
        p = fx_nal(au, p, 1, huge, false, &rng);
    return p;
}

static void send_frame(struct vpkt_tx *tx, const uint8_t *au, size_t len) {
    n_pkts = 0;
    REQUIRE(vpkt_tx_frame(tx, au, len, collect_pkt, NULL) > 0);
}

static void test_roundtrip_sizes(void) {
    static uint8_t au[VPKT_MAX_FRAME];
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    struct vpkt_rx *rx = new_rx();
    const size_t slice_sizes[] = {10, 200, 1500, 1900, 3000};
    const size_t huge_sizes[] = {0, 0, 0, 5000, 30000};
    for (int i = 0; i < 5; i++) {
        size_t len = make_au(au, (uint32_t)i + 1, 3, slice_sizes[i], huge_sizes[i]);
        send_frame(&tx, au, len);
        // The first k shards of a block are new; the rest arrive after recovery.
        int news = 0;
        for (int j = 0; j < n_pkts; j++) {
            enum vpkt_push r = vpkt_rx_push(rx, pkts[j].b, pkts[j].len, 0);
            CHECK(r == VPKT_NEW || r == VPKT_DUP || r == VPKT_LATE);
            news += r == VPKT_NEW;
        }
        CHECK(news < n_pkts);
        REQUIRE(n_outs == i + 1);
        CHECK(outs[i].info.complete && !outs[i].info.lost && outs[i].info.has_idr);
        CHECK(outs[i].info.frame == i);
        CHECK(outs[i].len == len);
        CHECK(memcmp(outs[i].data, au, len) == 0);
    }
    vpkt_tx_free(&tx);
}

static void test_small_block_shrinks_k(void) {
    uint8_t au[64];
    uint32_t rng = 3;
    size_t len = fx_nal(au, 0, 1, 20, true, &rng); // 26 bytes
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    send_frame(&tx, au, len);
    CHECK(n_pkts == 2); // k = 1, n = 1 + ceil(1 * 4 / 8)
    struct ota_hdr h;
    REQUIRE(ota_hdr_unpack(pkts[0].b, pkts[0].len, LINK, &h) == OTA_OK);
    CHECK(h.k == 1 && h.n == 2 && (h.flags & OTA_FLAG_EOF));
    // Parity alone recovers it.
    struct vpkt_rx *rx = new_rx();
    CHECK(vpkt_rx_push(rx, pkts[1].b, pkts[1].len, 0) == VPKT_NEW);
    REQUIRE(n_outs == 1);
    CHECK(outs[0].len == len && memcmp(outs[0].data, au, len) == 0);
    vpkt_tx_free(&tx);
}

static void test_shard_loss_within_budget(void) {
    static uint8_t au[VPKT_MAX_FRAME];
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    struct vpkt_rx *rx = new_rx();
    uint32_t rng = 77;
    for (int f = 0; f < 20; f++) {
        size_t len = make_au(au, (uint32_t)f + 100, 4, 900, f % 3 ? 0 : 4000);
        send_frame(&tx, au, len);
        // Drop up to n-k shards of every block, chosen at random.
        int dropped[256] = {0};
        for (int j = 0; j < n_pkts; j++) {
            struct ota_hdr h;
            REQUIRE(ota_hdr_unpack(pkts[j].b, pkts[j].len, LINK, &h) == OTA_OK);
            if (dropped[h.block] < h.n - h.k && olv_xorshift32(&rng) % 3 == 0) {
                dropped[h.block]++;
                continue;
            }
            vpkt_rx_push(rx, pkts[j].b, pkts[j].len, 0);
        }
        REQUIRE(n_outs == f + 1);
        CHECK(outs[f].info.complete);
        CHECK(outs[f].len == len && memcmp(outs[f].data, au, len) == 0);
    }
    vpkt_tx_free(&tx);
}

// Byte range of each block, read back from the packets (data shards, in order).
static int block_ranges(size_t *off, size_t *blen) {
    int nb = 0;
    static uint8_t blk[256][FEC_MAX_N * OTA_MAX_PAYLOAD];
    for (int j = 0; j < n_pkts; j++) {
        struct ota_hdr h;
        REQUIRE(ota_hdr_unpack(pkts[j].b, pkts[j].len, LINK, &h) == OTA_OK);
        size_t sz = pkts[j].len - OTA_HDR_LEN;
        if (h.shard < h.k)
            memcpy(blk[h.block] + h.shard * sz, pkts[j].b + OTA_HDR_LEN, sz);
        if (h.block + 1 > nb)
            nb = h.block + 1;
    }
    size_t pos = 0;
    for (int b = 0; b < nb; b++) {
        off[b] = pos;
        blen[b] = olv_get_u16(blk[b]) & VPKT_LEN_MASK;
        pos += blen[b];
    }
    return nb;
}

static void test_lost_block_keeps_whole_nals(void) {
    static uint8_t au[VPKT_MAX_FRAME], want[VPKT_MAX_FRAME];
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    // Slices of ~700 bytes plus a 6000-byte NAL spanning four blocks.
    size_t len = make_au(au, 9, 6, 700, 6000);
    send_frame(&tx, au, len);
    size_t off[256], blen[256];
    int nb = block_ranges(off, blen);
    REQUIRE(nb >= 6);
    int saved_n = n_pkts;
    static struct pkt saved[MAX_PKTS];
    memcpy(saved, pkts, sizeof(struct pkt) * (size_t)saved_n);

    for (int lost = 0; lost < nb; lost++) {
        struct vpkt_rx *rx = new_rx();
        for (int j = 0; j < saved_n; j++) {
            if (saved[j].b[8] == lost) // block field
                continue;
            vpkt_rx_push(rx, saved[j].b, saved[j].len, 0);
        }
        vpkt_rx_flush(rx);
        REQUIRE(n_outs == 1);
        CHECK(!outs[0].info.complete && outs[0].info.blocks_ok == nb - 1);
        // Expected: every NAL that does not overlap the lost block's range.
        size_t lo = off[lost], hi = off[lost] + blen[lost], w = 0, p = 0;
        struct h265_nal nal;
        while (h265_next_nal(au, len, &p, &nal)) {
            size_t ns = nal.start, ne = nal.end;
            if (ne <= lo || ns >= hi) {
                memcpy(want + w, au + ns, ne - ns);
                w += ne - ns;
            }
        }
        CHECK(outs[0].len == w);
        CHECK(memcmp(outs[0].data, want, w) == 0);
    }
    vpkt_tx_free(&tx);
}

static void test_two_receivers_merge(void) {
    static uint8_t au[VPKT_MAX_FRAME];
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    size_t len = make_au(au, 21, 3, 1200, 0);
    send_frame(&tx, au, len);
    // RX A hears shards 0-5, RX B hears 6-11: neither alone has k = 8.
    for (int mode = 0; mode < 3; mode++) {
        struct vpkt_rx *rx = new_rx();
        int news[2] = {0, 0};
        for (int j = 0; j < n_pkts; j++) {
            uint8_t shard = pkts[j].b[9];
            bool a = shard < 6 || shard == 7, b = shard >= 6;
            if ((mode == 0 || mode == 2) && a)
                news[0] += vpkt_rx_push(rx, pkts[j].b, pkts[j].len, 0) == VPKT_NEW;
            if ((mode == 1 || mode == 2) && b)
                news[1] += vpkt_rx_push(rx, pkts[j].b, pkts[j].len, 0) == VPKT_NEW;
        }
        vpkt_rx_flush(rx);
        REQUIRE(n_outs == 1);
        if (mode == 2) {
            CHECK(outs[0].info.complete);
            CHECK(outs[0].len == len && memcmp(outs[0].data, au, len) == 0);
            CHECK(news[0] > 0 && news[1] > 0);
        } else {
            CHECK(!outs[0].info.complete);
        }
    }
    vpkt_tx_free(&tx);
}

static void test_dup_late_lost_order(void) {
    static uint8_t au[5][4096];
    size_t len[5];
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    struct vpkt_rx *rx = new_rx();
    struct pkt first_of_0;
    for (int f = 0; f < 5; f++) {
        len[f] = make_au(au[f], (uint32_t)f + 50, 1, 300, 0);
        send_frame(&tx, au[f], len[f]);
        if (f == 0)
            first_of_0 = pkts[0];
        if (f == 2)
            continue; // frame 2 never arrives
        for (int j = 0; j < n_pkts; j++) {
            enum vpkt_push r = vpkt_rx_push(rx, pkts[j].b, pkts[j].len, 0);
            CHECK(j > 0 || r == VPKT_NEW);
            // A second copy (another VRX) is never new.
            r = vpkt_rx_push(rx, pkts[j].b, pkts[j].len, 0);
            CHECK(r == VPKT_DUP || r == VPKT_LATE);
        }
    }
    vpkt_rx_flush(rx);
    REQUIRE(n_outs == 5);
    for (int f = 0; f < 5; f++) {
        CHECK(outs[f].info.frame == f);
        CHECK(outs[f].info.lost == (f == 2));
        if (f != 2)
            CHECK(outs[f].len == len[f] && memcmp(outs[f].data, au[f], len[f]) == 0);
    }
    CHECK(vpkt_rx_push(rx, first_of_0.b, first_of_0.len, 0) == VPKT_LATE);
    CHECK(rx->st.frames_lost == 1 && rx->st.frames_complete == 4);

    // Wrong link id is rejected.
    struct pkt p = first_of_0;
    p.b[0] ^= 1;
    CHECK(vpkt_rx_push(rx, p.b, p.len, 0) == VPKT_BAD);
    vpkt_tx_free(&tx);
}

static void test_timeout(void) {
    static uint8_t au[4096];
    struct vpkt_tx tx;
    struct vpkt_tx_cfg c = tx_cfg();
    REQUIRE(vpkt_tx_init(&tx, &c) == 0);
    static struct vpkt_rx rx;
    struct vpkt_rx_cfg rc = {.link_id = LINK, .max_wait_frames = 8, .timeout_us = 50000};
    vpkt_rx_init(&rx, &rc, collect_frame, NULL);
    n_outs = 0;
    size_t len = make_au(au, 5, 1, 1500, 0);
    send_frame(&tx, au, len);
    for (int j = 0; j < 3; j++) // not enough to recover
        vpkt_rx_push(&rx, pkts[j].b, pkts[j].len, 1000);
    vpkt_rx_tick(&rx, 40000);
    CHECK(n_outs == 0);
    vpkt_rx_tick(&rx, 51000);
    REQUIRE(n_outs == 1);
    CHECK(!outs[0].info.complete && outs[0].len == 0);
    vpkt_tx_free(&tx);
}

int main(void) {
    T_RUN(test_roundtrip_sizes);
    T_RUN(test_small_block_shrinks_k);
    T_RUN(test_shard_loss_within_budget);
    T_RUN(test_lost_block_keeps_whole_nals);
    T_RUN(test_two_receivers_merge);
    T_RUN(test_dup_late_lost_order);
    T_RUN(test_timeout);
    fec_free_cache();
    T_DONE();
}
