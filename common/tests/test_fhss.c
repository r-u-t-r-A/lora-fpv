// SPDX-License-Identifier: GPL-3.0-or-later
#include "fhss.h"
#include "t.h"

#include <string.h>

static void test_every_channel_once(void) {
    for (int n = 1; n <= FHSS_MAX_CHANNELS; n++) {
        for (uint32_t cycle = 0; cycle < 200; cycle++) {
            uint8_t perm[FHSS_MAX_CHANNELS];
            fhss_permutation(0x12345678u + (uint32_t)n, cycle, (uint8_t)n, perm);
            int seen[FHSS_MAX_CHANNELS] = {0};
            for (int i = 0; i < n; i++) {
                REQUIRE(perm[i] < n);
                seen[perm[i]]++;
            }
            for (int i = 0; i < n; i++)
                CHECK(seen[i] == 1);
        }
    }
}

// Vectors from an independent Python implementation (see docs/protocol.md);
// web/src/fhss.ts checks the same ones.
static void test_reference_vectors(void) {
    uint32_t seed = fhss_seed("open-lr-video");
    CHECK(seed == 0xf53d2da0u);
    const uint8_t c0[10] = {6, 9, 7, 1, 3, 5, 0, 2, 8, 4};
    const uint8_t c1[10] = {8, 9, 0, 3, 4, 2, 6, 7, 1, 5};
    const uint8_t c12345[10] = {0, 3, 8, 9, 1, 7, 4, 2, 5, 6};
    const uint8_t hq7[40] = {9,  25, 18, 23, 20, 33, 28, 14, 27, 0,  4,  12, 16, 35,
                             34, 24, 15, 38, 39, 26, 37, 30, 32, 6,  31, 8,  1,  29,
                             21, 22, 17, 36, 5,  3,  19, 10, 2,  13, 7,  11};
    uint8_t p[FHSS_MAX_CHANNELS];
    fhss_permutation(seed, 0, 10, p);
    CHECK(memcmp(p, c0, 10) == 0);
    fhss_permutation(seed, 1, 10, p);
    CHECK(memcmp(p, c1, 10) == 0);
    fhss_permutation(seed, 12345, 10, p);
    CHECK(memcmp(p, c12345, 10) == 0);
    fhss_permutation(seed, 7, 40, p);
    CHECK(memcmp(p, hq7, 40) == 0);
}

static void test_air_ground_identical(void) {
    // Two independent instances (air and ground) with the same bind phrase.
    struct fhss_seq air, gnd;
    fhss_seq_init(&air, fhss_seed("my bind phrase"), 40);
    fhss_seq_init(&gnd, fhss_seed("my bind phrase"), 40);
    for (uint32_t abs = 0; abs < 40 * 5000; abs++) {
        uint8_t a = fhss_seq_channel(&air, abs / 40, (uint8_t)(abs % 40));
        // Ground jumps around (cache misses) and must still agree.
        uint32_t other = (abs * 2654435761u) % (40 * 5000);
        (void)fhss_seq_channel(&gnd, other / 40, (uint8_t)(other % 40));
        CHECK(a == fhss_seq_channel(&gnd, abs / 40, (uint8_t)(abs % 40)));
    }
    // Different bind phrase: sequences must differ.
    struct fhss_seq x;
    fhss_seq_init(&x, fhss_seed("another phrase"), 40);
    int same = 0;
    for (uint8_t h = 0; h < 40; h++)
        same += fhss_seq_channel(&x, 0, h) == fhss_seq_channel(&air, 0, h);
    CHECK(same < 10);
    // Consecutive cycles differ.
    int diff = 0;
    for (uint8_t h = 0; h < 40; h++)
        diff += fhss_seq_channel(&air, 10, h) != fhss_seq_channel(&air, 11, h);
    CHECK(diff > 20);
}

