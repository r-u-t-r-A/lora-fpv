// SPDX-License-Identifier: GPL-3.0-or-later
// Video packetizer (air) and reassembler (ground).
//
// A frame (one H.265 access unit, Annex B) is cut into chunks at NAL
// boundaries where possible. Each chunk becomes one FEC block:
//
//   block payload = blk_hdr u16 | chunk bytes | zero padding
//   blk_hdr bits 0-13: chunk length, bit 14 CONT: chunk starts inside a NAL
//   that began in the previous block, bit 15 MORE: the chunk's last NAL
//   continues in the next block.
//
// The block payload is split into k equal shards, RS parity adds n-k more;
// each shard is one OTA packet. Shards of all blocks of a frame are sent
// interleaved (shard 0 of every block, then shard 1, ...).
//
// The reassembler accepts packets from any number of receivers in any order,
// keeps the first good copy of each shard, recovers blocks once k shards are
// in, and emits frames in order. NALs cut by a lost block are removed so the
// decoder only sees whole NAL units.
#ifndef OLV_VPKT_H
#define OLV_VPKT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fec.h"
#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VPKT_BLK_HDR 2
#define VPKT_LEN_MASK 0x3FFFu
#define VPKT_CONT 0x4000u
#define VPKT_MORE 0x8000u

#define VPKT_MAX_FRAME (128 * 1024) // largest access unit handled
#define VPKT_TX_MAX_BLOCKS 96

struct vpkt_tx_cfg {
    uint16_t link_id;
    uint8_t mode;       // enum ota_mode
    uint8_t k, n;       // FEC shape for a full block (8, 12)
    uint16_t max_shard; // shard payload bytes, <= OTA_MAX_PAYLOAD
    uint16_t min_shard; // below this, small blocks use fewer shards
};

typedef void (*vpkt_emit_cb)(void *ctx, const uint8_t *pkt, size_t len);

struct vpkt_tx {
    struct vpkt_tx_cfg cfg;
    uint16_t seq;
    uint16_t frame;
    uint32_t frames, blocks, packets, bytes_in, bytes_out;
    uint8_t *pool; // VPKT_TX_MAX_BLOCKS * FEC_MAX_N * OTA_MAX_PACKET
};

int vpkt_tx_init(struct vpkt_tx *tx, const struct vpkt_tx_cfg *cfg);
void vpkt_tx_free(struct vpkt_tx *tx);

// Bytes of chunk data one full block carries.
static inline size_t vpkt_block_capacity(const struct vpkt_tx_cfg *c) {
    return (size_t)c->k * c->max_shard - VPKT_BLK_HDR;
}

// Packetises one access unit. hop is left 0: the air scheduler writes it into
// byte 4 at transmit time. Returns the number of packets, or -1 if the AU is
// too large.
int vpkt_tx_frame(struct vpkt_tx *tx, const uint8_t *au, size_t len, vpkt_emit_cb emit, void *ctx);

// ---- Reassembler ----

#define VPKT_RX_SLOTS 128
#define VPKT_RX_FRAMES 16

struct vpkt_rx_cfg {
    uint16_t link_id;
    // An incomplete frame is given up once packets for a frame this many
    // frames newer arrive (covers skew between receivers).
    uint8_t max_wait_frames;
    // ... or once this long has passed since its first packet (0 = off).
    uint32_t timeout_us;
};

struct vpkt_frame_info {
    uint16_t frame;
    bool lost;         // nothing at all arrived for this frame
    bool complete;     // every block recovered
    bool has_idr;      // a recovered block carried an IRAP or parameter sets
    uint8_t blocks;    // blocks known for the frame (from EOF or highest seen)
    uint8_t blocks_ok; // blocks recovered
};

typedef void (*vpkt_frame_cb)(void *ctx, const uint8_t *au, size_t len,
                              const struct vpkt_frame_info *info);

enum vpkt_push {
    VPKT_NEW = 0,    // shard stored (this copy was the first)
    VPKT_DUP = 1,    // shard already had, or its block already recovered
    VPKT_LATE = 2,   // frame already emitted
    VPKT_BAD = 3,    // header/link/shape error
    VPKT_IGNORED = 4 // not a video packet
};

struct vpkt_blk {
    bool used;
    bool decoded;
    uint16_t frame;
    uint8_t block, k, n, flags, count;
    uint16_t sz;
    uint32_t mask;
    uint8_t data[FEC_MAX_N][OTA_MAX_PAYLOAD];
};

struct vpkt_frm {
    bool used;
    uint16_t frame;
    int16_t eof_block; // -1 until a packet with EOF arrives
    uint8_t max_block;
    uint8_t decoded;
    uint32_t first_us;
};

struct vpkt_rx_stats {
    uint32_t pkts, new_shards, dups, late, bad;
    uint32_t blocks_ok, blocks_lost;
    uint32_t frames_complete, frames_partial, frames_lost;
    uint32_t restarts; // frame counter jumped far backwards
};

struct vpkt_rx {
    struct vpkt_rx_cfg cfg;
    vpkt_frame_cb cb;
    void *ctx;
    bool started;
    uint16_t next_frame;
    uint16_t newest_frame;
    struct vpkt_frm frames[VPKT_RX_FRAMES];
    struct vpkt_blk slots[VPKT_RX_SLOTS];
    struct vpkt_rx_stats st;
    uint8_t out[VPKT_MAX_FRAME];
};

void vpkt_rx_init(struct vpkt_rx *rx, const struct vpkt_rx_cfg *cfg, vpkt_frame_cb cb, void *ctx);

// Feeds one received OTA packet (good CRC). now_us is used for timeouts.
enum vpkt_push vpkt_rx_push(struct vpkt_rx *rx, const uint8_t *pkt, size_t len, uint32_t now_us);

// Applies the timeout rule; call periodically when timeout_us is set.
void vpkt_rx_tick(struct vpkt_rx *rx, uint32_t now_us);

// Emits every pending frame (end of stream).
void vpkt_rx_flush(struct vpkt_rx *rx);

#ifdef __cplusplus
}
#endif

#endif
