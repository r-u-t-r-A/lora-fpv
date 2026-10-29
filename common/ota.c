// SPDX-License-Identifier: GPL-3.0-or-later
#include "ota.h"
#include "olv_util.h"

uint16_t ota_link_id(const char *bind_phrase) {
    return (uint16_t)(olv_fnv1a32(bind_phrase) & 0xFFFFu);
}

void ota_hdr_pack(const struct ota_hdr *h, uint8_t out[OTA_HDR_LEN]) {
    olv_put_u16(out + 0, h->link_id);
    olv_put_u16(out + 2, h->seq);
    out[4] = h->hop;
    out[5] = h->flags;
    olv_put_u16(out + 6, h->frame);
    out[8] = h->block;
    out[9] = h->shard;
    out[10] = h->k;
    out[11] = h->n;
}

int ota_hdr_unpack(const uint8_t *buf, size_t len, uint16_t expect_link_id, struct ota_hdr *h) {
    if (len < OTA_HDR_LEN)
        return OTA_ERR_SHORT;
    h->link_id = olv_get_u16(buf + 0);
    h->seq = olv_get_u16(buf + 2);
    h->hop = buf[4];
    h->flags = buf[5];
    h->frame = olv_get_u16(buf + 6);
    h->block = buf[8];
    h->shard = buf[9];
    h->k = buf[10];
    h->n = buf[11];
    if (h->link_id != expect_link_id)
        return OTA_ERR_LINK;
    switch (ota_hdr_type(h)) {
    case OTA_TYPE_VIDEO:
        if (h->k == 0 || h->k > h->n || h->shard >= h->n)
            return OTA_ERR_INVALID;
        break;
    case OTA_TYPE_TELEM:
    case OTA_TYPE_BEACON:
        break;
    default:
        return OTA_ERR_INVALID;
    }
    return OTA_OK;
}

void ota_beacon_pack(const struct ota_beacon *b, uint8_t out[OTA_BEACON_LEN]) {
    olv_put_u32(out + 0, b->cycle);
    olv_put_u16(out + 4, b->hop_us);
    out[6] = b->n_channels;
    out[7] = b->fec_k;
    out[8] = b->fec_n;
    out[9] = (uint8_t)b->tx_power;
    olv_put_u16(out + 10, b->offset_us);
}

int ota_beacon_unpack(const uint8_t *buf, size_t len, struct ota_beacon *b) {
    if (len < OTA_BEACON_LEN)
        return OTA_ERR_SHORT;
    b->cycle = olv_get_u32(buf + 0);
    b->hop_us = olv_get_u16(buf + 4);
    b->n_channels = buf[6];
    b->fec_k = buf[7];
    b->fec_n = buf[8];
    b->tx_power = (int8_t)buf[9];
    b->offset_us = olv_get_u16(buf + 10);
    if (b->n_channels == 0 || b->hop_us == 0)
        return OTA_ERR_INVALID;
    return OTA_OK;
}