static void test_bands(void) {
    struct fhss_band lr = FHSS_BAND_LR_868, hq = FHSS_BAND_HQ_2G4;
    CHECK(fhss_freq_hz(&lr, 0) == 863500000u);
    CHECK(fhss_freq_hz(&lr, 9) == 869350000u);
    CHECK(fhss_freq_hz(&hq, 0) == 2402000000u);
    CHECK(fhss_freq_hz(&hq, 39) == 2480000000u);
}

// ---- RX sync, simulated in microsecond steps against a drifting TX clock ----

#define HOP_US 10000u
#define BEACON_OFF 200u
#define BEACON_AIR 500u
#define STEP 50u

struct link_sim {
    uint32_t seed;
    uint8_t n;
    double ppm;                      // RX clock drift relative to TX
    uint32_t rx_offset;              // RX clock offset
    uint32_t outage_from, outage_to; // TX-time window with no reception
    uint32_t beacon_drop_every;      // drop every Nth beacon (0 = none)
};

static uint32_t rx_clock(const struct link_sim *L, uint64_t t_tx) {
    return (uint32_t)(L->rx_offset + t_tx + (uint64_t)((double)t_tx * L->ppm / 1e6));
}

// Runs the link for dur_us of TX time. Returns the TX time of the first lock
// (UINT32_MAX if never). Counts hops where the RX was on the wrong channel
// while LOCKED into *wrong.
static uint32_t run_link(const struct link_sim *L, struct fhss_rx *rx, uint64_t t_from,
                         uint64_t dur_us, uint32_t *wrong) {
    struct fhss_seq txs;
    fhss_seq_init(&txs, L->seed, L->n);
    uint32_t first_lock = UINT32_MAX;
    // Packet completion offsets within a hop: beacon, then three video packets.
    const uint32_t done_at[4] = {BEACON_OFF + BEACON_AIR, 2500, 5000, 7500};
    for (uint64_t t = t_from; t < t_from + dur_us; t += STEP) {
        uint64_t abs_hop = t / HOP_US;
        uint32_t in_hop = (uint32_t)(t % HOP_US);
        uint32_t cycle = (uint32_t)(abs_hop / L->n);
        uint8_t hop = (uint8_t)(abs_hop % L->n);
        uint8_t tx_ch = fhss_seq_channel(&txs, cycle, hop);
        uint32_t now = rx_clock(L, t);
        uint8_t rx_ch = fhss_rx_poll(rx, now, NULL);
        if (rx->state == FHSS_RX_LOCKED && in_hop == HOP_US / 2 && rx_ch != tx_ch && wrong)
            (*wrong)++;
        bool out = t >= L->outage_from && t < L->outage_to;
        for (int i = 0; i < 4; i++) {
            if (in_hop != done_at[i] || out || rx_ch != tx_ch)
                continue;
            if (i == 0) {
                if (L->beacon_drop_every && abs_hop % L->beacon_drop_every == 0)
                    continue;
                uint32_t hop_start = now - BEACON_AIR - BEACON_OFF;
                fhss_rx_on_beacon(rx, cycle, hop, hop_start);
            } else {
                fhss_rx_on_packet(rx, hop);
            }
            if (rx->state == FHSS_RX_LOCKED && first_lock == UINT32_MAX)
                first_lock = (uint32_t)t;
        }
    }
    return first_lock;
}

static void test_rx_acquire(void) {
    const uint8_t sizes[2] = {10, 40};
    for (int si = 0; si < 2; si++) {
        for (uint32_t trial = 0; trial < 200; trial++) {
            struct link_sim L = {.seed = fhss_seed("acq") + trial,
                                 .n = sizes[si],
                                 .ppm = 40.0,
                                 .rx_offset = 123456789u * trial};
            struct fhss_rx rx;
            struct fhss_rx_cfg cfg = fhss_rx_default_cfg(L.n, HOP_US);
            uint64_t t0 = 1000000ull + trial * 3750u; // multiple of STEP
            fhss_rx_init(&rx, L.seed, &cfg, rx_clock(&L, t0));
            rx.park_ch = (uint8_t)(trial % L.n);
            uint32_t wrong = 0;
            uint32_t lock = run_link(&L, &rx, t0, 3000000, &wrong);
            REQUIRE(lock != UINT32_MAX);
            // Gate: lock in under 1 s; by construction within 2n hops.
            CHECK(lock - t0 <= 2u * L.n * HOP_US);
            CHECK(lock - t0 < 1000000u);
            CHECK(rx.state == FHSS_RX_LOCKED);
            CHECK(wrong == 0);
            CHECK(rx.locks == 1);
        }
    }
}

