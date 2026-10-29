// SPDX-License-Identifier: GPL-3.0-or-later
#include "fec.h"

#include <string.h>

#include "zfex.h"

// zfex builds one encode matrix per (k, n). Blocks use a handful of distinct
// shapes, so keep them all; tables are created on first use (not in any
// firmware hot path: FEC runs on the Luckfox and in the browser only).
static fec_t *codecs[FEC_MAX_N + 1][FEC_MAX_N + 1];

static fec_t *get_codec(uint8_t k, uint8_t n) {
    if (k == 0 || n > FEC_MAX_N || k > n)
        return NULL;
    if (!codecs[k][n]) {
        fec_t *f = NULL;
        if (fec_new(k, n, &f) != ZFEX_SC_OK)
            return NULL;
        codecs[k][n] = f;
    }
    return codecs[k][n];
}

void fec_free_cache(void) {
    for (int k = 0; k <= FEC_MAX_N; k++)
        for (int n = 0; n <= FEC_MAX_N; n++)
            if (codecs[k][n]) {
                fec_free(codecs[k][n]);
                codecs[k][n] = NULL;
            }
}

int fec_encode_parity(uint8_t k, uint8_t n, const uint8_t *const *data, uint8_t *const *parity,
                      size_t sz) {
    fec_t *f = get_codec(k, n);
    if (!f)
        return -1;
    if (n == k)
        return 0;
    unsigned nums[FEC_MAX_N];
    for (unsigned i = 0; i < (unsigned)(n - k); i++)
        nums[i] = k + i;
    return fec_encode(f, (const gf *const *)data, (gf *const *)parity, nums, (size_t)(n - k), sz) ==
                   ZFEX_SC_OK
               ? 0
               : -1;
}

int fec_recover(uint8_t k, uint8_t n, const uint8_t *const *shards, uint8_t *const *data_out,
                size_t sz) {
    fec_t *f = get_codec(k, n);
    if (!f)
        return -1;

    // zfex wants k input packets with data shard i at slot i, parity shards in
    // the slots of missing data shards.
    const gf *in[FEC_MAX_N];
    unsigned index[FEC_MAX_N];
    uint8_t missing[FEC_MAX_N];
    int n_missing = 0;
    for (uint8_t i = 0; i < k; i++) {
        if (shards[i]) {
            in[i] = shards[i];
            index[i] = i;
        } else {
            missing[n_missing++] = i;
        }
    }
    uint8_t next_parity = k;
    for (int m = 0; m < n_missing; m++) {
        while (next_parity < n && !shards[next_parity])
            next_parity++;
        if (next_parity >= n)
            return -1;
        in[missing[m]] = shards[next_parity];
        index[missing[m]] = next_parity;
        next_parity++;
    }

    for (uint8_t i = 0; i < k; i++)
        if (shards[i] && data_out[i] != shards[i])
            memcpy(data_out[i], shards[i], sz);
    if (n_missing == 0)
        return 0;

    gf *out[FEC_MAX_N];
    for (int m = 0; m < n_missing; m++)
        out[m] = data_out[missing[m]];
    return fec_decode(f, in, out, index, sz) == ZFEX_SC_OK ? 0 : -1;
}
