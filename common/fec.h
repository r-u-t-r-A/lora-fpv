// SPDX-License-Identifier: GPL-3.0-or-later
// Reed-Solomon erasure coding over zfex. A block has k data shards and n-k
// parity shards, all of the same size; any k of the n recover the data.
#ifndef OLV_FEC_H
#define OLV_FEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FEC_MAX_N 32

// parity[i] receives shard k+i, for i in 0..n-k-1. Returns 0 on success.
int fec_encode_parity(uint8_t k, uint8_t n, const uint8_t *const *data, uint8_t *const *parity,
                      size_t sz);

// shards[i] is shard i or NULL if it was lost (i in 0..n-1). On success, all k
// data shards are written to data_out[0..k-1] (present data shards are copied).
// Returns 0 on success, -1 if fewer than k shards are present or on bad args.
int fec_recover(uint8_t k, uint8_t n, const uint8_t *const *shards, uint8_t *const *data_out,
                size_t sz);

// Releases cached codec tables (tests and leak checkers only).
void fec_free_cache(void);

#ifdef __cplusplus
}
#endif

#endif
