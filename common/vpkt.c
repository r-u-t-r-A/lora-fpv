// SPDX-License-Identifier: GPL-3.0-or-later
#include "vpkt.h"
#include "h265.h"
#include "olv_util.h"

#include <stdlib.h>
#include <string.h>

#define MAX_NALS 512

static inline size_t div_up(size_t a, size_t b) {
    return (a + b - 1) / b;
}

// ---------------------------------------------------------------- packetizer

int vpkt_tx_init(struct vpkt_tx *tx, const struct vpkt_tx_cfg *cfg) {
    memset(tx, 0, sizeof(*tx));
    if (cfg->k == 0 || cfg->n < cfg->k || cfg->n > FEC_MAX_N || cfg->max_shard == 0 ||
        cfg->max_shard > OTA_MAX_PAYLOAD || (size_t)cfg->k * cfg->max_shard <= VPKT_BLK_HDR)
        return -1;
    tx->cfg = *cfg;
    if (tx->cfg.min_shard > tx->cfg.max_shard)
        tx->cfg.min_shard = tx->cfg.max_shard;
    tx->pool = malloc((size_t)VPKT_TX_MAX_BLOCKS * FEC_MAX_N * OTA_MAX_PACKET);
    return tx->pool ? 0 : -1;
}

void vpkt_tx_free(struct vpkt_tx *tx) {
    free(tx->pool);
    tx->pool = NULL;
}

struct blk_plan {
    size_t off, len;
    uint16_t hdr;
    uint16_t sz;
    uint8_t k, n;
    uint8_t flags; // OTA_FLAG_IDR / OTA_FLAG_EOF
};

static bool nal_is_key(uint8_t type) {
    return h265_is_irap(type) || type == H265_NAL_VPS || type == H265_NAL_SPS ||
           type == H265_NAL_PPS;
}

static uint8_t *pool_pkt(struct vpkt_tx *tx, int b, int s) {
    return tx->pool + ((size_t)b * FEC_MAX_N + (size_t)s) * OTA_MAX_PACKET;
}

