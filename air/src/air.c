// SPDX-License-Identifier: GPL-3.0-or-later
// Air unit: video source -> H.265 access units -> packetizer + FEC -> air
// scheduler (100 Hz hop clock, beacons) -> radio.
//
// Video sources on the host: an IVF file of H.265 access units (e.g.
// sim/loopback's reference.ivf), or a raw Annex-B stream on stdin (e.g.
// ffmpeg ... -c:v libx265 -x265-params aud=1:... -f hevc -). The RV1103
// encoder source (Rockchip MPI) plugs in here later (air/src/enc_rk.c).
#define _GNU_SOURCE // clock_nanosleep, sched_setscheduler, mlockall

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "airsched.h"
#include "fhss.h"
#include "h265.h"
#include "modes.h"
#include "olv_util.h"
#include "ota.h"
#include "radio.h"
#include "radio_conf.h"
#include "vpkt.h"

static volatile sig_atomic_t stop;

static void on_signal(int s) {
    (void)s;
    stop = 1;
}

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_until_us(uint64_t t) {
    struct timespec ts = {(time_t)(t / 1000000u), (long)(t % 1000000u) * 1000};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !stop) {
    }
}

// ------------------------------------------------------------ video sources

struct source {
    FILE *f;
    bool ivf, loop;
    uint8_t *buf; // stdin: accumulated stream
    size_t len, cap;
};

static int source_open(struct source *s, const char *path, bool loop) {
    memset(s, 0, sizeof(*s));
    s->loop = loop;
    if (!strcmp(path, "-")) {
        s->f = stdin;
        s->cap = 1 << 20;
        s->buf = malloc(s->cap);
        return s->buf ? 0 : -1;
    }
    s->f = fopen(path, "rb");
    if (!s->f) {
        perror(path);
        return -1;
    }
    uint8_t h[32];
    if (fread(h, 1, 32, s->f) != 32 || memcmp(h, "DKIF", 4) != 0) {
        fprintf(stderr, "%s: not an IVF file (use - for an Annex-B stream on stdin)\n", path);
        return -1;
    }
    s->ivf = true;
    return 0;
}

// Next access unit into au (cap bytes). Returns its length, 0 at end.
static size_t source_next(struct source *s, uint8_t *au, size_t cap) {
    if (s->ivf) {
        for (int attempt = 0; attempt < 2; attempt++) {
            uint8_t fh[12];
            if (fread(fh, 1, 12, s->f) == 12) {
                uint32_t n = olv_get_u32(fh);
                if (n > cap) {
                    fseek(s->f, n, SEEK_CUR);
                    return source_next(s, au, cap);
                }
                if (fread(au, 1, n, s->f) == n)
                    return n;
            }
            if (!s->loop)
                return 0;
            fseek(s->f, 32, SEEK_SET);
        }
        return 0;
    }
    // Annex B on stdin: an AU is complete once the next one has started.
    for (;;) {
        size_t pos = 0, first = SIZE_MAX, second = SIZE_MAX;
        struct h265_nal nal;
        bool vcl = false;
        while (h265_next_nal(s->buf, s->len, &pos, &nal)) {
            bool starts = (nal.type == H265_NAL_AUD || nal.type == H265_NAL_VPS ||
                           (h265_is_vcl(nal.type) && h265_first_slice(s->buf, &nal)));
            if (first == SIZE_MAX) {
                first = nal.start;
            } else if (starts && vcl) {
                second = nal.start;
                break;
            }
            if (h265_is_vcl(nal.type))
                vcl = true;
        }
        if (second != SIZE_MAX) {
            size_t n = second - first;
            if (n > cap)
                n = cap;
            memcpy(au, s->buf + first, n);
            memmove(s->buf, s->buf + second, s->len - second);
            s->len -= second;
            return n;
        }
        if (s->len == s->cap) {
            s->cap *= 2;
            s->buf = realloc(s->buf, s->cap);
        }
        size_t r = fread(s->buf + s->len, 1, s->cap - s->len, s->f);
        if (r == 0) {
            if (s->len == 0 || first == SIZE_MAX)
                return 0;
            size_t n = s->len - first < cap ? s->len - first : cap; // last AU
            memcpy(au, s->buf + first, n);
            s->len = 0;
            return n;
        }
        s->len += r;
    }
}

