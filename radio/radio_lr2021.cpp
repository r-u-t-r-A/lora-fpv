// SPDX-License-Identifier: GPL-3.0-or-later
// LR2021 backend (RadioLib LR2021 class), sub-GHz and 2.4 GHz.
//
// Things to verify on hardware in M0 (see docs/m0-bench.md):
// - that continuous RX stays in RX after each RX_DONE (no restart needed);
// - retune time of setFrequency(..., skipCalibration = true);
// - packet RSSI/SNR scaling.
#include "radio_impl.h"

namespace {

uint8_t flrc_cr(uint8_t cr) {
    switch (cr) {
    case 2:
        return RADIOLIB_FLRC_CR_1_2;
    case 3:
        return RADIOLIB_FLRC_CR_3_4;
    case 4:
        return RADIOLIB_FLRC_CR_1_0;
    default: // 5, checked by radio_check
        return RADIOLIB_FLRC_CR_2_3;
    }
}

struct Lr2021 : radio {
    LR2021 lr;

    explicit Lr2021(const radio_pins *p) : radio(p), lr(&mod) {}

    int setFreq(uint32_t hz) override {
        return radio_check_st("setFrequency", lr.setFrequency((float)(hz / 1e6), true));
    }

    int calibrate(uint32_t hz) override {
        // A band change may switch between the sub-GHz and HF power amplifiers.
        if (radio_check_st("setFrequency", lr.setFrequency((float)(hz / 1e6), false)))
            return -1;
        return radio_check_st("setOutputPower", lr.setOutputPower(power_dbm));
    }

    int setPower(int8_t dbm) override {
        power_dbm = dbm;
        return radio_check_st("setOutputPower", lr.setOutputPower(dbm));
    }

    int setModulation(uint32_t bitrate_bps, uint8_t cr) override {
        if (radio_check_st("setBitRate", lr.setBitRate((float)(bitrate_bps / 1000.0))))
            return -1;
        return radio_check_st("setCodingRate", lr.setCodingRate(flrc_cr(cr)));
    }

    int tx(const uint8_t *buf, size_t len) override {
        return radio_check_st("startTransmit", lr.startTransmit(buf, len));
    }

    int rx() override { return radio_check_st("startReceive", lr.startReceive()); }

    int standby() override { return radio_check_st("standby", lr.standby()); }

    radio_event onIrq(uint8_t *buf, size_t cap, size_t *len, radio_rx_info *info) override {
        uint32_t irq = lr.getIrqFlags();
        if (irq & RADIOLIB_LR2021_IRQ_TX_DONE) {
            lr.clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL);
            return RADIO_EV_TX_DONE;
        }
        if (irq & RADIOLIB_LR2021_IRQ_RX_DONE) {
            size_t n = lr.getPacketLength();
            if (n > cap)
                n = cap;
            int16_t st = lr.readData(buf, n); // also clears the IRQ flags
            *len = n;
            info->crc_ok = st == RADIOLIB_ERR_NONE;
            info->rssi_dbm = (int16_t)lr.getRSSI(true);
            info->snr_q4 = (int8_t)(lr.getSNR() * 4.0f);
            if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CRC_MISMATCH)
                return RADIO_EV_ERROR;
            return RADIO_EV_RX_DONE;
        }
        lr.clearIrqFlags(RADIOLIB_LR2021_IRQ_ALL); // preamble/sync/other: ignore
        return RADIO_EV_NONE;
    }
};

} // namespace

radio *lr2021_open(const radio_pins *pins, const radio_flrc *f) {
    Lr2021 *r = new Lr2021(pins);
    static const uint8_t modes[RADIO_RF_MODES] = {LR2021::MODE_STBY, LR2021::MODE_RX,
                                                  LR2021::MODE_TX, LR2021::MODE_RX_HF,
                                                  LR2021::MODE_TX_HF};
    if (r->rfSwitchTable(pins, modes, RADIO_RF_MODES))
        r->lr.setRfSwitchTable(r->rf_pins, r->rf_modes);

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
    if (radio_check_st("beginFLRC", r->lr.beginFLRC(cfg))) {
        delete r;
        return nullptr;
    }
    uint8_t sync[4] = {(uint8_t)(f->sync_word >> 24), (uint8_t)(f->sync_word >> 16),
                       (uint8_t)(f->sync_word >> 8), (uint8_t)f->sync_word};
    if (radio_check_st("setSyncWord", r->lr.setSyncWord(sync, 4)) ||
        radio_check_st("setCRC", r->lr.setCRC(f->crc_bytes ? f->crc_bytes : 2))) {
        delete r;
        return nullptr;
    }
    return r;
}
