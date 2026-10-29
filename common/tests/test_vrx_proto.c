// SPDX-License-Identifier: GPL-3.0-or-later
#include "vrx_proto.h"
#include "t.h"

#include <string.h>

static void test_rx_packet(void) {
    struct vrx_rx_packet p = {
        .ota_len = 100, .rssi = -97, .snr = -20, .hop = 33, .t_us = 0xDEADBEEF};
    for (int i = 0; i < 100; i++)
        p.ota[i] = (uint8_t)(i * 7);
    uint8_t buf[512];
    size_t n = vrx_rx_packet_pack(&p, buf, sizeof(buf));
    CHECK(n == 108);
    CHECK(vrx_rx_packet_pack(&p, buf, 107) == 0);
    struct vrx_rx_packet r;
    CHECK(vrx_rx_packet_unpack(buf, n, &r) == 0);
    CHECK(r.ota_len == 100 && r.rssi == -97 && r.snr == -20 && r.hop == 33 && r.t_us == 0xDEADBEEF);
    CHECK(memcmp(r.ota, p.ota, 100) == 0);
    CHECK(vrx_rx_packet_unpack(buf, 7, &r) != 0);
    CHECK(vrx_rx_packet_unpack(buf, OTA_MAX_PACKET + VRX_RX_TRAILER + 1, &r) != 0);
}

static void test_status(void) {
    struct vrx_status s = {.vrx_id = 3,
                           .band = 1,
                           .sync_state = 2,
                           .hop = 17,
                           .cycle = 123456,
                           .pkts_ok = 1000,
                           .pkts_crc = 12,
                           .pkts_drop = 1,
                           .noise_dbm = -110,
                           .locks = 4,
                           .uptime_ms = 99999};
    uint8_t b[VRX_STATUS_LEN];
    vrx_status_pack(&s, b);
    struct vrx_status r;
    CHECK(vrx_status_unpack(b, sizeof(b), &r) == 0);
    CHECK(memcmp(&r, &s, sizeof(s)) == 0);
    CHECK(vrx_status_unpack(b, sizeof(b) - 1, &r) != 0);
}

static void test_records(void) {
    uint8_t msg[64];
    size_t n = 0;
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {9};
    n = vrx_rec_append(msg, n, sizeof(msg), VRX_REC_RX_PACKET, a, sizeof(a));
    CHECK(n == 6);
    n = vrx_rec_append(msg, n, sizeof(msg), VRX_REC_LOG, b, sizeof(b));
    CHECK(n == 10);
    uint8_t big[64] = {0};
    CHECK(vrx_rec_append(msg, n, sizeof(msg), VRX_REC_LOG, big, sizeof(big)) == 0);

    size_t pos = 0;
    uint8_t type;
    const uint8_t *pl;
    size_t len;
    CHECK(vrx_rec_next(msg, n, &pos, &type, &pl, &len));
    CHECK(type == VRX_REC_RX_PACKET && len == 3 && pl[2] == 3);
    CHECK(vrx_rec_next(msg, n, &pos, &type, &pl, &len));
    CHECK(type == VRX_REC_LOG && len == 1 && pl[0] == 9);
    CHECK(!vrx_rec_next(msg, n, &pos, &type, &pl, &len));
    // Truncated record.
    pos = 0;
    CHECK(!vrx_rec_next(msg, 5, &pos, &type, &pl, &len));
}

int main(void) {
    T_RUN(test_rx_packet);
    T_RUN(test_status);
    T_RUN(test_records);
    T_DONE();
}