// --------------------------------------------------------------------- main

struct enq_ctx {
    struct airsched *as;
};

static void enqueue(void *ctx, const uint8_t *pkt, size_t len) {
    struct enq_ctx *e = ctx;
    airsched_enqueue(e->as, pkt, len);
}

static void usage(void) {
    fprintf(stderr,
            "usage: air --source FILE.ivf|- [options]\n"
            "  --mode lr520|lr260|hq2600|hq1300   (default lr520; hq1300 on a 2.4 GHz-only chip)\n"
            "  --bind PHRASE        (default open-lr-video)\n"
            "  --fps N              frame rate of the source (default: mode)\n"
            "  --loop               loop an IVF source\n"
            "  --k N --n N          FEC shape (default 8/12)\n"
            "  --shard N            shard bytes (default: best fit per hop)\n"
            "  --max-packet N       packet size limit (default: the chip's, 255 LR2021 / 127\n"
            "                       SX1280; use 127 when SX1280 VRXs listen to an LR2021 air)\n"
            "  --cr N               FLRC coding rate (2=1/2 3=3/4 4=1 5=2/3; default: mode)\n"
            "  --radio-conf PATH    wiring (default " RADIO_CONF_PATH ")\n"
            "  --power DBM          chip output (default from radio conf, -9)\n"
            "  --outdoor            allow > 0 dBm\n"
            "  --rt                 SCHED_FIFO + mlockall\n"
            "  --seconds S          stop after S seconds\n");
}

