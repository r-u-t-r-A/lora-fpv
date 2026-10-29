// SPDX-License-Identifier: GPL-3.0-or-later
// common/radio.h over UDP multicast on the loopback interface: every process
// that opens a simulated radio shares one "ether". A packet is heard by a
// radio that is in RX on the same frequency, after its FLRC airtime, unless
// the receiver's loss model drops it. Lets the real air/ and vrx/ programs
// run against each other on one PC, in real time.
//
// The chip in radio_pins.chip is emulated as far as the link sees it:
// settings it cannot do are refused (radio_check), its maximum packet length
// applies to TX and RX, and a receiver only hears packets sent with the same
// FLRC bit rate and coding rate.
//
// Timing: all processes share CLOCK_MONOTONIC, so each datagram carries its
// TX start time and the RX-done time is that plus the airtime, however late
// the receiving process gets to read it (like the kernel timestamp of the
// IRQ edge on hardware). A receiver hears a packet only if it was already
// listening on that frequency when the packet started.
//
// Environment:
//   OLV_SIM_ETHER    group:port (default 239.255.77.1:47999)
//   OLV_SIM_CHANNEL  receiver loss model, sim/channel.h syntax (default none)
//   OLV_SIM_RSSI     reported RSSI in dBm (default -80)
#define _GNU_SOURCE // ppoll

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "airtime.h"
#include "channel.h"
#include "fhss.h"
#include "olv_util.h"
#include "ota.h"
#include "radio.h"

#define WIRE_MAGIC 0x4F4C5645u // "OLVE"
#define WIRE_HDR 32
#define PENDING 64

enum smode { S_STBY, S_RX, S_TX };

struct pending {
    uint32_t done_us;
    uint32_t freq;
    uint16_t len;
    uint8_t data[OTA_MAX_PACKET];
};

struct radio {
    int sock;
    struct sockaddr_in group;
    uint32_t id;
    uint32_t freq;
    enum smode mode;
    struct flrc_phy phy;
    const struct radio_caps *caps;
    struct radio_pins pins;
    struct channel_model ch;
    bool lossy;
    int16_t rssi;
    bool tx_pending;
    uint32_t tx_done_us;
    uint32_t listen_us; // since when in RX on the current frequency
    struct pending q[PENDING];
    int nq;
    double t0;
};

uint32_t radio_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

static int32_t tdiff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

// Channel index for the loss model, from either default band.
static uint8_t chan_of(uint32_t hz) {
    const struct fhss_band bands[2] = {FHSS_BAND_LR_868, FHSS_BAND_HQ_2G4};
    for (int i = 0; i < 2; i++)
        if (hz >= bands[i].start_hz && (hz - bands[i].start_hz) / bands[i].step_hz < bands[i].n)
            return (uint8_t)((hz - bands[i].start_hz) / bands[i].step_hz);
    return 0;
}

struct radio *radio_open(const struct radio_pins *pins, const struct radio_flrc *f) {
    const char *err = radio_check(pins, f);
    if (err) {
        fprintf(stderr, "radio_sim: %s\n", err);
        return NULL;
    }
    struct radio *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    char ether[64] = "239.255.77.1:47999";
    const char *env = getenv("OLV_SIM_ETHER");
    if (env)
        snprintf(ether, sizeof(ether), "%s", env);
    char *colon = strchr(ether, ':');
    int port = colon ? atoi(colon + 1) : 47999;
    if (colon)
        *colon = 0;

