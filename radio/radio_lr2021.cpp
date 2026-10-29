// SPDX-License-Identifier: GPL-3.0-or-later
// common/radio.h on top of RadioLib's LR2021 driver and LinuxHal.
//
// Things to verify on hardware in M0 (see docs/m0-bench.md):
// - that continuous RX stays in RX after each RX_DONE (no restart needed);
// - retune time of setFrequency(..., skipCalibration = true);
// - packet RSSI/SNR scaling.
#include "radio.h"
#include "linux_hal.h"

#include <stdio.h>
#include <string.h>

struct radio {
    LinuxHal hal;
    Module mod;
    LR2021 lr;
    int8_t power_dbm;
    uint32_t pins[Module::RFSWITCH_MAX_PINS];
    Module::RfSwitchMode_t table[RADIO_RF_MODES + 1];

    radio(const radio_pins *p)
        : hal(p->spidev, p->spi_hz),
          // Chip select is driven by spidev, so RadioLib gets no CS pin.
          mod(&hal, RADIOLIB_NC, (uint32_t)p->irq, (uint32_t)p->nreset, (uint32_t)p->busy),
          lr(&mod), power_dbm(0) {}
};

static uint32_t pin_or_nc(int p) {
    return p < 0 ? RADIOLIB_NC : (uint32_t)p;
}

static uint8_t flrc_cr(uint8_t cr) {
    switch (cr) {
    case 2:
        return RADIOLIB_FLRC_CR_1_2;
    case 3:
        return RADIOLIB_FLRC_CR_3_4;
    case 4:
        return RADIOLIB_FLRC_CR_1_0;
    case 5:
        return RADIOLIB_FLRC_CR_2_3;
    default:
        return 0xFF;
    }
}

static int check(const char *what, int16_t st) {
    if (st != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "radio: %s failed: RadioLib error %d\n", what, st);
        return -1;
    }
    return 0;
}

extern "C" uint32_t radio_now_us(void) {
    return (uint32_t)(LinuxHal::monotonicNs() / 1000u);
}

extern "C" struct radio *radio_open(const struct radio_pins *pins, const struct radio_flrc *f) {
    if (flrc_cr(f->cr) == 0xFF) {
        fprintf(stderr, "radio: invalid coding rate %u\n", f->cr);
        return nullptr;
    }
    radio *r = new radio(pins);

    // RF switch: pins and their levels per mode, if the module has any.
    bool any_rf = false;
    for (size_t i = 0; i < Module::RFSWITCH_MAX_PINS; i++)
        r->pins[i] = i < RADIO_RF_PINS ? pin_or_nc(pins->rf_pin[i]) : RADIOLIB_NC;
    for (int i = 0; i < RADIO_RF_PINS; i++)
        any_rf |= pins->rf_pin[i] >= 0;
    if (any_rf) {
        static const uint8_t modes[RADIO_RF_MODES] = {LR2021::MODE_STBY, LR2021::MODE_RX,
                                                      LR2021::MODE_TX, LR2021::MODE_RX_HF,
                                                      LR2021::MODE_TX_HF};
        for (int m = 0; m < RADIO_RF_MODES; m++) {
            r->table[m].mode = modes[m];
            for (size_t i = 0; i < Module::RFSWITCH_MAX_PINS; i++)
                r->table[m].values[i] = (pins->rf_table[m] >> i) & 1u;
        }
        r->table[RADIO_RF_MODES] = END_OF_MODE_TABLE;
        r->lr.setRfSwitchTable(r->pins, r->table);
    }

    r->lr.tcxoVoltage = f->tcxo_mv / 1000.0f;
    ConfigFLRC_t cfg;
    cfg.frequency = (float)(f->freq_hz / 1e6);
    cfg.bitRate = (float)(f->bitrate_bps / 1000.0);
    cfg.codingRate = flrc_cr(f->cr);
    cfg.power = f->power_dbm;
    cfg.preambleLength = f->preamble_bits;
    cfg.dataShaping = RADIOLIB_SHAPING_0_5;
    r->power_dbm = f->power_dbm;
    r->hal.init(); // opens spidev; RadioLib calls it again, which is harmless
    if (!r->hal.ok()) {
        fprintf(stderr, "radio: cannot open %s\n", pins->spidev);
        delete r;
        return nullptr;
    }
    if (check("beginFLRC", r->lr.beginFLRC(cfg))) {
        delete r;
        return nullptr;
    }
    uint8_t sync[4] = {(uint8_t)(f->sync_word >> 24), (uint8_t)(f->sync_word >> 16),
                       (uint8_t)(f->sync_word >> 8), (uint8_t)f->sync_word};
    if (check("setSyncWord", r->lr.setSyncWord(sync, 4)) ||
        check("setCRC", r->lr.setCRC(f->crc_bytes ? f->crc_bytes : 2)) ||
        !r->hal.armIrq((uint32_t)pins->irq)) {
        delete r;
        return nullptr;
    }
    return r;
}

extern "C" void radio_close(struct radio *r) {
    if (!r)
        return;
    r->lr.standby();
    delete r;
}

extern "C" int radio_set_freq(struct radio *r, uint32_t hz) {
    return check("setFrequency", r->lr.setFrequency((float)(hz / 1e6), true));
}

extern "C" int radio_calibrate(struct radio *r, uint32_t hz) {
    // A band change may switch between the sub-GHz and HF power amplifiers.
    if (check("setFrequency", r->lr.setFrequency((float)(hz / 1e6), false)))
        return -1;
    return check("setOutputPower", r->lr.setOutputPower(r->power_dbm));
}

extern "C" int radio_set_power(struct radio *r, int8_t dbm) {
    r->power_dbm = dbm;
    return check("setOutputPower", r->lr.setOutputPower(dbm));
}

extern "C" int radio_tx(struct radio *r, const uint8_t *buf, size_t len) {
    return check("startTransmit", r->lr.startTransmit(buf, len));
}

extern "C" int radio_rx(struct radio *r) {
    return check("startReceive", r->lr.startReceive());
}

extern "C" int radio_standby(struct radio *r) {
    return check("standby", r->lr.standby());
}

extern "C" enum radio_event radio_poll(struct radio *r, uint32_t timeout_us, uint8_t *buf,
                                       size_t cap, size_t *len, struct radio_rx_info *info) {
    uint64_t t_ns = 0;
    int w = r->hal.waitIrq(timeout_us, &t_ns);
    if (w <= 0)
        return w < 0 ? RADIO_EV_ERROR : RADIO_EV_NONE;
    uint32_t irq = r->lr.getIrqFlags();
    if (info)
        info->t_us = (uint32_t)(t_ns / 1000u);
    if (irq & RADIOLIB_LR2021_IRQ_TX_DONE) {
        r->lr.clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL);
        return RADIO_EV_TX_DONE;
    }
    if (irq & RADIOLIB_LR2021_IRQ_RX_DONE) {
        size_t n = r->lr.getPacketLength();
        if (n > cap)
            n = cap;
        int16_t st = r->lr.readData(buf, n); // also clears the IRQ flags
        *len = n;
        if (info) {
            info->crc_ok = st == RADIOLIB_ERR_NONE;
            info->rssi_dbm = (int16_t)r->lr.getRSSI(true);
            info->snr_q4 = (int8_t)(r->lr.getSNR() * 4.0f);
        }
        if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CRC_MISMATCH)
            return RADIO_EV_ERROR;
        return RADIO_EV_RX_DONE;
    }
    r->lr.clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL); // preamble/sync/other: ignore
    return RADIO_EV_NONE;
}
