// SPDX-License-Identifier: GPL-3.0-or-later
// VRX -> browser stream. Each WebSocket binary message carries one or more
// records:   type u8 | len u16 | payload[len]   (little-endian, no CRC: TCP).
// See docs/protocol.md; web/src/proto.ts mirrors this file.
#ifndef OLV_VRX_PROTO_H
#define OLV_VRX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VRX_REC_HDR 3
#define VRX_MSG_MAX 4096 // largest WebSocket message a VRX sends

enum vrx_rec_type {
    VRX_REC_RX_PACKET = 2, // struct vrx_rx_packet
    VRX_REC_STATUS = 4,    // struct vrx_status
    VRX_REC_LOG = 5,       // UTF-8 text
};

// ---- RX_PACKET: OTA bytes, then an 8-byte radio trailer. ----
#define VRX_RX_TRAILER 8

struct vrx_rx_packet {
    uint8_t ota[OTA_MAX_PACKET];
    uint16_t ota_len;
    int16_t rssi;  // dBm
    int8_t snr;    // dB * 4
    uint8_t hop;   // hop index the radio was on
    uint32_t t_us; // VRX monotonic clock at RX done, microseconds, wraps
};

size_t vrx_rx_packet_pack(const struct vrx_rx_packet *r, uint8_t *out, size_t out_cap);
int vrx_rx_packet_unpack(const uint8_t *buf, size_t len, struct vrx_rx_packet *r);

// ---- STATUS: sent by the VRX a few times per second. ----
#define VRX_STATUS_LEN 28

struct vrx_status {
    uint8_t vrx_id;     // the <id> in 10.55.<id>.1
    uint8_t band;       // 0 = 868 MHz, 1 = 2.4 GHz
    uint8_t sync_state; // enum fhss_rx_state
    uint8_t hop;
    uint32_t cycle;
    uint32_t pkts_ok;   // good CRC, right link id
    uint32_t pkts_crc;  // CRC failures (erasures)
    uint32_t pkts_drop; // dropped because the WebSocket could not keep up
    int16_t noise_dbm;  // last noise floor reading, INT16_MIN if unknown
    uint16_t locks;     // times the RX acquired lock
    uint32_t uptime_ms;
};

void vrx_status_pack(const struct vrx_status *s, uint8_t out[VRX_STATUS_LEN]);
int vrx_status_unpack(const uint8_t *buf, size_t len, struct vrx_status *s);

// Appends one record to a message buffer. Returns the new message length, or 0
// if it does not fit.
size_t vrx_rec_append(uint8_t *msg, size_t msg_len, size_t msg_cap, uint8_t type,
                      const uint8_t *payload, size_t len);

// Iterates records in a message. Returns false at the end or on a truncated
// record.
bool vrx_rec_next(const uint8_t *msg, size_t msg_len, size_t *pos, uint8_t *type,
                  const uint8_t **payload, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
