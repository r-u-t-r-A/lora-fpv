// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by firmware and host code: little-endian access,
// FNV-1a, xorshift32. Header-only, no libc beyond <stdint.h>/<stddef.h>.
#ifndef OLV_UTIL_H
#define OLV_UTIL_H

#include <stddef.h>
#include <stdint.h>

static inline void olv_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void olv_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t olv_get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t olv_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// FNV-1a, 32 bit, over a NUL-terminated string.
static inline uint32_t olv_fnv1a32(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

// xorshift32 (Marsaglia). State must be non-zero.
static inline uint32_t olv_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#endif
