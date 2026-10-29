// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "channel.h"
#include "olv_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double uniform(struct channel_model *m) {
    return (olv_xorshift32(&m->rng) >> 8) * (1.0 / 16777216.0);
}

void channel_reset(struct channel_model *m) {
    m->rng = m->seed ? m->seed : 0x2545F491u;
    m->bad = false;
}

int channel_parse(struct channel_model *m, const char *spec) {
    memset(m, 0, sizeof(*m));
    m->ge_loss_bad = 1.0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            fprintf(stderr, "channel: expected key=value, got '%s'\n", tok);
            return -1;
        }
        *eq = 0;
        const char *k = tok, *v = eq + 1;
        if (!strcmp(k, "loss")) {
            m->loss = atof(v);
        } else if (!strcmp(k, "burst")) {
            if (sscanf(v, "%lf/%lf", &m->ge_p_gb, &m->ge_p_bg) != 2) {
                fprintf(stderr, "channel: burst=P_GB/P_BG\n");
                return -1;
            }
        } else if (!strcmp(k, "burstloss")) {
            m->ge_loss_bad = atof(v);
        } else if (!strcmp(k, "bad")) {
            char list[256];
            snprintf(list, sizeof(list), "%s", v);
            for (char *s2 = NULL, *e = strtok_r(list, "+", &s2); e; e = strtok_r(NULL, "+", &s2)) {
                int ch;
                double p;
                if (sscanf(e, "%d:%lf", &ch, &p) != 2 || ch < 0 || ch >= FHSS_MAX_CHANNELS) {
                    fprintf(stderr, "channel: bad=CH:P[+CH:P]\n");
                    return -1;
                }
                m->ch_loss[ch] = p;
            }
        } else if (!strcmp(k, "outage")) {
            if (m->n_outages == CH_MAX_OUTAGES ||
                sscanf(v, "%lf-%lf", &m->outage_from[m->n_outages], &m->outage_to[m->n_outages]) !=
                    2) {
                fprintf(stderr, "channel: outage=T0-T1 (seconds), at most %d\n", CH_MAX_OUTAGES);
                return -1;
            }
            m->n_outages++;
        } else if (!strcmp(k, "seed")) {
            m->seed = (uint32_t)strtoul(v, NULL, 0);
        } else {
            fprintf(stderr, "channel: unknown key '%s'\n", k);
            return -1;
        }
    }
    channel_reset(m);
    return 0;
}

bool channel_lost(struct channel_model *m, uint8_t ch, double t_s) {
    bool lost = false;
    if (m->ge_p_gb > 0) {
        if (m->bad) {
            if (uniform(m) < m->ge_p_bg)
                m->bad = false;
        } else if (uniform(m) < m->ge_p_gb) {
            m->bad = true;
        }
        if (m->bad && uniform(m) < m->ge_loss_bad)
            lost = true;
    }
    if (uniform(m) < m->loss)
        lost = true;
    if (ch < FHSS_MAX_CHANNELS && m->ch_loss[ch] > 0 && uniform(m) < m->ch_loss[ch])
        lost = true;
    for (int i = 0; i < m->n_outages; i++)
        if (t_s >= m->outage_from[i] && t_s < m->outage_to[i])
            lost = true;
    return lost;
}
