// SPDX-License-Identifier: GPL-3.0-or-later
// SX1280 backend (RadioLib SX1280 class), 2.4 GHz only, built around the
// Ebyte E28-2G4M27S(X): SX1280 + external PA/LNA whose TXEN and RXEN pins the
// host drives (radio_conf "module=e28-2g4m27s", rf_pins=TXEN,RXEN).
//
// Differences from the LR2021 that matter here:
// - FLRC payload is limited to 127 bytes (the air sizes its packets from
//   radio_chip_caps); bit rates top out at 1.3 Mbit/s; no 2/3 coding rate.
// - RadioLib's readData() puts the chip into standby, which would end the
//   continuous RX the hop loop relies on, so packets are read straight from
//   the data buffer and the chip stays in RX.
// - No FLRC SNR; RSSI is the sync-word RSSI from GetPacketStatus.
// - RX -> TX goes through standby so TXEN and RXEN are never high together
//   (RadioLib switches the pins one at a time).
//
// To verify on hardware (docs/m0-bench.md): continuous RX keeps receiving
// after each RX_DONE; retune time; CRC length on air (RadioLib's FLRC CRC
// setting is shared with GFSK, check against an LR2021 if both are mixed);
// PA gain and LNA gain of the module.
#include "radio_impl.h"

namespace {

// Exposes the buffer access RadioLib keeps protected.
class Sx1280Dev : public SX1280 {
  public:
    explicit Sx1280Dev(Module *m) : SX1280(m) {}
    using SX128x::clearIrqStatus;
    using SX128x::readBuffer;
};

// RadioLib takes FLRC rates in kbit/s and names 1.04 Mbit/s "1000".
float kbps(uint32_t bps) {
    return bps == 1040000 ? 1000.0f : (float)(bps / 1000.0);
}

struct Sx1280 : radio {
    Sx1280Dev dev;
    bool in_rx = false;

    explicit Sx1280(const radio_pins *p) : radio(p), dev(&mod) {}

    int setFreq(uint32_t hz) override {
        return radio_check_st("setFrequency", dev.setFrequency((float)(hz / 1e6)));
    }

    int calibrate(uint32_t hz) override {
        // One band and one PA: nothing to recalibrate beyond the synthesizer.
        if (setFreq(hz))
            return -1;
        return radio_check_st("setOutputPower", dev.setOutputPower(power_dbm));
    }

    int setPower(int8_t dbm) override {
        power_dbm = dbm;
        return radio_check_st("setOutputPower", dev.setOutputPower(dbm));
    }

    int setModulation(uint32_t bitrate_bps, uint8_t cr) override {
        if (radio_check_st("setBitRate", dev.setBitRate(kbps(bitrate_bps))))
            return -1;
        return radio_check_st("setCodingRate", dev.setCodingRate(cr)); // RadioLib: 2, 3, 4
    }

    int tx(const uint8_t *buf, size_t len) override {
        if (len > 127) {
            fprintf(stderr, "radio: SX1280 FLRC packet of %zu bytes (max 127)\n", len);
            return -1;
        }
        if (in_rx && standby())
            return -1;
        return radio_check_st("startTransmit", dev.startTransmit(buf, len));
    }

    int rx() override {
        in_rx = radio_check_st("startReceive", dev.startReceive()) == 0;
        return in_rx ? 0 : -1;
    }

    int standby() override {
        in_rx = false;
        return radio_check_st("standby", dev.standby());
    }

    radio_event onIrq(uint8_t *buf, size_t cap, size_t *len, radio_rx_info *info) override {
        uint16_t irq = dev.getIrqStatus();
        if (irq & RADIOLIB_SX128X_IRQ_TX_DONE) {
            dev.clearIrqStatus();
            return RADIO_EV_TX_DONE;
        }
        if (irq & RADIOLIB_SX128X_IRQ_RX_DONE) {
            uint8_t offset = 0;
            size_t n = dev.getPacketLength(true, &offset);
            if (n > cap)
                n = cap;
            int16_t st = dev.readBuffer(buf, (uint8_t)n, offset);
            *len = n;
            info->crc_ok = !(irq & RADIOLIB_SX128X_IRQ_CRC_ERROR) && st == RADIOLIB_ERR_NONE;
            info->rssi_dbm = (int16_t)dev.getRSSI();
            info->snr_q4 = 0;
            dev.clearIrqStatus();
            return st == RADIOLIB_ERR_NONE ? RADIO_EV_RX_DONE : RADIO_EV_ERROR;
        }
        dev.clearIrqStatus(); // sync word error, timeouts: ignore
        return RADIO_EV_NONE;
    }
};

} // namespace

radio *sx1280_open(const radio_pins *pins, const radio_flrc *f) {
    Sx1280 *r = new Sx1280(pins);
    static const uint8_t modes[3] = {Module::MODE_IDLE, Module::MODE_RX, Module::MODE_TX};
    if (r->rfSwitchTable(pins, modes, 3))
        r->dev.setRfSwitchTable(r->rf_pins, r->rf_modes);
    if (f->tcxo_mv)
        fprintf(stderr, "radio: tcxo_mv ignored, the SX1280 has no TCXO supply pin\n");

    ConfigFLRC_t cfg;
    cfg.frequency = (float)(f->freq_hz / 1e6);
    cfg.bitRate = kbps(f->bitrate_bps);
    cfg.codingRate = f->cr; // the SX128x driver takes 2, 3, 4 here, not RADIOLIB_FLRC_CR_*
    cfg.power = f->power_dbm;
    cfg.preambleLength = f->preamble_bits; // 4..32, multiple of 4
    cfg.dataShaping = RADIOLIB_SHAPING_0_5;
    r->power_dbm = f->power_dbm;
    r->hal.init();
    if (!r->hal.ok()) {
        fprintf(stderr, "radio: cannot open %s\n", pins->spidev);
        delete r;
        return nullptr;
    }
    uint8_t sync[4] = {(uint8_t)(f->sync_word >> 24), (uint8_t)(f->sync_word >> 16),
                       (uint8_t)(f->sync_word >> 8), (uint8_t)f->sync_word};
    // Variable length last: the other packet-parameter calls reset the RX
    // length filter to 255, beyond the FLRC limit.
    if (radio_check_st("beginFLRC", r->dev.beginFLRC(cfg)) ||
        radio_check_st("setSyncWord", r->dev.setSyncWord(sync, 4)) ||
        radio_check_st("setCRC", r->dev.setCRC(f->crc_bytes ? f->crc_bytes : 2)) ||
        radio_check_st("variablePacketLengthMode", r->dev.variablePacketLengthMode(127))) {
        delete r;
        return nullptr;
    }
    return r;
}