int vpkt_tx_frame(struct vpkt_tx *tx, const uint8_t *au, size_t len, vpkt_emit_cb emit, void *ctx) {
    const struct vpkt_tx_cfg *c = &tx->cfg;
    if (len == 0 || len > VPKT_MAX_FRAME)
        return -1;

    // NAL start offsets and types; a sentinel at len closes the last one.
    size_t nal_start[MAX_NALS + 1];
    uint8_t nal_type[MAX_NALS];
    int n_nals = 0;
    struct h265_nal nal;
    size_t p = 0;
    while (n_nals < MAX_NALS && h265_next_nal(au, len, &p, &nal)) {
        nal_start[n_nals] = nal.start;
        nal_type[n_nals] = nal.type;
        n_nals++;
    }
    nal_start[n_nals] = len;

    // Plan the blocks: cut at the last NAL start that fits, fragment if none.
    const size_t cap = vpkt_block_capacity(c);
    struct blk_plan plan[VPKT_TX_MAX_BLOCKS];
    int nb = 0;
    size_t pos = 0;
    int ni = 0; // first NAL whose start is >= pos
    while (pos < len) {
        if (nb == VPKT_TX_MAX_BLOCKS)
            return -1;
        while (ni < n_nals && nal_start[ni] < pos)
            ni++;
        size_t chunk = len - pos;
        if (chunk > cap) {
            size_t best = 0;
            for (int i = ni; i < n_nals && nal_start[i] <= pos + cap; i++)
                if (nal_start[i] > pos)
                    best = nal_start[i];
            chunk = best ? best - pos : cap;
        }
        size_t end = pos + chunk;
        bool starts_on_nal = ni < n_nals && nal_start[ni] == pos;
        bool ends_on_nal = end == len;
        uint8_t flags = 0;
        // Key NALs touching [pos, end): the one containing pos and all starting inside.
        for (int i = (starts_on_nal || ni == 0) ? ni : ni - 1; i < n_nals && nal_start[i] < end;
             i++)
            if (nal_is_key(nal_type[i]))
                flags |= OTA_FLAG_IDR;
        for (int i = ni; i < n_nals && nal_start[i] <= end; i++)
            if (nal_start[i] == end)
                ends_on_nal = true;

        struct blk_plan *bp = &plan[nb++];
        bp->off = pos;
        bp->len = chunk;
        bp->hdr =
            (uint16_t)(chunk | (starts_on_nal ? 0 : VPKT_CONT) | (ends_on_nal ? 0 : VPKT_MORE));
        size_t payload = chunk + VPKT_BLK_HDR;
        size_t k = c->k;
        size_t sz = div_up(payload, k);
        if (sz < c->min_shard) {
            k = div_up(payload, c->min_shard);
            if (k == 0)
                k = 1;
            sz = div_up(payload, k);
        }
        bp->k = (uint8_t)k;
        bp->n = (uint8_t)(k + div_up(k * (size_t)(c->n - c->k), c->k));
        bp->sz = (uint16_t)sz;
        bp->flags = flags;
        pos = end;
    }
    plan[nb - 1].flags |= OTA_FLAG_EOF;

    // Build data shards and parity for every block.
    uint8_t maxn = 0;
    for (int b = 0; b < nb; b++) {
        struct blk_plan *bp = &plan[b];
        uint8_t blk[FEC_MAX_N * OTA_MAX_PAYLOAD];
        size_t total = (size_t)bp->k * bp->sz;
        olv_put_u16(blk, bp->hdr);
        memcpy(blk + VPKT_BLK_HDR, au + bp->off, bp->len);
        memset(blk + VPKT_BLK_HDR + bp->len, 0, total - VPKT_BLK_HDR - bp->len);
        const uint8_t *data[FEC_MAX_N];
        uint8_t *parity[FEC_MAX_N];
        for (int s = 0; s < bp->k; s++) {
            uint8_t *dst = pool_pkt(tx, b, s) + OTA_HDR_LEN;
            memcpy(dst, blk + (size_t)s * bp->sz, bp->sz);
            data[s] = dst;
        }
        for (int s = bp->k; s < bp->n; s++)
            parity[s - bp->k] = pool_pkt(tx, b, s) + OTA_HDR_LEN;
        if (fec_encode_parity(bp->k, bp->n, data, parity, bp->sz) != 0)
            return -1;
        if (bp->n > maxn)
            maxn = bp->n;
    }

    // Emit interleaved across blocks.
    int count = 0;
    for (int s = 0; s < maxn; s++) {
        for (int b = 0; b < nb; b++) {
            struct blk_plan *bp = &plan[b];
            if (s >= bp->n)
                continue;
            struct ota_hdr h = {
                .link_id = c->link_id,
                .seq = tx->seq++,
                .hop = 0,
                .flags = ota_flags(OTA_TYPE_VIDEO, (enum ota_mode)c->mode, bp->flags),
                .frame = tx->frame,
                .block = (uint8_t)b,
                .shard = (uint8_t)s,
                .k = bp->k,
                .n = bp->n,
            };
            uint8_t *pkt = pool_pkt(tx, b, s);
            ota_hdr_pack(&h, pkt);
            emit(ctx, pkt, OTA_HDR_LEN + bp->sz);
            tx->bytes_out += OTA_HDR_LEN + bp->sz;
            count++;
        }
    }
    tx->frame++;
    tx->frames++;
    tx->blocks += (uint32_t)nb;
    tx->packets += (uint32_t)count;
    tx->bytes_in += (uint32_t)len;
    return count;
}

// --------------------------------------------------------------- reassembler

void vpkt_rx_init(struct vpkt_rx *rx, const struct vpkt_rx_cfg *cfg, vpkt_frame_cb cb, void *ctx) {
    memset(rx, 0, sizeof(*rx));
    rx->cfg = *cfg;
    if (rx->cfg.max_wait_frames == 0)
        rx->cfg.max_wait_frames = 2;
    if (rx->cfg.max_wait_frames >= VPKT_RX_FRAMES)
        rx->cfg.max_wait_frames = VPKT_RX_FRAMES - 1;
    rx->cb = cb;
    rx->ctx = ctx;
}

static struct vpkt_frm *frm_get(struct vpkt_rx *rx, uint16_t f) {
    struct vpkt_frm *e = &rx->frames[f % VPKT_RX_FRAMES];
    return (e->used && e->frame == f) ? e : NULL;
}

static struct vpkt_blk *blk_find(struct vpkt_rx *rx, uint16_t f, uint8_t b) {
    for (int i = 0; i < VPKT_RX_SLOTS; i++)
        if (rx->slots[i].used && rx->slots[i].frame == f && rx->slots[i].block == b)
            return &rx->slots[i];
    return NULL;
}

static struct vpkt_blk *blk_alloc(struct vpkt_rx *rx) {
    for (int i = 0; i < VPKT_RX_SLOTS; i++)
        if (!rx->slots[i].used)
            return &rx->slots[i];
    return NULL;
}

