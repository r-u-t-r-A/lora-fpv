// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal IVF writer/reader. IVF carries one access unit per record with a
// timestamp, so lost frames leave gaps instead of shifting later frames.
// ffmpeg reads it with "-c:v hevc -i file.ivf".
#ifndef OLV_SIM_IVF_H
#define OLV_SIM_IVF_H

#include <stdint.h>
#include <stdio.h>

#include "olv_util.h"

static inline void ivf_write_header(FILE *f, uint16_t w, uint16_t h, uint32_t fps,
                                    uint32_t frames) {
    uint8_t b[32] = {'D', 'K', 'I', 'F', 0, 0, 32, 0, 'H', '2', '6', '5'};
    olv_put_u16(b + 12, w);
    olv_put_u16(b + 14, h);
    olv_put_u32(b + 16, fps);
    olv_put_u32(b + 20, 1);
    olv_put_u32(b + 24, frames);
    fwrite(b, 1, sizeof(b), f);
}

static inline void ivf_write_frame(FILE *f, uint64_t pts, const uint8_t *data, uint32_t len) {
    uint8_t b[12];
    olv_put_u32(b, len);
    olv_put_u32(b + 4, (uint32_t)pts);
    olv_put_u32(b + 8, (uint32_t)(pts >> 32));
    fwrite(b, 1, sizeof(b), f);
    fwrite(data, 1, len, f);
}

#endif
