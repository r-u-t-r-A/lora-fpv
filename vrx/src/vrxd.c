// SPDX-License-Identifier: GPL-3.0-or-later
// VRX daemon: receives OTA packets and streams them to the browser GUI.
// Backends:
//   --radio         receive with the radio (vrxd: LR2021 or SX1280 as set in
//                   the radio conf, vrxd_sim: simulated ether, see
//                   sim/radio_sim.c), hop sync, mode scan, per-channel stats
//   --replay FILE   play a sim/loopback packet log in real time (laptop GUI work)
#define _POSIX_C_SOURCE 200809L

#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fhss.h"
#include "httpd.h"
#include "olv_util.h"
#include "modes.h"
#include "ota.h"
#include "radio_conf.h"
#include "rxlink.h"
#include "vrx_proto.h"
#include "vrxlog.h"

struct vrx_conf {
    int id;
    char band[8];  // "auto", "868" or "2400": which bands mode=auto scans
    char mode[16]; // "auto" (scan), lr520 lr260 hq2600 hq1300
    char bind[64];
};

struct replay {
    uint8_t *data;
    size_t len, pos;
    uint64_t t0_log, t0_wall;
    bool loop;
    double speed;
    uint32_t loops;
};

struct vrxd {
    struct vrx_conf conf;
    const char *conf_path;
    struct httpd *http;
    struct replay rp;
    struct rxlink *link; // radio backend, NULL in replay mode
    struct vrx_status st;
    uint64_t start_us;
};

static volatile sig_atomic_t stop;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// ------------------------------------------------------------------ config

static void conf_defaults(struct vrx_conf *c) {
    c->id = 1;
    snprintf(c->band, sizeof(c->band), "auto");
    snprintf(c->mode, sizeof(c->mode), "auto");
    snprintf(c->bind, sizeof(c->bind), "open-lr-video");
}

static void conf_set(struct vrx_conf *c, const char *k, const char *v) {
    if (!strcmp(k, "id"))
        c->id = atoi(v);
    else if (!strcmp(k, "band"))
        snprintf(c->band, sizeof(c->band), "%s", v);
    else if (!strcmp(k, "mode"))
        snprintf(c->mode, sizeof(c->mode), "%s", v);
    else if (!strcmp(k, "bind"))
        snprintf(c->bind, sizeof(c->bind), "%s", v);
}

// /etc/vrx.conf: key=value lines, # comments.
static void conf_load(struct vrx_conf *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        conf_set(c, line, eq + 1);
    }
    fclose(f);
}

static int conf_save(const struct vrx_conf *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "id=%d\nband=%s\nmode=%s\nbind=%s\n", c->id, c->band, c->mode, c->bind);
    return fclose(f);
}

static bool valid_mode(const char *m) {
    return !strcmp(m, "auto") || olv_mode_find(m) != NULL;
}

// Extracts "key": "string" or "key": number from a flat JSON object.
static bool json_get(const char *body, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p)
        return false;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < cap) {
            if (*p == '\\' && p[1])
                p++;
            out[n++] = *p++;
        }
    } else {
        while (*p && (*p == '-' || (*p >= '0' && *p <= '9')) && n + 1 < cap)
            out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 3 < cap; in++) {
        if (*in == '"' || *in == '\\')
            out[n++] = '\\';
        if ((unsigned char)*in >= 0x20)
            out[n++] = *in;
    }
    out[n] = 0;
}

static int api(void *ctx, const char *method, const char *path, const char *body, size_t body_len,
               char *resp, size_t cap) {
    struct vrxd *d = ctx;
    (void)body_len;
    if (!strcmp(path, "/api/stats")) {
        if (!d->link) {
            snprintf(resp, cap, "{\"error\":\"no radio in replay mode\"}");
            return 404;
        }
        rxlink_stats_json(d->link, resp, cap);
        return 200;
    }
    if (strcmp(path, "/api/config") != 0) {
        snprintf(resp, cap, "{\"error\":\"unknown endpoint\"}");
        return 404;
    }
    if (!strcmp(method, "POST")) {
        struct vrx_conf c = d->conf;
        char v[64];
        if (json_get(body, "band", v, sizeof(v)))
            conf_set(&c, "band", v);
        if (json_get(body, "mode", v, sizeof(v)))
            conf_set(&c, "mode", v);
        if (json_get(body, "bind", v, sizeof(v)))
            conf_set(&c, "bind", v);
        if (json_get(body, "id", v, sizeof(v)))
            conf_set(&c, "id", v);
        if (!valid_mode(c.mode) || olv_band_parse(c.band) < 0 || c.id < 1 || c.id > 254) {
            snprintf(resp, cap, "{\"error\":\"invalid band, mode or id\"}");
            return 400;
        }
        d->conf = c;
        if (d->conf_path && conf_save(&d->conf, d->conf_path) != 0) {
            snprintf(resp, cap, "{\"error\":\"could not save config\"}");
            return 500;
        }
        // Takes effect when vrxd restarts (the radio is set up from it at start).
    }
    char bind[160];
    json_escape(d->conf.bind, bind, sizeof(bind));
    snprintf(resp, cap,
             "{\"id\":%d,\"band\":\"%s\",\"mode\":\"%s\",\"active_mode\":\"%s\","
             "\"bind\":\"%s\",\"link_id\":%u,\"backend\":\"%s\",\"clients\":%u}",
             d->conf.id, d->conf.band, d->conf.mode, d->link ? d->link->mode->name : d->conf.mode,
             bind, ota_link_id(d->conf.bind), d->rp.data ? "replay" : "radio",
             httpd_ws_clients(d->http));
    return 200;
}