static void blk_read(const struct vpkt_blk *s, size_t off, uint8_t *dst, size_t len) {
    while (len) {
        size_t i = off / s->sz, o = off % s->sz;
        size_t n = s->sz - o;
        if (n > len)
            n = len;
        memcpy(dst, s->data[i] + o, n);
        dst += n;
        off += n;
        len -= n;
    }
}

// Offset of the first / last Annex-B start code (including one leading zero
// of a 4-byte code) in buf[from, to), or SIZE_MAX if none.
static size_t first_start(const uint8_t *buf, size_t from, size_t to) {
    for (size_t i = from; i + 2 < to; i++)
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            return (i > from && buf[i - 1] == 0) ? i - 1 : i;
    return SIZE_MAX;
}

static size_t last_start(const uint8_t *buf, size_t from, size_t to) {
    size_t found = SIZE_MAX;
    for (size_t i = from; i + 2 < to; i++)
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            found = (i > from && buf[i - 1] == 0) ? i - 1 : i;
    return found;
}

static void emit_next(struct vpkt_rx *rx) {
    uint16_t f = rx->next_frame;
    struct vpkt_frm *e = frm_get(rx, f);
    struct vpkt_frame_info info = {.frame = f};
    rx->next_frame++;
    if (!e) {
        info.lost = true;
        rx->st.frames_lost++;
        rx->cb(rx->ctx, NULL, 0, &info);
        return;
    }

    int nblocks = e->eof_block >= 0 ? e->eof_block + 1 : e->max_block + 1;
    size_t n = 0, open_start = 0;
    bool open = false;
    for (int b = 0; b < nblocks; b++) {
        struct vpkt_blk *s = blk_find(rx, f, (uint8_t)b);
        if (!s || !s->decoded) {
            rx->st.blocks_lost++;
            if (open)
                n = open_start;
            open = false;
            continue;
        }
        uint8_t hb[VPKT_BLK_HDR];
        blk_read(s, 0, hb, VPKT_BLK_HDR);
        uint16_t hdr = olv_get_u16(hb);
        size_t clen = hdr & VPKT_LEN_MASK;
        if (clen + VPKT_BLK_HDR > (size_t)s->k * s->sz || n + clen > VPKT_MAX_FRAME) {
            rx->st.blocks_lost++;
            if (open)
                n = open_start;
            open = false;
            continue;
        }
        rx->st.blocks_ok++;
        info.blocks_ok++;
        if (s->flags & OTA_FLAG_IDR)
            info.has_idr = true;

        size_t start = n;
        if (open && !(hdr & VPKT_CONT)) {
            // Previous NAL never finished: drop it, this chunk starts fresh.
            start = open_start;
            open = false;
        }
        blk_read(s, VPKT_BLK_HDR, rx->out + start, clen);
        size_t end = start + clen;
        if ((hdr & VPKT_CONT) && !open) {
            // Tail of a NAL whose head was lost: skip to the next NAL.
            size_t fs = first_start(rx->out, start, end);
            if (fs == SIZE_MAX) {
                end = start;
            } else {
                memmove(rx->out + start, rx->out + fs, end - fs);
                end = start + (end - fs);
            }
        }
        n = end;
        if (hdr & VPKT_MORE) {
            size_t ls = last_start(rx->out, start, end);
            if (ls != SIZE_MAX) {
                open_start = ls;
                open = true;
            } else if (!open) {
                n = start; // a fragment with no start of its own and nothing to continue
            }
        } else {
            open = false;
        }
    }
    if (open)
        n = open_start;

    info.blocks = (uint8_t)nblocks;
    info.complete = info.blocks_ok == nblocks && e->eof_block >= 0;
    if (info.complete)
        rx->st.frames_complete++;
    else
        rx->st.frames_partial++;

    for (int i = 0; i < VPKT_RX_SLOTS; i++)
        if (rx->slots[i].used && rx->slots[i].frame == f)
            rx->slots[i].used = false;
    e->used = false;
    rx->cb(rx->ctx, rx->out, n, &info);
}

static bool frame_complete(const struct vpkt_frm *e) {
    return e && e->eof_block >= 0 && e->decoded == e->eof_block + 1;
}

static void try_emit(struct vpkt_rx *rx) {
    for (;;) {
        if (frame_complete(frm_get(rx, rx->next_frame)))
            emit_next(rx);
        else if ((int16_t)(rx->newest_frame - rx->next_frame) >= (int16_t)rx->cfg.max_wait_frames)
            emit_next(rx);
        else
            break;
    }
}

