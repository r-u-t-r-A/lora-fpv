// SPDX-License-Identifier: GPL-3.0-or-later
// Over-the-air packet format: one LR2021 FLRC packet = 12-byte header + payload.
// All multi-byte fields are little-endian. See docs/protocol.md.
#ifndef OLV_OTA_H
#define OLV_OTA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Design value until M0 confirms the LR2021 FLRC maximum payload.
#define OTA_MAX_PACKET 255
#define OTA_HDR_LEN 12
#define OTA_MAX_PAYLOAD (OTA_MAX_PACKET - OTA_HDR_LEN)

enum ota_type {
    OTA_TYPE_VIDEO = 0,
    OTA_TYPE_TELEM = 1,
    OTA_TYPE_BEACON = 2,
};

// flags bits 4-5: link mode. Lets a receiver tell which preset it locked onto.
enum ota_mode {
    OTA_MODE_LR_520 = 0,  // 868 MHz, FLRC 520 kbit/s
    OTA_MODE_LR_260 = 1,  // 868 MHz, FLRC 260 kbit/s robust preset
    OTA_MODE_HQ_2600 = 2, // 2.4 GHz, FLRC 2.6 Mbit/s
    OTA_MODE_HQ_1300 = 3, // 2.4 GHz, FLRC 1.3 Mbit/s
};

#define OTA_FLAG_TYPE_MASK 0x03u
#define OTA_FLAG_IDR 0x04u // block carries an IDR and/or VPS/SPS/PPS
#define OTA_FLAG_EOF 0x08u // block is the last block of its frame
#define OTA_FLAG_MODE_SHIFT 4
#define OTA_FLAG_MODE_MASK 0x30u

struct ota_hdr {
    uint16_t link_id; // FNV-1a of bind phrase, low 16 bits
    uint16_t seq;     // packet counter, wraps
    uint8_t hop;      // hop index within the current cycle
    uint8_t flags;    // see OTA_FLAG_*
    uint16_t frame;   // video frame number
    uint8_t block;    // FEC block number within frame
    uint8_t shard;    // shard index 0..n-1 (shards >= k are parity)
    uint8_t k;        // data shards in this block
    uint8_t n;        // total shards in this block
};

static inline enum ota_type ota_hdr_type(const struct ota_hdr *h) {
    return (enum ota_type)(h->flags & OTA_FLAG_TYPE_MASK);
}

static inline enum ota_mode ota_hdr_mode(const struct ota_hdr *h) {
    return (enum ota_mode)((h->flags & OTA_FLAG_MODE_MASK) >> OTA_FLAG_MODE_SHIFT);
}

static inline uint8_t ota_flags(enum ota_type t, enum ota_mode m, uint8_t extra) {
    return (uint8_t)((t & OTA_FLAG_TYPE_MASK) | ((m << OTA_FLAG_MODE_SHIFT) & OTA_FLAG_MODE_MASK) |
                     (extra & (OTA_FLAG_IDR | OTA_FLAG_EOF)));
}

uint16_t ota_link_id(const char *bind_phrase);

void ota_hdr_pack(const struct ota_hdr *h, uint8_t out[OTA_HDR_LEN]);

enum ota_err {
    OTA_OK = 0,
    OTA_ERR_SHORT = -1,   // fewer than OTA_HDR_LEN bytes
    OTA_ERR_LINK = -2,    // link_id mismatch
    OTA_ERR_INVALID = -3, // inconsistent fields (video: k==0, k>n, shard>=n; unknown type)
};

// Parses and validates a received packet. expect_link_id is compared with the
// header; pass the receiver's own link id.
int ota_hdr_unpack(const uint8_t *buf, size_t len, uint16_t expect_link_id, struct ota_hdr *h);

// ---- Beacon (type 2) payload: lets a parked receiver learn the hop clock. ----
// Sent as the first packet of every hop (beacon_every = 1) by the air unit.
#define OTA_BEACON_LEN 12

struct ota_beacon {
    uint32_t cycle;     // hop cycle counter (seeds the per-cycle permutation)
    uint16_t hop_us;    // dwell time per hop in microseconds (10000 = 100 Hz)
    uint8_t n_channels; // channels in the hop list
    uint8_t fec_k;      // default data shards (informational)
    uint8_t fec_n;      // default total shards (informational)
    int8_t tx_power;    // dBm, informational
    uint16_t offset_us; // time from hop start to start of this beacon's transmission
};

void ota_beacon_pack(const struct ota_beacon *b, uint8_t out[OTA_BEACON_LEN]);
int ota_beacon_unpack(const uint8_t *buf, size_t len, struct ota_beacon *b);

#ifdef __cplusplus
}
#endif

#endif
