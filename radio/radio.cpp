// SPDX-License-Identifier: GPL-3.0-or-later
// common/radio.h for the RV1103: validates the settings against the chip
// (common/radio_caps.c), opens the chip backend and forwards the C calls.
#include "radio_impl.h"

extern "C" uint32_t radio_now_us(void) {
    return (uint32_t)(LinuxHal::monotonicNs() / 1000u);
}

extern "C" struct radio *radio_open(const struct radio_pins *pins, const struct radio_flrc *f) {
    const char *err = radio_check(pins, f);
    if (err) {
        fprintf(stderr, "radio: %s\n", err);
        return nullptr;
    }
    radio *r = pins->chip == RADIO_CHIP_SX1280 ? sx1280_open(pins, f) : lr2021_open(pins, f);
    if (r && !r->hal.armIrq((uint32_t)pins->irq)) {
        delete r;
        return nullptr;
    }
    return r;
}

extern "C" void radio_close(struct radio *r) {
    if (!r)
        return;
    r->standby();
    delete r;
}

extern "C" int radio_set_freq(struct radio *r, uint32_t hz) {
    return r->setFreq(hz);
}

extern "C" int radio_calibrate(struct radio *r, uint32_t hz) {
    return r->calibrate(hz);
}

extern "C" int radio_set_power(struct radio *r, int8_t dbm) {
    return r->setPower(dbm);
}

extern "C" int radio_set_modulation(struct radio *r, uint32_t bitrate_bps, uint8_t cr) {
    return r->setModulation(bitrate_bps, cr);
}

extern "C" int radio_tx(struct radio *r, const uint8_t *buf, size_t len) {
    return r->tx(buf, len);
}

extern "C" int radio_rx(struct radio *r) {
    return r->rx();
}

extern "C" int radio_standby(struct radio *r) {
    return r->standby();
}

extern "C" enum radio_event radio_poll(struct radio *r, uint32_t timeout_us, uint8_t *buf,
                                       size_t cap, size_t *len, struct radio_rx_info *info) {
    uint64_t t_ns = 0;
    int w = r->hal.waitIrq(timeout_us, &t_ns);
    if (w <= 0)
        return w < 0 ? RADIO_EV_ERROR : RADIO_EV_NONE;
    radio_rx_info tmp;
    if (!info)
        info = &tmp;
    info->t_us = (uint32_t)(t_ns / 1000u);
    radio_event ev = r->onIrq(buf, cap, len, info);
    if (ev == RADIO_EV_RX_DONE)
        info->rssi_dbm = (int16_t)(info->rssi_dbm - r->lna_gain_db); // chip sees the LNA's gain
    return ev;
}
