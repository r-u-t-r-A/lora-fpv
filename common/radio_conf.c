// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "radio_conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        const char *k = line, *v = eq + 1;
        int tbl[RADIO_RF_MODES];
        if (!strcmp(k, "spidev"))
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
