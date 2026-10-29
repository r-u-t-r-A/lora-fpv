// SPDX-License-Identifier: GPL-3.0-or-later
// Radio interface used by air/ and vrx/. Implementations:
//   radio/radio_lr2021.cpp  RadioLib LR2021 over Linux spidev + libgpiod (RV1103)
//   sim/radio_sim.c         in-process simulated channel for host tests
// All calls are non-blocking except radio_open. Times are CLOCK_MONOTONIC
// microseconds, truncated to 32 bits.
#ifndef OLV_RADIO_H
#define OLV_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct radio;

struct radio_flrc {
    uint32_t bitrate_bps; // FLRC gross bit rate: 2600000, 1300000, 520000, 260000
    uint8_t cr;           // inner coding rate: 2 = 1/2, 3 = 3/4, 4 = 1 (uncoded)
    uint16_t preamble_bits;
    uint32_t sync_word;
    int8_t power_dbm; // at the LR2021 output; the module PA adds its gain
};

struct radio_pins {
    const char *spidev; // e.g. "/dev/spidev0.0"
    uint32_t spi_hz;
    const char *gpiochip; // e.g. "/dev/gpiochip1"
    int busy, irq, nreset;
    int tx_en, rx_en; // NiceRF PA/LNA control lines, -1 if not wired
};

struct radio_rx_info {
    int16_t rssi_dbm;
    int8_t snr_q4; // dB * 4
    bool crc_ok;
    uint32_t t_us; // RX done
};

struct radio *radio_open(const struct radio_pins *pins, const struct radio_flrc *flrc);
void radio_close(struct radio *r);

int radio_set_freq(struct radio *r, uint32_t hz);

// Starts a transmission; completion is reported by radio_poll.
int radio_tx(struct radio *r, const uint8_t *buf, size_t len);

// Enters continuous receive on the current frequency.
int radio_rx(struct radio *r);

enum radio_event { RADIO_EV_NONE = 0, RADIO_EV_TX_DONE = 1, RADIO_EV_RX_DONE = 2 };

// Polls for one event. On RX_DONE, the packet is copied to buf (up to cap)
// and *len and *info are filled. Packets with bad CRC are reported with
// crc_ok = false and must be treated as erasures. timeout_us = 0 does not wait.
enum radio_event radio_poll(struct radio *r, uint32_t timeout_us, uint8_t *buf, size_t cap,
                            size_t *len, struct radio_rx_info *info);

#ifdef __cplusplus
}
#endif

#endif
