// SPDX-License-Identifier: GPL-3.0-or-later
#include "h265.h"

#include <string.h>

// Returns offset of the next 00 00 01 at or after pos, or len.
static size_t find_start(const uint8_t *buf, size_t len, size_t pos) {
    for (size_t i = pos; i + 2 < len; i++)
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            return i;
    return len;
}

bool h265_next_nal(const uint8_t *buf, size_t len, size_t *pos, struct h265_nal *nal) {
    size_t sc = find_start(buf, len, *pos);
    if (sc + 3 >= len)
        return false;
    // Leading zeros (four-byte start codes, trailing_zero_8bits) go with this NAL.
    size_t start = sc;
    while (start > *pos && buf[start - 1] == 0)
        start--;
    size_t data = sc + 3;
    size_t end = find_start(buf, len, data);
    while (end > data && buf[end - 1] == 0)
        end--;
    nal->start = start;
    nal->data = data;
    nal->end = end;
    nal->type = (uint8_t)((buf[data] >> 1) & 0x3F);
    *pos = end;
    return true;
}

bool h265_first_slice(const uint8_t *buf, const struct h265_nal *nal) {
    if (nal->data + 2 >= nal->end)
        return false;
    return (buf[nal->data + 2] & 0x80) != 0;
}

void h265_split_aus(const uint8_t *buf, size_t len, h265_au_cb cb, void *ctx) {
    size_t pos = 0, au_start = 0;
    bool au_has_vcl = false, au_open = false;
    struct h265_nal nal;
    while (h265_next_nal(buf, len, &pos, &nal)) {
        bool begins_au = false;
        if (h265_is_vcl(nal.type))
            begins_au = au_has_vcl && h265_first_slice(buf, &nal);
        else if (nal.type == H265_NAL_AUD || nal.type == H265_NAL_VPS || nal.type == H265_NAL_SPS ||
                 nal.type == H265_NAL_PPS || nal.type == H265_NAL_SEI_PREFIX)
            begins_au = au_has_vcl;
        if (begins_au) {
            cb(ctx, buf + au_start, nal.start - au_start);
            au_start = nal.start;
            au_has_vcl = false;
        }
        if (!au_open) {
            au_start = nal.start;
            au_open = true;
        }
        if (h265_is_vcl(nal.type))
            au_has_vcl = true;
    }
    if (au_open && au_has_vcl)
        cb(ctx, buf + au_start, len - au_start);
}

static bool append(uint8_t *out, size_t cap, size_t *n, const uint8_t *src, size_t len) {
    if (*n + len > cap)
        return false;
    memcpy(out + *n, src, len);
    *n += len;
    return true;
}

static const uint8_t start_code[4] = {0, 0, 0, 1};

size_t h265_prepare_au(struct h265_ps_cache *ps, const uint8_t *au, size_t len, bool repeat,
                       uint8_t *out, size_t out_cap, bool *has_ps) {
    struct h265_nal nal;
    size_t pos = 0;
    bool found_ps = false;
    while (h265_next_nal(au, len, &pos, &nal)) {
        size_t body = nal.end - nal.data;
        uint8_t *dst = NULL;
        size_t *dst_len = NULL;
        if (nal.type == H265_NAL_VPS) {
            dst = ps->vps;
            dst_len = &ps->vps_len;
        } else if (nal.type == H265_NAL_SPS) {
            dst = ps->sps;
            dst_len = &ps->sps_len;
        } else if (nal.type == H265_NAL_PPS) {
            dst = ps->pps;
            dst_len = &ps->pps_len;
        }
        if (dst) {
            found_ps = true;
            if (body <= H265_PS_MAX) {
                memcpy(dst, au + nal.data, body);
                *dst_len = body;
            }
        }
    }

    size_t n = 0;
    if (repeat && !found_ps && ps->vps_len && ps->sps_len && ps->pps_len) {
        if (!append(out, out_cap, &n, start_code, 4) ||
            !append(out, out_cap, &n, ps->vps, ps->vps_len) ||
            !append(out, out_cap, &n, start_code, 4) ||
            !append(out, out_cap, &n, ps->sps, ps->sps_len) ||
            !append(out, out_cap, &n, start_code, 4) ||
            !append(out, out_cap, &n, ps->pps, ps->pps_len))
            return 0;
        found_ps = true;
    }
    pos = 0;
    while (h265_next_nal(au, len, &pos, &nal)) {
        if (nal.type == H265_NAL_AUD || nal.type == H265_NAL_SEI_PREFIX ||
            nal.type == H265_NAL_SEI_SUFFIX)
            continue;
        if (!append(out, out_cap, &n, start_code, 4) ||
            !append(out, out_cap, &n, au + nal.data, nal.end - nal.data))
            return 0;
    }
    if (has_ps)
        *has_ps = found_ps;
    return n;
}
