// SPDX-License-Identifier: GPL-3.0-or-later
#include "ota.h"
#include "t.h"

#include <string.h>

static void test_link_id(void) {
    // FNV-1a("test") = 0xafd071e5 (reference value).
    CHECK(ota_link_id("test") == 0x71e5);
}

static void test_hdr_layout(void) {
    struct ota_hdr h = {.link_id = 0x1234,
                        .seq = 0xBEEF,
                        .hop = 7,
                        .flags = ota_flags(OTA_TYPE_VIDEO, OTA_MODE_HQ_2600, OTA_FLAG_EOF),
                        .frame = 0x0102,
                        .block = 3,
                        .shard = 9,
                        .k = 8,
                        .n = 12};
    uint8_t b[OTA_HDR_LEN];
    ota_hdr_pack(&h, b);
    const uint8_t want[OTA_HDR_LEN] = {0x34, 0x12, 0xEF, 0xBE, 7, 0x28, 0x02, 0x01, 3, 9, 8, 12};
    CHECK(memcmp(b, want, sizeof(want)) == 0);

    struct ota_hdr r;
    CHECK(ota_hdr_unpack(b, sizeof(b), 0x1234, &r) == OTA_OK);
    CHECK(memcmp(&r, &h, sizeof(h)) == 0);
    CHECK(ota_hdr_type(&r) == OTA_TYPE_VIDEO);
    CHECK(ota_hdr_mode(&r) == OTA_MODE_HQ_2600);
    CHECK(r.flags & OTA_FLAG_EOF);
    CHECK(!(r.flags & OTA_FLAG_IDR));
}

static void test_hdr_reject(void) {
    struct ota_hdr h = {.link_id = 1, .k = 8, .n = 12, .shard = 11};
    uint8_t b[OTA_HDR_LEN];
    struct ota_hdr r;
    ota_hdr_pack(&h, b);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN - 1, 1, &r) == OTA_ERR_SHORT);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 2, &r) == OTA_ERR_LINK);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 1, &r) == OTA_OK);

    h.shard = 12; // >= n
    ota_hdr_pack(&h, b);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 1, &r) == OTA_ERR_INVALID);
    h.shard = 0;
    h.k = 13; // k > n
    ota_hdr_pack(&h, b);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 1, &r) == OTA_ERR_INVALID);
    h.k = 0;
    ota_hdr_pack(&h, b);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 1, &r) == OTA_ERR_INVALID);

    // Beacons do not carry FEC fields.
    h.flags = ota_flags(OTA_TYPE_BEACON, OTA_MODE_LR_520, 0);
    ota_hdr_pack(&h, b);
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 1, &r) == OTA_OK);
    // Type 3 is undefined.
    b[5] = 3;
    CHECK(ota_hdr_unpack(b, OTA_HDR_LEN, 1, &r) == OTA_ERR_INVALID);
}

static void test_beacon(void) {
    struct ota_beacon bc = {.cycle = 0xA1B2C3D4,
                            .hop_us = 10000,
                            .n_channels = 40,
                            .fec_k = 8,
                            .fec_n = 12,
                            .tx_power = -3,
                            .offset_us = 150};
    uint8_t b[OTA_BEACON_LEN];
    ota_beacon_pack(&bc, b);
    CHECK(b[0] == 0xD4 && b[3] == 0xA1);
    struct ota_beacon r;
    CHECK(ota_beacon_unpack(b, sizeof(b), &r) == OTA_OK);
    CHECK(memcmp(&r, &bc, sizeof(bc)) == 0);
    CHECK(ota_beacon_unpack(b, sizeof(b) - 1, &r) == OTA_ERR_SHORT);
    b[6] = 0;
    CHECK(ota_beacon_unpack(b, sizeof(b), &r) == OTA_ERR_INVALID);
}

int main(void) {
    T_RUN(test_link_id);
    T_RUN(test_hdr_layout);
    T_RUN(test_hdr_reject);
    T_RUN(test_beacon);
    T_DONE();
}
