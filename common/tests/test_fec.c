// SPDX-License-Identifier: GPL-3.0-or-later
#include "fec.h"
#include "olv_util.h"
#include "t.h"

#include <string.h>

#define SZ 243

static uint8_t shards[FEC_MAX_N][SZ];
static uint8_t out[FEC_MAX_N][SZ];

static void make_block(uint8_t k, uint8_t n, uint32_t seed) {
    uint32_t st = seed | 1;
    for (int i = 0; i < k; i++)
        for (int j = 0; j < SZ; j++)
            shards[i][j] = (uint8_t)olv_xorshift32(&st);
    const uint8_t *data[FEC_MAX_N];
    uint8_t *par[FEC_MAX_N];
    for (int i = 0; i < k; i++)
        data[i] = shards[i];
    for (int i = k; i < n; i++)
        par[i - k] = shards[i];
    REQUIRE(fec_encode_parity(k, n, data, par, SZ) == 0);
}

// Tries recovery with the shards whose bit is set in mask present.
static int recover_mask(uint8_t k, uint8_t n, uint32_t mask) {
    const uint8_t *in[FEC_MAX_N];
    uint8_t *o[FEC_MAX_N];
    for (int i = 0; i < n; i++)
        in[i] = (mask & (1u << i)) ? shards[i] : NULL;
    for (int i = 0; i < k; i++)
        o[i] = out[i];
    memset(out, 0, sizeof(out));
    int r = fec_recover(k, n, in, o, SZ);
    if (r != 0)
        return r;
    for (int i = 0; i < k; i++)
        if (memcmp(out[i], shards[i], SZ) != 0)
            return -2;
    return 0;
}

static void test_8_of_12_all_patterns(void) {
    make_block(8, 12, 1);
    int tried = 0;
    for (uint32_t mask = 0; mask < (1u << 12); mask++) {
        int present = __builtin_popcount(mask);
        int r = recover_mask(8, 12, mask);
        if (present >= 8) {
            CHECK(r == 0);
            tried++;
        } else {
            CHECK(r == -1);
        }
    }
    CHECK(tried == 794); // sum C(12, 8..12)
}

static void test_random_shapes(void) {
    uint32_t st = 99;
    for (int trial = 0; trial < 300; trial++) {
        uint8_t n = (uint8_t)(1 + olv_xorshift32(&st) % FEC_MAX_N);
        uint8_t k = (uint8_t)(1 + olv_xorshift32(&st) % n);
        make_block(k, n, (uint32_t)trial);
        // Pick exactly k random shards.
        uint32_t mask = 0;
        while (__builtin_popcount(mask) < k)
            mask |= 1u << (olv_xorshift32(&st) % n);
        CHECK(recover_mask(k, n, mask) == 0);
    }
}

static void test_parity_only_block(void) {
    // All data lost, recovery from parity only.
    make_block(4, 8, 5);
    CHECK(recover_mask(4, 8, 0xF0) == 0);
}

static void test_bad_args(void) {
    const uint8_t *in[FEC_MAX_N] = {0};
    uint8_t *o[FEC_MAX_N] = {0};
    CHECK(fec_recover(0, 4, in, o, SZ) == -1);
    CHECK(fec_recover(5, 4, in, o, SZ) == -1);
    CHECK(fec_recover(4, FEC_MAX_N + 1, in, o, SZ) == -1);
}

int main(void) {
    T_RUN(test_8_of_12_all_patterns);
    T_RUN(test_random_shapes);
    T_RUN(test_parity_only_block);
    T_RUN(test_bad_args);
    fec_free_cache();
    T_DONE();
}
