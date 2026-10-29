// SPDX-License-Identifier: GPL-3.0-or-later
// Chip capabilities, module presets and which link modes each chip can run.
#define _POSIX_C_SOURCE 200809L
#include "modes.h"
#include "radio.h"
#include "radio_conf.h"
#include "t.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct radio_flrc flrc_at(uint32_t hz, uint32_t bps, uint8_t cr, int8_t dbm) {
    return (struct radio_flrc){.freq_hz = hz, .bitrate_bps = bps, .cr = cr, .power_dbm = dbm};
}

static void test_chip_limits(void) {
    struct radio_pins lr = {.chip = RADIO_CHIP_LR2021}, sx = {.chip = RADIO_CHIP_SX1280};
    struct radio_flrc f = flrc_at(868000000u, 520000, 3, -9);
    CHECK(radio_check(&lr, &f) == NULL);
    CHECK(radio_check(&sx, &f) != NULL); // no sub-GHz
    f = flrc_at(2440000000u, 2600000, 3, -9);
    CHECK(radio_check(&lr, &f) == NULL);
    CHECK(radio_check(&sx, &f) != NULL); // no 2.6 Mbit/s
    f = flrc_at(2440000000u, 1300000, 3, -9);
    CHECK(radio_check(&sx, &f) == NULL);
    f.cr = 5;
    CHECK(radio_check(&sx, &f) != NULL); // no 2/3
    f.cr = 2;
    f.power_dbm = 14;
    CHECK(radio_check(&sx, &f) != NULL);
    f.power_dbm = -18;
    CHECK(radio_check(&sx, &f) == NULL);
    CHECK(radio_chip_caps(RADIO_CHIP_SX1280)->max_packet == 127);
    CHECK(radio_chip_caps(RADIO_CHIP_LR2021)->max_packet == OTA_MAX_PACKET);
    CHECK(radio_chip_parse("sx1280") == RADIO_CHIP_SX1280);
    CHECK(radio_chip_parse("sx1276") == -1);
}

static void test_e28_preset(void) {
    struct radio_pins p = {.chip = RADIO_CHIP_LR2021};
    struct radio_flrc f = flrc_at(2440000000u, 1300000, 3, -9);
    REQUIRE(radio_conf_module("e28-2g4m27s", &p, &f) == 0);
    CHECK(p.chip == RADIO_CHIP_SX1280);
    // rf_pins = TXEN, RXEN: idle both low, RX only RXEN, TX only TXEN.
    CHECK(p.rf_table[RADIO_RF_STBY] == 0);
    CHECK(p.rf_table[RADIO_RF_RX] == 2);
    CHECK(p.rf_table[RADIO_RF_TX] == 1);
    CHECK(f.power_dbm == -18); // lowest setting for the bench
    CHECK(radio_conf_port_dbm(&p, -1) == 24);
    CHECK(radio_check(&p, &f) == NULL);
    f.power_dbm = 0;
    CHECK(radio_check(&p, &f) != NULL); // above the module's -1 dBm drive limit
    CHECK(radio_conf_module("e22", &p, &f) == -1);
}

static void test_conf_file(void) {
    char path[] = "/tmp/olv-radio-conf-XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    fprintf(fp, "module=e28-2g4m27s     # preset\nbusy=54\nirq= 55 \nnreset=56\n"
                "rf_pins=57,58\npower_dbm=-10\nlna_gain_db=11\nspi_hz=\n");
    fclose(fp);
    struct radio_pins p;
    struct radio_flrc f;
    CHECK(radio_conf_load(path, &p, &f) == 0);
    CHECK(p.chip == RADIO_CHIP_SX1280 && p.irq == 55 && p.rf_pin[0] == 57 && p.rf_pin[1] == 58);
    CHECK(f.power_dbm == -10 && p.lna_gain_db == 11 && p.pa_gain_db == 25);
    CHECK(p.spi_hz == 8000000); // empty value keeps the default
    fp = fopen(path, "w");
    fprintf(fp, "chip=sx1281x\n");
    fclose(fp);
    CHECK(radio_conf_load(path, &p, &f) == -1);
    unlink(path);
}

static size_t select_names(const char *mode, const char *band, enum radio_chip chip, char *out) {
    const struct olv_mode *m[4];
    size_t n = olv_modes_select(mode, olv_band_parse(band), radio_chip_caps(chip), m, 4);
    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        strcat(out, i ? " " : "");
        strcat(out, m[i]->name);
    }
    return n;
}

static void test_mode_selection(void) {
    char s[64];
    select_names("auto", "auto", RADIO_CHIP_LR2021, s);
    CHECK(!strcmp(s, "lr520 lr260 hq2600 hq1300"));
    select_names("auto", "2400", RADIO_CHIP_LR2021, s); // 2.4 GHz only
    CHECK(!strcmp(s, "hq2600 hq1300"));
    select_names("auto", "868", RADIO_CHIP_LR2021, s);
    CHECK(!strcmp(s, "lr520 lr260"));
    select_names("auto", "auto", RADIO_CHIP_SX1280, s);
    CHECK(!strcmp(s, "hq1300"));
    CHECK(select_names("auto", "868", RADIO_CHIP_SX1280, s) == 0);
    CHECK(select_names("hq2600", "auto", RADIO_CHIP_SX1280, s) == 0);
    select_names("hq1300", "auto", RADIO_CHIP_SX1280, s);
    CHECK(!strcmp(s, "hq1300"));
    CHECK(olv_band_parse("5800") == -1);
}

int main(void) {
    T_RUN(test_chip_limits);
    T_RUN(test_e28_preset);
    T_RUN(test_conf_file);
    T_RUN(test_mode_selection);
    T_DONE();
}
