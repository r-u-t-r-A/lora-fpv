// SPDX-License-Identifier: GPL-3.0-or-later
// Radio interface used by air/, vrx/ and tools/. Implementations:
//   radio/radio_lr2021.cpp  RadioLib LR2021 over Linux spidev + GPIO chardev (RV1103)
//   (sim/ drives common/ directly in virtual time and needs no radio)
// All calls except radio_open are non-blocking or bounded by a timeout.
// Times are CLOCK_MONOTONIC microseconds, truncated to 32 bits.
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
    uint32_t freq_hz;     // initial frequency; the front end is calibrated here
    uint32_t bitrate_bps; // 2600000 2080000 1300000 1040000 650000 520000 325000 260000
    uint8_t cr;           // inner coding rate: 2 = 1/2, 3 = 3/4, 4 = 1 (uncoded), 5 = 2/3
    uint16_t preamble_bits;
    uint32_t sync_word;
    uint8_t crc_bytes; // 2 (default), 3 or 4
    int8_t power_dbm;  // at the LR2021 output; the module PA adds its gain
    uint16_t tcxo_mv;  // TCXO supply on DIO3 in mV, 0 for a crystal
};

// GPIO numbers as in the Luckfox pinout: bank * 32 + line, e.g. GPIO1_C7 =
// 1 * 32 + 2 * 8 + 7 = 55. Bank n is /dev/gpiochip<n>. -1 = not connected.
#define RADIO_RF_PINS 3
enum radio_rf_mode {
    RADIO_RF_STBY = 0,
    RADIO_RF_RX = 1,    // sub-GHz receive
    RADIO_RF_TX = 2,    // sub-GHz transmit
    RADIO_RF_RX_HF = 3, // 2.4 GHz receive
    RADIO_RF_TX_HF = 4, // 2.4 GHz transmit
    RADIO_RF_MODES = 5,
};

struct radio_pins {
    const char *spidev; // e.g. "/dev/spidev0.0" (chip select handled by spidev)
    uint32_t spi_hz;
    int busy, irq, nreset; // irq = the module pin wired to LR2021 DIO5
    // PA/LNA/antenna-switch control lines of the module and their levels per
    // mode (bit i = level of rf_pin[i]); take both from the module datasheet.
    int rf_pin[RADIO_RF_PINS];
    uint8_t rf_table[RADIO_RF_MODES];
};

struct radio_rx_info {
    int16_t rssi_dbm;
    int8_t snr_q4; // dB * 4
    bool crc_ok;
    uint32_t t_us; // RX done (kernel timestamp of the IRQ edge)
};

// Returns NULL on failure (message on stderr).
struct radio *radio_open(const struct radio_pins *pins, const struct radio_flrc *flrc);
void radio_close(struct radio *r);

// Retune without recalibration (fast path for hopping).
int radio_set_freq(struct radio *r, uint32_t hz);

// Full calibration of the front end around hz (band change).
int radio_calibrate(struct radio *r, uint32_t hz);

int radio_set_power(struct radio *r, int8_t dbm);

// Starts a transmission; completion is reported by radio_poll.
int radio_tx(struct radio *r, const uint8_t *buf, size_t len);

// Enters continuous receive on the current frequency.
int radio_rx(struct radio *r);

int radio_standby(struct radio *r);

enum radio_event {
    RADIO_EV_NONE = 0,
    RADIO_EV_TX_DONE = 1,
    RADIO_EV_RX_DONE = 2,
    RADIO_EV_ERROR = -1,
};

// Waits up to timeout_us for one event. On RX_DONE the packet is copied to
// buf (up to cap) and *len and *info are filled; packets with a bad CRC come
// back with crc_ok = false and must be treated as erasures. On TX_DONE,
// info->t_us is the TX done time.
enum radio_event radio_poll(struct radio *r, uint32_t timeout_us, uint8_t *buf, size_t cap,
                            size_t *len, struct radio_rx_info *info);

uint32_t radio_now_us(void);

#ifdef __cplusplus
}
#endif

#endif
