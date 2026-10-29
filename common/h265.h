// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal H.265 Annex-B helpers: NAL iteration, access-unit splitting and a
// parameter-set repeater (re-sends VPS/SPS/PPS so late joiners can decode).
#ifndef OLV_H265_H
#define OLV_H265_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    H265_NAL_IDR_W_RADL = 19,
    H265_NAL_IDR_N_LP = 20,
    H265_NAL_CRA = 21,
    H265_NAL_VPS = 32,
    H265_NAL_SPS = 33,
    H265_NAL_PPS = 34,
    H265_NAL_AUD = 35,
    H265_NAL_SEI_PREFIX = 39,
    H265_NAL_SEI_SUFFIX = 40,
};

struct h265_nal {
    size_t start; // offset of the start code
    size_t data;  // offset of the NAL header (after the start code)
    size_t end;   // offset one past the last byte
    uint8_t type;
};

// Finds the next NAL at or after *pos. Returns false at end of buffer.
bool h265_next_nal(const uint8_t *buf, size_t len, size_t *pos, struct h265_nal *nal);

static inline bool h265_is_vcl(uint8_t type) {
    return type < 32;
}
static inline bool h265_is_irap(uint8_t type) {
    return type >= 16 && type <= 23;
}

// first_slice_segment_in_pic_flag of a VCL NAL.
bool h265_first_slice(const uint8_t *buf, const struct h265_nal *nal);

// Splits a stream into access units. Calls cb once per AU with the byte range.
typedef void (*h265_au_cb)(void *ctx, const uint8_t *au, size_t len);
void h265_split_aus(const uint8_t *buf, size_t len, h265_au_cb cb, void *ctx);

// Rewrites one AU for transmission: drops AUD and SEI NALs (the frame number
// in the OTA header marks frame boundaries), and, when the AU has no VPS/SPS/
// PPS and repeat is set, prepends the cached ones. Caches parameter sets it
// sees. Returns the output length, or 0 if out_cap is too small.
#define H265_PS_MAX 256
struct h265_ps_cache {
    uint8_t vps[H265_PS_MAX], sps[H265_PS_MAX], pps[H265_PS_MAX];
    size_t vps_len, sps_len, pps_len;
};

size_t h265_prepare_au(struct h265_ps_cache *ps, const uint8_t *au, size_t len, bool repeat,
                       uint8_t *out, size_t out_cap, bool *has_ps);

#ifdef __cplusplus
}
#endif

#endif