enum vpkt_push vpkt_rx_push(struct vpkt_rx *rx, const uint8_t *pkt, size_t len, uint32_t now_us) {
    struct ota_hdr h;
    rx->st.pkts++;
    if (ota_hdr_unpack(pkt, len, rx->cfg.link_id, &h) != OTA_OK) {
        rx->st.bad++;
        return VPKT_BAD;
    }
    if (ota_hdr_type(&h) != OTA_TYPE_VIDEO)
        return VPKT_IGNORED;
    size_t sz = len - OTA_HDR_LEN;
    if (sz == 0 || sz > OTA_MAX_PAYLOAD || h.n > FEC_MAX_N || (size_t)h.k * sz < VPKT_BLK_HDR) {
        rx->st.bad++;
        return VPKT_BAD;
    }

    uint16_t f = h.frame;
    if (!rx->started) {
        rx->started = true;
        rx->next_frame = f;
        rx->newest_frame = f;
    }
    int16_t d = (int16_t)(f - rx->next_frame);
    if (d < 0) {
        rx->st.late++;
        return VPKT_LATE;
    }
    if (d >= 4 * VPKT_RX_FRAMES) {
        // Long outage or air unit restart: flush what is pending, jump ahead.
        vpkt_rx_flush(rx);
        rx->st.frames_lost += (uint16_t)(f - rx->next_frame);
        rx->next_frame = f;
        rx->newest_frame = f;
        d = 0;
    }
    while (d >= VPKT_RX_FRAMES) {
        emit_next(rx);
        d--;
    }
    if ((int16_t)(f - rx->newest_frame) > 0)
        rx->newest_frame = f;

    struct vpkt_frm *e = frm_get(rx, f);
    if (!e) {
        e = &rx->frames[f % VPKT_RX_FRAMES];
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->frame = f;
        e->eof_block = -1;
        e->first_us = now_us;
    }

    struct vpkt_blk *s = blk_find(rx, f, h.block);
    if (!s) {
        while (!(s = blk_alloc(rx)) && rx->next_frame != f)
            emit_next(rx);
        if (!s || !frm_get(rx, f)) {
            rx->st.bad++;
            return VPKT_BAD;
        }
        s->used = true;
        s->decoded = false;
        s->frame = f;
        s->block = h.block;
        s->k = h.k;
        s->n = h.n;
        s->sz = (uint16_t)sz;
        s->flags = h.flags;
        s->mask = 0;
        s->count = 0;
        if (h.block > e->max_block)
            e->max_block = h.block;
    } else if (s->k != h.k || s->n != h.n || s->sz != sz) {
        rx->st.bad++;
        return VPKT_BAD;
    }
    if (h.flags & OTA_FLAG_EOF)
        e->eof_block = h.block;

    enum vpkt_push ret = VPKT_DUP;
    if (!s->decoded && !(s->mask & (1u << h.shard))) {
        memcpy(s->data[h.shard], pkt + OTA_HDR_LEN, sz);
        s->mask |= 1u << h.shard;
        s->count++;
        rx->st.new_shards++;
        ret = VPKT_NEW;
        if (s->count >= s->k) {
            const uint8_t *in[FEC_MAX_N];
            uint8_t *out[FEC_MAX_N];
            for (int i = 0; i < s->n; i++)
                in[i] = (s->mask & (1u << i)) ? s->data[i] : NULL;
            for (int i = 0; i < s->k; i++)
                out[i] = s->data[i];
            if (fec_recover(s->k, s->n, in, out, s->sz) == 0) {
                s->decoded = true;
                e->decoded++;
            }
        }
    } else {
        rx->st.dups++;
    }
    try_emit(rx);
    return ret;
}

void vpkt_rx_tick(struct vpkt_rx *rx, uint32_t now_us) {
    if (!rx->started || rx->cfg.timeout_us == 0)
        return;
    while ((int16_t)(rx->newest_frame - rx->next_frame) >= 0) {
        // Age of the oldest frame that has packets decides.
        struct vpkt_frm *oldest = NULL;
        for (uint16_t f = rx->next_frame; (int16_t)(rx->newest_frame - f) >= 0; f++)
            if ((oldest = frm_get(rx, f)))
                break;
        if (!oldest || (int32_t)(now_us - oldest->first_us) < (int32_t)rx->cfg.timeout_us)
            break;
        emit_next(rx);
    }
}

void vpkt_rx_flush(struct vpkt_rx *rx) {
    if (!rx->started)
        return;
    while ((int16_t)(rx->newest_frame - rx->next_frame) >= 0)
        emit_next(rx);
}
