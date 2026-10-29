// SPDX-License-Identifier: GPL-3.0-or-later
// Chip backends behind common/radio.h. radio.cpp checks the settings against
// the chip's capabilities, opens the backend picked by radio_pins.chip and
// forwards the C calls to it.
#ifndef OLV_RADIO_IMPL_H
#define OLV_RADIO_IMPL_H

#include "linux_hal.h"
#include "radio.h"

#include <stdio.h>

struct radio {
    LinuxHal hal;
    Module mod;
    int8_t power_dbm;
    int8_t lna_gain_db;
    uint32_t rf_pins[Module::RFSWITCH_MAX_PINS];
    Module::RfSwitchMode_t rf_modes[RADIO_RF_MODES + 1];

    explicit radio(const radio_pins *p)
        : hal(p->spidev, p->spi_hz),
          // Chip select is driven by spidev, so RadioLib gets no CS pin.
          mod(&hal, RADIOLIB_NC, (uint32_t)p->irq, (uint32_t)p->nreset, (uint32_t)p->busy),
          power_dbm(0), lna_gain_db(p->lna_gain_db) {}
    virtual ~radio() {}

    virtual int setFreq(uint32_t hz) = 0;
    virtual int calibrate(uint32_t hz) = 0;
    virtual int setPower(int8_t dbm) = 0;
    virtual int setModulation(uint32_t bitrate_bps, uint8_t cr) = 0;
    virtual int tx(const uint8_t *buf, size_t len) = 0;
    virtual int rx() = 0;
    virtual int standby() = 0;
    // Called after an IRQ edge at t_us; reads and clears the chip's flags.
    virtual radio_event onIrq(uint8_t *buf, size_t cap, size_t *len, radio_rx_info *info) = 0;

    // Fills rf_pins/rf_modes from the board config for the given RadioLib
    // mode numbers (one per used rf_table entry, in order). Returns false if
    // the module has no RF control lines.
    bool rfSwitchTable(const radio_pins *p, const uint8_t *modes, int n_modes) {
        bool any = false;
        for (size_t i = 0; i < Module::RFSWITCH_MAX_PINS; i++) {
            int pin = i < RADIO_RF_PINS ? p->rf_pin[i] : -1;
            rf_pins[i] = pin < 0 ? RADIOLIB_NC : (uint32_t)pin;
            any |= pin >= 0;
        }
        for (int m = 0; m < n_modes; m++) {
            rf_modes[m].mode = modes[m];
            for (size_t i = 0; i < Module::RFSWITCH_MAX_PINS; i++)
                rf_modes[m].values[i] = (p->rf_table[m] >> i) & 1u;
        }
        rf_modes[n_modes] = END_OF_MODE_TABLE;
        return any;
    }
};

static inline int radio_check_st(const char *what, int16_t st) {
    if (st != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "radio: %s failed: RadioLib error %d\n", what, st);
        return -1;
    }
    return 0;
}

radio *lr2021_open(const radio_pins *pins, const radio_flrc *flrc);
radio *sx1280_open(const radio_pins *pins, const radio_flrc *flrc);

#endif