// ------------------------------------------------------------------ replay

static int replay_open(struct replay *r, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    if (!vrxlog_check_header(f)) {
        fprintf(stderr, "%s: not a vrxlog file\n", path);
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 8, SEEK_SET);
    r->len = (size_t)(sz - 8);
    r->data = malloc(r->len);
    if (!r->data || fread(r->data, 1, r->len, f) != r->len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    r->pos = 0;
    r->t0_wall = 0;
    return 0;
}

// Sends every record that is due, batched into WebSocket messages.
static void replay_step(struct vrxd *d, uint64_t now) {
    struct replay *r = &d->rp;
    uint8_t msg[VRX_MSG_MAX];
    size_t msg_len = 0;
    for (;;) {
        if (r->pos + 10 > r->len) {
            if (!r->loop)
                break;
            r->pos = 0;
            r->t0_wall = 0;
            r->loops++;
        }
        const uint8_t *p = r->data + r->pos;
        uint64_t t = olv_get_u32(p) | ((uint64_t)olv_get_u32(p + 4) << 32);
        uint16_t len = olv_get_u16(p + 8);
        if (r->pos + 10 + len > r->len) {
            r->pos = r->len;
            continue;
        }
        if (!r->t0_wall) {
            r->t0_wall = now;
            r->t0_log = t;
        }
        uint64_t due = r->t0_wall + (uint64_t)((double)(t - r->t0_log) / r->speed);
        if (due > now)
            break;
        size_t nl = vrx_rec_append(msg, msg_len, sizeof(msg), VRX_REC_RX_PACKET, p + 10, len);
        if (!nl) {
            httpd_ws_broadcast(d->http, msg, msg_len);
            msg_len = 0;
            continue;
        }
        msg_len = nl;
        d->st.pkts_ok++;
        struct vrx_rx_packet rx;
        if (vrx_rx_packet_unpack(p + 10, len, &rx) == 0)
            d->st.hop = rx.hop;
        r->pos += 10u + len;
    }
    if (msg_len)
        httpd_ws_broadcast(d->http, msg, msg_len);
}

static void send_status(struct vrxd *d, uint64_t now) {
    d->st.vrx_id = (uint8_t)d->conf.id;
    d->st.band = strcmp(d->conf.band, "2400") == 0;
    if (d->link) {
        const struct rxlink *l = d->link;
        d->st.band = olv_mode_is_2g4(l->mode);
        d->st.sync_state = (uint8_t)l->sync.state;
        d->st.hop = l->sync.hop;
        d->st.cycle = l->sync.cycle;
        d->st.pkts_ok = l->pkts_ok;
        d->st.pkts_crc = l->pkts_crc;
        d->st.locks = (uint16_t)l->sync.locks;
    } else {
        d->st.sync_state = FHSS_RX_LOCKED;
    }
    d->st.pkts_drop = httpd_ws_dropped(d->http);
    d->st.noise_dbm = INT16_MIN;
    d->st.uptime_ms = (uint32_t)((now - d->start_us) / 1000u);
    uint8_t pl[VRX_STATUS_LEN], msg[64];
    vrx_status_pack(&d->st, pl);
    size_t n = vrx_rec_append(msg, 0, sizeof(msg), VRX_REC_STATUS, pl, sizeof(pl));
    httpd_ws_broadcast(d->http, msg, n);
}

static void emit_ws(void *ctx, const uint8_t *msg, size_t len) {
    httpd_ws_broadcast(ctx, msg, len);
}

static void usage(void) {
    fprintf(stderr, "usage: vrxd [--config /etc/vrx.conf] [--listen ADDR] [--port 8080]\n"
                    "            [--www DIR] [--id N]\n"
                    "            --radio [--radio-conf " RADIO_CONF_PATH "] [--capture FILE]\n"
                    "          | --replay FILE [--loop] [--speed X]\n");
}

int main(int argc, char **argv) {
    static struct vrxd d;
    conf_defaults(&d.conf);
    const char *listen_addr = NULL, *www = NULL, *replay_path = NULL;
    const char *radio_conf = RADIO_CONF_PATH, *capture = NULL;
    bool use_radio = false;
    int port = 8080, id = 0;
    d.rp.speed = 1.0;
    static const struct option lo[] = {{"config", 1, 0, 'c'},
                                       {"listen", 1, 0, 'l'},
                                       {"port", 1, 0, 'p'},
                                       {"www", 1, 0, 'w'},
                                       {"id", 1, 0, 'i'},
                                       {"replay", 1, 0, 'r'},
                                       {"loop", 0, 0, 'L'},
                                       {"speed", 1, 0, 's'},
                                       {"radio", 0, 0, 'R'},
                                       {"radio-conf", 1, 0, 'C'},
                                       {"capture", 1, 0, 'P'},
                                       {"help", 0, 0, 'h'},
                                       {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "c:l:p:w:i:r:Ls:h", lo, NULL)) != -1) {
        switch (c) {
        case 'c':
            d.conf_path = optarg;
            break;
        case 'l':
            listen_addr = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'w':
            www = optarg;
            break;
        case 'i':
            id = atoi(optarg);
            break;
        case 'r':
            replay_path = optarg;
            break;
        case 'L':
            d.rp.loop = true;
            break;
        case 's':
            d.rp.speed = atof(optarg);
            break;
        case 'R':
            use_radio = true;
            break;
        case 'C':
            radio_conf = optarg;
            break;
        case 'P':
            capture = optarg;
            break;
        default:
            usage();
            return c == 'h' ? 0 : 2;
        }
    }
    if (d.conf_path)
        conf_load(&d.conf, d.conf_path);
    if (id)
        d.conf.id = id;
    if (d.rp.speed <= 0)
        d.rp.speed = 1.0;
    static struct rxlink link;
    if (use_radio == (replay_path != NULL)) {
        usage();
        return 2;
    }
    if (replay_path && replay_open(&d.rp, replay_path))
        return 1;
    if (use_radio) {
        struct radio_pins pins;
        struct radio_flrc flrc;
        int band = olv_band_parse(d.conf.band);
        if (!valid_mode(d.conf.mode) || band < 0) {
            fprintf(stderr, "vrxd: invalid band=%s / mode=%s\n", d.conf.band, d.conf.mode);
            return 2;
        }
        if (radio_conf_load(radio_conf, &pins, &flrc) < 0)
            return 2;
        const struct radio_caps *caps = radio_chip_caps(pins.chip);
        const struct olv_mode *modes[RXLINK_MAX_MODES];
        size_t n_modes = olv_modes_select(d.conf.mode, band, caps, modes, RXLINK_MAX_MODES);
        if (n_modes == 0) {
            fprintf(stderr, "vrxd: the %s cannot receive mode=%s in band=%s\n", caps->name,
                    d.conf.mode, d.conf.band);
            return 2;
        }
        flrc.cr = 0; // each mode's default
        if (rxlink_open(&link, modes, n_modes, d.conf.bind, &pins, &flrc))
            return 1;
        fprintf(stderr, "vrxd %d: %s, scanning", d.conf.id, caps->name);
        for (size_t i = 0; i < n_modes; i++)
            fprintf(stderr, " %s", modes[i]->name);
        fprintf(stderr, "\n");
        if (capture) {
            link.capture = fopen(capture, "wb");
            if (!link.capture) {
                perror(capture);
                return 1;
            }
            vrxlog_write_header(link.capture);
        }
        d.link = &link;
    }

    struct httpd_cfg hc = {
        .bind_addr = listen_addr, .port = (uint16_t)port, .www_root = www, .api = api, .ctx = &d};
    d.http = httpd_open(&hc);
    if (!d.http)
        return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (d.link)
        fprintf(stderr, "vrxd %d: ws://%s:%d/rx, radio %s, bind \"%s\"\n", d.conf.id,
                listen_addr ? listen_addr : "0.0.0.0", port, d.conf.mode, d.conf.bind);
    else
        fprintf(stderr, "vrxd %d: ws://%s:%d/rx, replaying %s%s\n", d.conf.id,
                listen_addr ? listen_addr : "0.0.0.0", port, replay_path,
                d.rp.loop ? " (loop)" : "");

    d.start_us = now_us();
    uint64_t next_status = d.start_us;
    while (!stop) {
        if (d.link) {
            // Radio first: it bounds its own wait by the next hop boundary.
            rxlink_step(d.link, 2000, emit_ws, d.http);
            if (httpd_poll(d.http, 0) < 0)
                break;
        } else {
            if (httpd_poll(d.http, 2) < 0)
                break;
        }
        uint64_t now = now_us();
        // Do not burn through the log while nobody is watching.
        if (!d.link && httpd_ws_clients(d.http))
            replay_step(&d, now);
        else
            d.rp.t0_wall = 0;
        if (now >= next_status) {
            send_status(&d, now);
            next_status = now + 250000u;
        }
    }
    if (d.link) {
        char js[8192];
        rxlink_stats_json(d.link, js, sizeof(js));
        fprintf(stderr, "vrxd %d: %s\n", d.conf.id, js);
        if (d.link->capture)
            fclose(d.link->capture);
        rxlink_close(d.link);
    }
    httpd_close(d.http);
    free(d.rp.data);
    return 0;
}
