// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "radio_conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Strips leading and trailing blanks in place.
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    return s;
}

static void parse_list(const char *s, int *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = -1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    int i = 0;
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t && i < n;
         t = strtok_r(NULL, ",", &save))
        out[i++] = (int)strtol(t, NULL, 0);
}

// Ebyte E28-2G4M27S(X): SX1280 + PA/LNA, TXEN/RXEN driven by the host (high =
// path on, never both). Numbers from the ExpressLRS "DIY 2400 E28" target:
// SX1280 -15..-1 dBm gives 10..250 mW (about 25 dB of PA gain), LNA gain 12 dB,
// and ExpressLRS never drives the PA above -1 dBm. Wire rf_pins=TXEN,RXEN.
static const struct module_preset {
    const char *name;
    enum radio_chip chip;
    uint8_t rf_table[RADIO_RF_MODES];
    int8_t pa_gain_db, lna_gain_db, max_power_dbm, power_dbm;
} presets[] = {
    {"e28-2g4m27s", RADIO_CHIP_SX1280, {0, 2, 1, 2, 1}, 25, 12, -1, -18},
};

int radio_conf_module(const char *name, struct radio_pins *p, struct radio_flrc *f) {
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        const struct module_preset *m = &presets[i];
        if (strcmp(m->name, name))
            continue;
        p->chip = m->chip;
        memcpy(p->rf_table, m->rf_table, sizeof(p->rf_table));
        p->pa_gain_db = m->pa_gain_db;
        p->lna_gain_db = m->lna_gain_db;
        p->max_power_dbm = m->max_power_dbm;
        p->has_max_power = true;
        f->power_dbm = m->power_dbm;
        return 0;
    }
    return -1;
}

int radio_conf_load(const char *path, struct radio_pins *p, struct radio_flrc *f) {
    static char spidev[128] = "/dev/spidev0.0";
    memset(p, 0, sizeof(*p));
    p->spidev = spidev;
    p->spi_hz = 8000000;
    p->busy = p->irq = p->nreset = -1;
    for (int i = 0; i < RADIO_RF_PINS; i++)
        p->rf_pin[i] = -1;
    f->preamble_bits = 32;
    f->sync_word = 0x2D014B1Du;
    f->crc_bytes = 2;
    f->power_dbm = -9;
    f->tcxo_mv = 0;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return 1;
    char line[256];
    int rc = 0, lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *nl = strpbrk(line, "\r\n#");
        if (nl)
            *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) {
            if (strspn(line, " \t") != strlen(line)) {
                fprintf(stderr, "%s:%d: expected key=value\n", path, lineno);
                rc = -1;
            }
            continue;
        }
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (*v == 0)
            continue; // "busy=" in a template: not filled in yet
        int tbl[RADIO_RF_MODES];
        int chip;
        if (!strcmp(k, "chip")) {
            if ((chip = radio_chip_parse(v)) < 0) {
                fprintf(stderr, "%s:%d: unknown chip '%s' (lr2021, sx1280)\n", path, lineno, v);
                rc = -1;
            } else
                p->chip = (enum radio_chip)chip;
        } else if (!strcmp(k, "module")) {
            if (radio_conf_module(v, p, f)) {
                fprintf(stderr, "%s:%d: unknown module '%s' (e28-2g4m27s)\n", path, lineno, v);
                rc = -1;
            }
        } else if (!strcmp(k, "pa_gain_db"))
            p->pa_gain_db = (int8_t)atoi(v);
        else if (!strcmp(k, "lna_gain_db"))
            p->lna_gain_db = (int8_t)atoi(v);
        else if (!strcmp(k, "max_power_dbm")) {
            p->max_power_dbm = (int8_t)atoi(v);
            p->has_max_power = true;
        } else if (!strcmp(k, "spidev"))
            snprintf(spidev, sizeof(spidev), "%s", v);
        else if (!strcmp(k, "spi_hz"))
            p->spi_hz = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(k, "busy"))
            p->busy = atoi(v);
        else if (!strcmp(k, "irq"))
            p->irq = atoi(v);
        else if (!strcmp(k, "nreset"))
            p->nreset = atoi(v);
        else if (!strcmp(k, "rf_pins"))
            parse_list(v, p->rf_pin, RADIO_RF_PINS);
        else if (!strcmp(k, "rf_table")) {
            parse_list(v, tbl, RADIO_RF_MODES);
            for (int i = 0; i < RADIO_RF_MODES; i++)
                p->rf_table[i] = (uint8_t)(tbl[i] < 0 ? 0 : tbl[i]);
        } else if (!strcmp(k, "tcxo_mv"))
            f->tcxo_mv = (uint16_t)atoi(v);
        else if (!strcmp(k, "power_dbm"))
            f->power_dbm = (int8_t)atoi(v);
        else if (!strcmp(k, "preamble_bits"))
            f->preamble_bits = (uint16_t)atoi(v);
        else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, k);
            rc = -1;
        }
    }
    fclose(fp);
    return rc;
}

int radio_conf_complete(const struct radio_pins *p) {
    return p->busy >= 0 && p->irq >= 0 && p->nreset >= 0;
}