    r->sock = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(r->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(r->sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    struct ip_mreq m;
    m.imr_multiaddr.s_addr = inet_addr(ether);
    m.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
    struct in_addr lo = {.s_addr = htonl(INADDR_LOOPBACK)};
    unsigned char loop = 1;
    if (r->sock < 0 || bind(r->sock, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        setsockopt(r->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0 ||
        setsockopt(r->sock, IPPROTO_IP, IP_MULTICAST_IF, &lo, sizeof(lo)) != 0 ||
        setsockopt(r->sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1) != 0) {
        perror("radio_sim: ether socket");
        if (r->sock >= 0)
            close(r->sock);
        free(r);
        return NULL;
    }
    r->group = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    r->group.sin_addr.s_addr = inet_addr(ether);

    uint32_t seed = radio_now_us() ^ ((uint32_t)getpid() << 16);
    r->id = olv_xorshift32(&seed) | 1;
    r->freq = f->freq_hz;
    r->phy = flrc_phy_default(f->bitrate_bps, f->cr);
    r->caps = radio_chip_caps(pins->chip);
    r->pins = *pins;
    const char *chs = getenv("OLV_SIM_CHANNEL");
    if (chs && *chs) {
        if (channel_parse(&r->ch, chs)) {
            close(r->sock);
            free(r);
            return NULL;
        }
        r->lossy = true;
    }
    const char *rs = getenv("OLV_SIM_RSSI");
    r->rssi = (int16_t)(rs ? atoi(rs) : -80);
    r->t0 = radio_now_us() / 1e6;
    fprintf(stderr, "radio_sim: %s, ether %s:%d, id %08x%s%s\n", r->caps->name, ether, port, r->id,
            r->lossy ? ", loss " : "", r->lossy ? chs : "");
    return r;
}

void radio_close(struct radio *r) {
    if (!r)
        return;
    close(r->sock);
    free(r);
}

int radio_set_freq(struct radio *r, uint32_t hz) {
    if (!radio_caps_freq_ok(r->caps, hz)) {
        fprintf(stderr, "radio_sim: %s cannot tune to %.3f MHz\n", r->caps->name, hz / 1e6);
        return -1;
    }
    r->freq = hz;
    r->nq = 0; // anything in flight on the old channel is lost
    r->listen_us = radio_now_us();
    return 0;
}

int radio_calibrate(struct radio *r, uint32_t hz) {
    return radio_set_freq(r, hz);
}

int radio_set_power(struct radio *r, int8_t dbm) {
    const char *err = radio_check_power(&r->pins, r->freq, dbm);
    if (err)
        fprintf(stderr, "radio_sim: %s\n", err);
    return err ? -1 : 0;
}

int radio_set_modulation(struct radio *r, uint32_t bitrate_bps, uint8_t cr) {
    if (!radio_caps_bitrate_ok(r->caps, bitrate_bps) || cr > 7 || !(r->caps->cr_mask & (1u << cr)))
        return -1;
    r->phy = flrc_phy_default(bitrate_bps, cr);
    r->nq = 0;
    return 0;
}

int radio_tx(struct radio *r, const uint8_t *buf, size_t len) {
    if (len > r->caps->max_packet || r->tx_pending)
        return -1;
    uint8_t w[WIRE_HDR + OTA_MAX_PACKET];
    uint32_t now = radio_now_us(), air = flrc_airtime_us(&r->phy, len);
    olv_put_u32(w, WIRE_MAGIC);
    olv_put_u32(w + 4, r->id);
    olv_put_u32(w + 8, r->freq);
    olv_put_u32(w + 12, air);
    olv_put_u32(w + 16, (uint32_t)len);
    olv_put_u32(w + 20, r->phy.bitrate_bps);
    olv_put_u32(w + 24, r->phy.cr);
    olv_put_u32(w + 28, now);
    memcpy(w + WIRE_HDR, buf, len);
    if (sendto(r->sock, w, WIRE_HDR + len, 0, (struct sockaddr *)&r->group, sizeof(r->group)) < 0)
        return -1;
    r->mode = S_TX;
    r->tx_pending = true;
    r->tx_done_us = now + air;
    return 0;
}

int radio_rx(struct radio *r) {
    if (r->mode != S_RX)
        r->listen_us = radio_now_us();
    r->mode = S_RX;
    return 0;
}

int radio_standby(struct radio *r) {
    r->mode = S_STBY;
    r->nq = 0;
    return 0;
}

// Reads every datagram waiting on the socket into the pending queue.
static void drain(struct radio *r) {
    uint8_t w[WIRE_HDR + OTA_MAX_PACKET + 16];
    for (;;) {
        ssize_t n = recv(r->sock, w, sizeof(w), MSG_DONTWAIT);
        if (n < WIRE_HDR)
            return;
        if (olv_get_u32(w) != WIRE_MAGIC || olv_get_u32(w + 4) == r->id)
            continue;
        uint32_t freq = olv_get_u32(w + 8), air = olv_get_u32(w + 12);
        uint32_t len = olv_get_u32(w + 16), t_tx = olv_get_u32(w + 28);
        if (len > OTA_MAX_PACKET || (size_t)n < WIRE_HDR + len)
            continue;
        // Different modulation: not even a sync word match. Longer than
        // this chip's FLRC limit: dropped by its length filter.
        if (r->mode != S_RX || freq != r->freq || r->nq == PENDING ||
            tdiff(t_tx, r->listen_us) < 0 || // tuned in after the preamble
            olv_get_u32(w + 20) != r->phy.bitrate_bps || olv_get_u32(w + 24) != r->phy.cr ||
            len > r->caps->max_packet)
            continue;
        struct pending *p = &r->q[r->nq++];
        p->done_us = t_tx + air;
        p->freq = freq;
        p->len = (uint16_t)len;
        memcpy(p->data, w + WIRE_HDR, len);
    }
}

enum radio_event radio_poll(struct radio *r, uint32_t timeout_us, uint8_t *buf, size_t cap,
                            size_t *len, struct radio_rx_info *info) {
    uint32_t deadline = radio_now_us() + timeout_us;
    for (;;) {
        drain(r);
        uint32_t now = radio_now_us();
        if (r->tx_pending && tdiff(now, r->tx_done_us) >= 0) {
            r->tx_pending = false;
            r->mode = S_STBY;
            if (info)
                info->t_us = r->tx_done_us;
            return RADIO_EV_TX_DONE;
        }
        // Oldest packet whose airtime has elapsed.
        int best = -1;
        for (int i = 0; i < r->nq; i++)
            if (tdiff(now, r->q[i].done_us) >= 0 &&
                (best < 0 || tdiff(r->q[i].done_us, r->q[best].done_us) < 0))
                best = i;
        if (best >= 0) {
            struct pending p = r->q[best];
            r->q[best] = r->q[--r->nq];
            // Retuned during the packet: never received.
            if (p.freq != r->freq || r->mode != S_RX)
                continue;
            bool lost = r->lossy && channel_lost(&r->ch, chan_of(p.freq), now / 1e6 - r->t0);
            size_t n = p.len < cap ? p.len : cap;
            memcpy(buf, p.data, n);
            *len = n;
            if (info) {
                info->crc_ok = !lost; // a lost packet shows up as a CRC error
                info->rssi_dbm = r->rssi;
                info->snr_q4 = 40;
                info->t_us = p.done_us;
            }
            return RADIO_EV_RX_DONE;
        }
        if (tdiff(now, deadline) >= 0)
            return RADIO_EV_NONE;
        // Sleep until the next due event, a new datagram, or the deadline.
        uint32_t wake = deadline;
        if (r->tx_pending && tdiff(r->tx_done_us, wake) < 0)
            wake = r->tx_done_us;
        for (int i = 0; i < r->nq; i++)
            if (tdiff(r->q[i].done_us, wake) < 0)
                wake = r->q[i].done_us;
        int32_t us = tdiff(wake, now);
        struct pollfd pf = {r->sock, POLLIN, 0};
        struct timespec ts = {us / 1000000, (long)(us % 1000000) * 1000};
        if (ppoll(&pf, 1, &ts, NULL) < 0 && errno != EINTR)
            return RADIO_EV_ERROR;
    }
}
