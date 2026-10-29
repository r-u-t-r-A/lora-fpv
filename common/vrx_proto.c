// SPDX-License-Identifier: GPL-3.0-or-later
#include "vrx_proto.h"
#include "olv_util.h"

#include <string.h>

size_t vrx_rx_packet_pack(const struct vrx_rx_packet *r, uint8_t *out, size_t out_cap) {
    size_t total = (size_t)r->ota_len + VRX_RX_TRAILER;
    if (r->ota_len > OTA_MAX_PACKET || total > out_cap)
        return 0;
    memcpy(out, r->ota, r->ota_len);
    uint8_t *t = out + r->ota_len;
    olv_put_u16(t + 0, (uint16_t)r->rssi);
    t[2] = (uint8_t)r->snr;
    t[3] = r->hop;
    olv_put_u32(t + 4, r->t_us);
    return total;
}

int vrx_rx_packet_unpack(const uint8_t *buf, size_t len, struct vrx_rx_packet *r) {
    if (len < VRX_RX_TRAILER || len - VRX_RX_TRAILER > OTA_MAX_PACKET)
        return -1;
    r->ota_len = (uint16_t)(len - VRX_RX_TRAILER);
    memcpy(r->ota, buf, r->ota_len);
    const uint8_t *t = buf + r->ota_len;
    r->rssi = (int16_t)olv_get_u16(t + 0);
    r->snr = (int8_t)t[2];
    r->hop = t[3];
    r->t_us = olv_get_u32(t + 4);
    return 0;
}

void vrx_status_pack(const struct vrx_status *s, uint8_t out[VRX_STATUS_LEN]) {
    out[0] = s->vrx_id;
    out[1] = s->band;
    out[2] = s->sync_state;
    out[3] = s->hop;
    olv_put_u32(out + 4, s->cycle);
    olv_put_u32(out + 8, s->pkts_ok);
    olv_put_u32(out + 12, s->pkts_crc);
    olv_put_u32(out + 16, s->pkts_drop);
    olv_put_u16(out + 20, (uint16_t)s->noise_dbm);
    olv_put_u16(out + 22, s->locks);
    olv_put_u32(out + 24, s->uptime_ms);
}

int vrx_status_unpack(const uint8_t *buf, size_t len, struct vrx_status *s) {
    if (len != VRX_STATUS_LEN)
        return -1;
    s->vrx_id = buf[0];
    s->band = buf[1];
    s->sync_state = buf[2];
    s->hop = buf[3];
    s->cycle = olv_get_u32(buf + 4);
    s->pkts_ok = olv_get_u32(buf + 8);
    s->pkts_crc = olv_get_u32(buf + 12);
    s->pkts_drop = olv_get_u32(buf + 16);
    s->noise_dbm = (int16_t)olv_get_u16(buf + 20);
    s->locks = olv_get_u16(buf + 22);
    s->uptime_ms = olv_get_u32(buf + 24);
    return 0;
}

size_t vrx_rec_append(uint8_t *msg, size_t msg_len, size_t msg_cap, uint8_t type,
                      const uint8_t *payload, size_t len) {
    if (len > 0xFFFFu || msg_len + VRX_REC_HDR + len > msg_cap)
        return 0;
    msg[msg_len] = type;
    olv_put_u16(msg + msg_len + 1, (uint16_t)len);
    if (len)
        memcpy(msg + msg_len + VRX_REC_HDR, payload, len);
    return msg_len + VRX_REC_HDR + len;
}

bool vrx_rec_next(const uint8_t *msg, size_t msg_len, size_t *pos, uint8_t *type,
                  const uint8_t **payload, size_t *len) {
    if (*pos + VRX_REC_HDR > msg_len)
        return false;
    size_t l = olv_get_u16(msg + *pos + 1);
    if (*pos + VRX_REC_HDR + l > msg_len)
        return false;
    *type = msg[*pos];
    *payload = msg + *pos + VRX_REC_HDR;
    *len = l;
    *pos += VRX_REC_HDR + l;
    return true;
}