int main(int argc, char **argv) {
    const char *src_path = NULL, *bind = "open-lr-video", *conf = RADIO_CONF_PATH;
    const struct olv_mode *mode = NULL;
    int fps = 0, k = 8, n = 12, shard = 0, cr = 0, power = 1000, max_packet = 0;
    bool loop = false, outdoor = false, rt = false;
    double seconds = 0;
    enum {
        O_SRC = 256,
        O_MODE,
        O_BIND,
        O_FPS,
        O_LOOP,
        O_K,
        O_N,
        O_SHARD,
        O_CR,
        O_CONF,
        O_POWER,
        O_OUTDOOR,
        O_RT,
        O_SEC,
        O_MAXPKT
    };
    static const struct option lo[] = {{"source", 1, 0, O_SRC},
                                       {"mode", 1, 0, O_MODE},
                                       {"bind", 1, 0, O_BIND},
                                       {"fps", 1, 0, O_FPS},
                                       {"loop", 0, 0, O_LOOP},
                                       {"k", 1, 0, O_K},
                                       {"n", 1, 0, O_N},
                                       {"shard", 1, 0, O_SHARD},
                                       {"cr", 1, 0, O_CR},
                                       {"radio-conf", 1, 0, O_CONF},
                                       {"power", 1, 0, O_POWER},
                                       {"outdoor", 0, 0, O_OUTDOOR},
                                       {"rt", 0, 0, O_RT},
                                       {"seconds", 1, 0, O_SEC},
                                       {"max-packet", 1, 0, O_MAXPKT},
                                       {"help", 0, 0, 'h'},
                                       {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case O_SRC:
            src_path = optarg;
            break;
        case O_MODE:
            if (!(mode = olv_mode_find(optarg))) {
                fprintf(stderr, "unknown mode %s\n", optarg);
                return 2;
            }
            break;
        case O_BIND:
            bind = optarg;
            break;
        case O_FPS:
            fps = atoi(optarg);
            break;
        case O_LOOP:
            loop = true;
            break;
        case O_K:
            k = atoi(optarg);
            break;
        case O_N:
            n = atoi(optarg);
            break;
        case O_SHARD:
            shard = atoi(optarg);
            break;
        case O_CR:
            cr = atoi(optarg);
            break;
        case O_CONF:
            conf = optarg;
            break;
        case O_POWER:
            power = atoi(optarg);
            break;
        case O_OUTDOOR:
            outdoor = true;
            break;
        case O_RT:
            rt = true;
            break;
        case O_SEC:
            seconds = atof(optarg);
            break;
        case O_MAXPKT:
            max_packet = atoi(optarg);
            break;
        default:
            usage();
            return c == 'h' ? 0 : 2;
        }
    }
    if (!src_path) {
        usage();
        return 2;
    }

    struct radio_pins pins;
    struct radio_flrc flrc;
    if (radio_conf_load(conf, &pins, &flrc) < 0)
        return 2;
    const struct radio_caps *caps = radio_chip_caps(pins.chip);
    if (!mode)
        mode = olv_mode_find(caps->sub_ghz ? "lr520" : "hq1300");
    if (!olv_mode_supported(mode, caps)) {
        fprintf(stderr, "the %s cannot run %s (%s, %u kbit/s)\n", caps->name, mode->name,
                olv_mode_is_2g4(mode) ? "2.4 GHz" : "868 MHz", mode->bitrate_bps / 1000);
        return 2;
    }
    if (max_packet <= 0 || max_packet > caps->max_packet)
        max_packet = caps->max_packet;
    if (!fps)
        fps = mode->fps;
    if (power != 1000)
        flrc.power_dbm = (int8_t)power;
    if (flrc.power_dbm > 0 && !outdoor) {
        fprintf(stderr, "refusing %d dBm without --outdoor\n", flrc.power_dbm);
        return 2;
    }
    int port_dbm = radio_conf_port_dbm(&pins, flrc.power_dbm);
    if (!outdoor && port_dbm > 0)
        fprintf(stderr,
                "air: WARNING: ~%+d dBm at the antenna port even at this setting (PA gain %d dB); "
                "on the bench use at least %d dB of attenuation\n",
                port_dbm, pins.pa_gain_db, port_dbm + 40);
    flrc.freq_hz = olv_band_center(&mode->band);
    flrc.bitrate_bps = mode->bitrate_bps;
    flrc.cr = (uint8_t)(cr ? cr : mode->cr);

    uint32_t seed = fhss_seed(bind);
    uint16_t link_id = ota_link_id(bind);
    static struct airsched as;
    struct airsched_cfg sc = {.phy = flrc_phy_default(flrc.bitrate_bps, flrc.cr),
                              .hop_us = 10000,
                              .retune_us = 200,
                              .beacon_every = 1,
                              .beacons_per_hop = mode->beacons_per_hop,
                              .seed = seed,
                              .n_channels = mode->band.n,
                              .link_id = link_id,
                              .mode = (uint8_t)mode->mode,
                              .fec_k = (uint8_t)k,
                              .fec_n = (uint8_t)n,
                              .tx_power = flrc.power_dbm,
                              .queue_max = 256,
                              .max_packet = (uint16_t)max_packet};
    unsigned per_hop;
    uint32_t shard_bps;
    size_t best = airsched_best_packet(&sc, &per_hop, &shard_bps);
    if (!shard)
        shard = best > OTA_HDR_LEN ? (int)(best - OTA_HDR_LEN) : 0;
    struct vpkt_tx_cfg tc = {.link_id = link_id,
                             .mode = (uint8_t)mode->mode,
                             .k = (uint8_t)k,
                             .n = (uint8_t)n,
                             .max_shard = (uint16_t)shard,
                             .min_shard = (uint16_t)shard};
    struct vpkt_tx tx;
    if (shard <= 0 || shard + OTA_HDR_LEN > max_packet || vpkt_tx_init(&tx, &tc)) {
        fprintf(stderr, "invalid FEC/shard settings\n");
        return 2;
    }
    struct source src;
    if (source_open(&src, src_path, loop))
        return 1;

    if (rt) {
        struct sched_param sp = {.sched_priority = 50};
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0 || mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            perror("air: --rt");
    }
    struct radio *r = radio_open(&pins, &flrc);
    if (!r)
        return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("air: %s on %s, bind \"%s\" (link %u), %u ch, shard %d B x %u/hop (%.0f kbit/s video "
           "at RS %d/%d), %d fps, %d dBm",
           mode->name, caps->name, bind, link_id, mode->band.n, shard, per_hop,
           shard_bps / 1000.0 * k / n, k, n, fps, flrc.power_dbm);
    if (pins.pa_gain_db)
        printf(" (~%+d dBm at the antenna port)", radio_conf_port_dbm(&pins, flrc.power_dbm));
    printf("\n");

    static uint8_t au[VPKT_MAX_FRAME], prep[VPKT_MAX_FRAME];
    struct h265_ps_cache ps = {0};
    struct enq_ctx ectx = {.as = &as};
    uint64_t t0 = mono_us();
    airsched_init(&as, &sc, t0);
    radio_calibrate(r, fhss_freq_hz(&mode->band, fhss_seq_channel(&as.seq, 0, 0)));
    uint64_t frame_us = 1000000u / (uint64_t)fps, next_frame = t0, next_report = t0 + 1000000u;
    uint32_t frames = 0, overruns = 0, tx_errors = 0, late_frames = 0;
    bool eof = false;
    uint8_t cur_ch = 0xFF;

    while (!stop && !(eof && airsched_queued(&as) == 0)) {
        uint64_t now = mono_us();
        if (seconds > 0 && now - t0 >= (uint64_t)(seconds * 1e6))
            break;
        if (!eof && now >= next_frame) {
            size_t len = source_next(&src, au, sizeof(au));
            if (len == 0) {
                eof = true;
            } else {
                bool has_ps;
                size_t plen = h265_prepare_au(&ps, au, len, frames % (uint32_t)fps == 0, prep,
                                              sizeof(prep), &has_ps);
                if (plen)
                    vpkt_tx_frame(&tx, prep, plen, enqueue, &ectx);
                frames++;
            }
            next_frame += frame_us;
            if (now > next_frame + frame_us) { // fell behind: do not try to catch up
                late_frames++;
                next_frame = now + frame_us;
            }
            continue;
        }
        struct airsched_action a;
        airsched_next(&as, now, &a);
        if (a.channel != cur_ch) {
            radio_set_freq(r, fhss_freq_hz(&mode->band, a.channel));
            cur_ch = a.channel;
        }
        if (a.send) {
            uint64_t hop_end = as.hop_start_us + sc.hop_us;
            if (radio_tx(r, a.out, a.len) != 0) {
                tx_errors++;
                continue;
            }
            uint8_t dummy[8];
            size_t dl;
            struct radio_rx_info info;
            enum radio_event ev;
            do
                ev = radio_poll(r, a.airtime_us + 5000, dummy, sizeof(dummy), &dl, &info);
            while (ev == RADIO_EV_RX_DONE);
            if (ev != RADIO_EV_TX_DONE)
                tx_errors++;
            if (mono_us() > hop_end)
                overruns++; // the packet ran into the next hop: timing problem
        } else {
            uint64_t wake = a.wake_us;
            if (!eof && next_frame < wake)
                wake = next_frame;
            sleep_until_us(wake);
        }
        if (now >= next_report) {
            printf("air: %u frames, %u pkts, %u beacons, queue %u, drops %u, overruns %u, late "
                   "frames %u, tx errors %u\n",
                   frames, as.st.sent, as.st.beacons, airsched_queued(&as), as.st.dropped, overruns,
                   late_frames, tx_errors);
            fflush(stdout);
            next_report += 1000000u;
        }
    }
    printf("air: done, %u frames, %u packets, %u beacons, %u drops, %u overruns\n", frames,
           as.st.sent, as.st.beacons, as.st.dropped, overruns);
    radio_close(r);
    vpkt_tx_free(&tx);
    fec_free_cache();
    free(src.buf);
    if (src.f && src.f != stdin)
        fclose(src.f);
    return 0;
}