static void test_rx_tracks_with_beacon_loss(void) {
    struct link_sim L = {.seed = 42, .n = 40, .ppm = -60.0, .rx_offset = 5, .beacon_drop_every = 2};
    struct fhss_rx rx;
    struct fhss_rx_cfg cfg = fhss_rx_default_cfg(L.n, HOP_US);
    fhss_rx_init(&rx, L.seed, &cfg, rx_clock(&L, 0));
    uint32_t wrong = 0;
    run_link(&L, &rx, 0, 20000000, &wrong); // 20 s
    CHECK(rx.state == FHSS_RX_LOCKED);
    CHECK(rx.locks == 1);
    CHECK(wrong == 0);
}

static void test_rx_coast_and_repark(void) {
    // Short outage (100 ms): coasts and re-locks without re-parking.
    struct link_sim L = {
        .seed = 7, .n = 10, .ppm = 20.0, .outage_from = 2000000, .outage_to = 2100000};
    struct fhss_rx rx;
    struct fhss_rx_cfg cfg = fhss_rx_default_cfg(L.n, HOP_US);
    fhss_rx_init(&rx, L.seed, &cfg, rx_clock(&L, 0));
    run_link(&L, &rx, 0, 2050000, NULL);
    CHECK(rx.state == FHSS_RX_COASTING);
    run_link(&L, &rx, 2050000, 1000000, NULL);
    CHECK(rx.state == FHSS_RX_LOCKED);
    CHECK(rx.locks == 1);

    // Long outage (3 s): falls back to PARKED, then re-acquires.
    L.outage_from = 4000000;
    L.outage_to = 7000000;
    run_link(&L, &rx, 3050000, 2500000, NULL); // up to t = 5.55 s
    CHECK(rx.state == FHSS_RX_PARKED);
    uint32_t lock = run_link(&L, &rx, 5550000, 3000000, NULL);
    CHECK(rx.state == FHSS_RX_LOCKED);
    CHECK(rx.locks == 2);
    CHECK(lock >= 7000000u && lock - 7000000u < 1000000u);
}

static void test_rx_park_timeout(void) {
    struct fhss_rx rx;
    struct fhss_rx_cfg cfg = fhss_rx_default_cfg(10, HOP_US);
    fhss_rx_init(&rx, 1, &cfg, 0);
    bool changed = false;
    uint8_t ch = fhss_rx_poll(&rx, cfg.park_us - 1, &changed);
    CHECK(ch == 0 && !changed);
    ch = fhss_rx_poll(&rx, cfg.park_us, &changed);
    CHECK(ch == 1 && changed && rx.park_timeouts == 1);
    CHECK(fhss_rx_next_event_us(&rx) == 2 * cfg.park_us);
    // A beacon that does not match the parked channel is rejected.
    struct fhss_seq s;
    fhss_seq_init(&s, 1, 10);
    uint8_t wrong_hop = 0;
    while (fhss_seq_channel(&s, 5, wrong_hop) == rx.park_ch)
        wrong_hop++;
    CHECK(!fhss_rx_on_beacon(&rx, 5, wrong_hop, cfg.park_us));
    CHECK(rx.state == FHSS_RX_PARKED);
}

int main(void) {
    T_RUN(test_every_channel_once);
    T_RUN(test_reference_vectors);
    T_RUN(test_air_ground_identical);
    T_RUN(test_bands);
    T_RUN(test_rx_acquire);
    T_RUN(test_rx_tracks_with_beacon_loss);
    T_RUN(test_rx_coast_and_repark);
    T_RUN(test_rx_park_timeout);
    T_DONE();
}
