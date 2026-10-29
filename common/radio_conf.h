// SPDX-License-Identifier: GPL-3.0-or-later
// Board wiring and radio defaults from a key=value file (default
// /etc/olv-radio.conf), so air and vrxd do not need pin flags:
//
//   chip=lr2021             (or sx1280)
//   module=e28-2g4m27s      preset for a known module (sets chip, rf_table,
//                           gains, power limit); later lines override it
//   spidev=/dev/spidev0.0   spi_hz=8000000
//   busy=54  irq=55  nreset=56
//   rf_pins=57,58           rf_table=0,1,2,1,2   (stby,rx,tx,rx_hf,tx_hf bitmasks)
//   tcxo_mv=0               power_dbm=-9         preamble_bits=32
//   pa_gain_db=0  lna_gain_db=0  max_power_dbm=N  (module front end)
#ifndef OLV_RADIO_CONF_H
#define OLV_RADIO_CONF_H

#include "radio.h"

#define RADIO_CONF_PATH "/etc/olv-radio.conf"

// Fills defaults, then whatever the file sets. Returns 0 if the file was
// read, 1 if it does not exist (defaults kept), -1 on a bad line.
// The spidev string is stored in static memory.
int radio_conf_load(const char *path, struct radio_pins *pins, struct radio_flrc *flrc);

// Applies a module preset by name; returns -1 if unknown.
int radio_conf_module(const char *name, struct radio_pins *pins, struct radio_flrc *flrc);

// Estimated power at the module's antenna port.
static inline int radio_conf_port_dbm(const struct radio_pins *p, int8_t chip_dbm) {
    return chip_dbm + p->pa_gain_db;
}

// True if busy, irq and nreset are all set.
int radio_conf_complete(const struct radio_pins *pins);

#endif
