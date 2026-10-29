// SPDX-License-Identifier: GPL-3.0-or-later
// Loopback test bench: encode a clip with x265 (via ffmpeg) using the air
// unit's encoder settings, run it through the real packetizer, FEC, air
// scheduler and hop sequence, drop packets independently for each simulated
// receiver, acquire and track the hop sequence on each receiver, merge all
// receivers, and decode the result. Prints link, loss and picture statistics
// and checks the M2 gate (picture recovers under 20 % random loss).
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "airsched.h"
#include "channel.h"
#include "fhss.h"
#include "h265.h"
#include "ivf.h"
#include "modes.h"
#include "ota.h"
#include "vpkt.h"
#include "vrx_proto.h"
#include "vrxlog.h"

#define MAX_RX 4
#define MAX_FRAMES 65000

enum fstate { FS_NONE = 0, FS_COMPLETE, FS_PARTIAL, FS_LOST };

struct stream_out {
    char name[16];
    char path[600];
    FILE *ivf;
    struct vpkt_rx *rx;
    uint8_t *state; // enum fstate per frame
    uint32_t written;
};

struct receiver {
    struct channel_model ch;
    struct fhss_rx sync;
    uint32_t clock_off;
    struct stream_out out;
    FILE *log;
    uint32_t heard, lost, off_channel;
    uint32_t ch_sent[FHSS_MAX_CHANNELS], ch_ok[FHSS_MAX_CHANNELS];
    double first_lock_s;
};

struct opts {
    const char *input;
    const char *label; // text drawn over the picture (the board's NO CAMERA card)
    const char *out_dir;
    const char *bind;
    const struct olv_mode *mode;
    int w, h, fps;
    double seconds;
    double refresh_s;
    bool pir; // column intra refresh instead of periodic IDR
    int kbps;
    int qpmin;
    int k, n, shard, min_shard;
    int cr;
    int max_packet;
    int n_rx;
    const char *rx_spec[MAX_RX];
    bool mp4;
    bool gate;
    double good_db;
};

static void usage(void) {
    fprintf(stderr,
            "usage: loopback [options]\n"
            "  --mode lr520|lr260|hq2600|hq1300   link preset (default lr520)\n"
            "  --input FILE        source clip (default: ffmpeg testsrc2)\n"
            "  --label TEXT        draw TEXT over the picture (needs ffmpeg with drawtext)\n"
            "  --size WxH --fps N  override the preset picture\n"
            "  --seconds S         clip length (default 10)\n"
            "  --kbps N            video CBR target (default: 85%% of link capacity)\n"
            "  --refresh S         refresh period in seconds (IDR interval or PIR sweep)\n"
            "  --pir               column intra refresh instead of periodic IDR (x265's PIR\n"
            "                      does not recover cleanly; see docs/reports)\n"
            "  --qpmin N           QP floor (default 20)\n"
            "  --k N --n N         FEC shape (default 8/12)\n"
            "  --shard N           shard payload bytes (default: best fit per hop)\n"
            "  --min-shard N       smaller blocks use fewer shards below this (default: shard)\n"
            "  --cr 2|3|4|5        FLRC coding rate 1/2, 3/4, 1, 2/3 (default: mode)\n"
            "  --chip lr2021|sx1280  radio chip: packet limit 255 / 127 B (default lr2021)\n"
            "  --rx SPEC           add a receiver (repeatable, max %d); default two of\n"
            "                      'loss=0.2'. SPEC keys: loss=P burst=PGB/PBG burstloss=P\n"
            "                      bad=CH:P[+CH:P] outage=T0-T1 seed=N\n"
            "  --bind PHRASE       bind phrase (default open-lr-video)\n"
            "  --out DIR           output directory (default sim_out)\n"
            "  --mp4               write DIR/compare.mp4 (reference | rx... | merged)\n"
            "  --good-db X         PSNR vs. lossless decode counted as good (default 30)\n"
            "  --gate              exit 1 unless the merged stream passes the M2 gate\n",
            MAX_RX);
}

static bool safe_path(const char *s) {
    return s && !strchr(s, '\'');
}

// ------------------------------------------------------------ encoder (ffmpeg)

static uint8_t *run_encoder(const struct opts *o, int slices, size_t *out_len) {
    char src[1024];
    char label[512] = "";
    if (o->label)
        snprintf(label, sizeof(label),
                 "%sdrawtext=text='%s':fontcolor=white:fontsize=h/7:box=1:boxcolor=black@0.7:"
                 "boxborderw=h/40:x=(w-tw)/2:y=(h-th)/2",
                 o->input ? "," : " -vf \"", o->label);
    if (o->input)
        snprintf(src, sizeof(src), "-i '%s' -vf \"scale=%d:%d,fps=%d%s\"", o->input, o->w, o->h,
                 o->fps, label);
    else
        snprintf(src, sizeof(src), "-f lavfi -i testsrc2=size=%dx%d:rate=%d%s%s", o->w, o->h,
                 o->fps, label, o->label ? "\"" : "");
    int keyint = (int)lround(o->refresh_s * o->fps);
    if (keyint < 2)
        keyint = 2;
    int vbv = o->kbps / o->fps; // one frame
    if (vbv < 1)
        vbv = 1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -hide_banner -loglevel error -nostdin %s -t %.3f -pix_fmt yuv420p "
             "-c:v libx265 -preset ultrafast -tune zerolatency -x265-params "
             "'bitrate=%d:vbv-maxrate=%d:vbv-bufsize=%d:strict-cbr=1:qpmin=%d:"
             "intra-refresh=%d:keyint=%d:min-keyint=%d:scenecut=0:open-gop=0:bframes=0:ref=1:"
             "rc-lookahead=0:slices=%d:aud=1:info=0:repeat-headers=1:log-level=error' "
             "-f hevc -",
             src, o->seconds, o->kbps, o->kbps, vbv, o->qpmin, o->pir ? 1 : 0, keyint, keyint,
             slices);
    fprintf(stderr, "encoder: %s\n", cmd);
    FILE *p = popen(cmd, "r");
    if (!p) {
        perror("popen ffmpeg");
        return NULL;
    }
    size_t cap = 1 << 20, len = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        size_t r = fread(buf + len, 1, cap - len, p);
        if (r == 0)
            break;
        len += r;
    }
    int rc = pclose(p);
    if (rc != 0 || len == 0) {
        fprintf(stderr, "encoder failed (exit %d, %zu bytes)\n", rc, len);
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

struct au_vec {
    const uint8_t **p;
    size_t *len;
    int n, cap;
};

static void push_au(void *ctx, const uint8_t *au, size_t len) {
    struct au_vec *v = ctx;
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1024;
        v->p = realloc(v->p, sizeof(*v->p) * (size_t)v->cap);
        v->len = realloc(v->len, sizeof(*v->len) * (size_t)v->cap);
    }
    v->p[v->n] = au;
    v->len[v->n] = len;
    v->n++;
}

// --------------------------------------------------------------- reassembly

static void on_frame(void *ctx, const uint8_t *au, size_t len, const struct vpkt_frame_info *info) {
    struct stream_out *s = ctx;
    if (info->frame < MAX_FRAMES)
        s->state[info->frame] = info->lost ? FS_LOST : info->complete ? FS_COMPLETE : FS_PARTIAL;
    if (len) {
        ivf_write_frame(s->ivf, info->frame, au, (uint32_t)len);
        s->written++;
    }
}

static int stream_open(struct stream_out *s, const char *dir, const char *name, uint16_t link_id,
                       const struct opts *o, int n_frames) {
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->path, sizeof(s->path), "%s/%s.ivf", dir, name);
    s->ivf = fopen(s->path, "wb");
    if (!s->ivf) {
        perror(s->path);
        return -1;
    }
    ivf_write_header(s->ivf, (uint16_t)o->w, (uint16_t)o->h, (uint32_t)o->fps, (uint32_t)n_frames);
    s->state = calloc((size_t)n_frames, 1);
    s->rx = malloc(sizeof(*s->rx));
    struct vpkt_rx_cfg c = {.link_id = link_id, .max_wait_frames = 2};
    vpkt_rx_init(s->rx, &c, on_frame, s);
    return 0;
}

// ----------------------------------------------------------- picture quality

struct quality {
    bool ok;
    int frames;
    double mean_db;
    int good, identical;
    int startup;     // frames before the first good frame (acquisition, first IDR)
    int longest_bad; // longest bad stretch after the first good frame, in frames
    int bad_runs;    // bad stretches after the first good frame
};

static struct quality measure(const struct opts *o, const char *dir, const char *name,
                              int n_frames) {
    struct quality q = {0};
    char cmd[4096], stats[600];
    snprintf(stats, sizeof(stats), "%s/%s.psnr", dir, name);
    // Both streams to constant frame rate, padded at both ends, so frame i of
    // one lines up with frame i of the other; lost frames repeat the last one.
    const char *chain = "fps=%d:start_time=0,tpad=stop_mode=clone:stop_duration=60,"
                        "trim=end_frame=%d,setpts=N/(%d*TB)";
    char ca[256], cb[256];
    snprintf(ca, sizeof(ca), chain, o->fps, n_frames, o->fps);
    snprintf(cb, sizeof(cb), chain, o->fps, n_frames, o->fps);
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -hide_banner -loglevel quiet -nostdin -copyts -c:v hevc -i '%s/reference.ivf' "
             "-c:v hevc -i '%s/%s.ivf' -lavfi '[0:v]%s[r];[1:v]%s[d];[d][r]psnr=stats_file=%s' "
             "-f null -",
             dir, dir, name, ca, cb, stats);
    if (system(cmd) != 0) {
        fprintf(stderr, "psnr run failed for %s\n", name);
        return q;
    }
    FILE *f = fopen(stats, "r");
    if (!f)
        return q;
    char line[1024];
    int run = 0;
    bool seen_good = false;
    double sum = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "psnr_avg:");
        if (!p)
            continue;
        p += 9;
        double db = strncmp(p, "inf", 3) == 0 ? 100.0 : atof(p);
        if (db > 100)
            db = 100;
        q.frames++;
        sum += db;
        if (db >= 100)
            q.identical++;
        if (db >= o->good_db) {
            q.good++;
            seen_good = true;
            run = 0;
        } else if (!seen_good) {
            q.startup++;
        } else {
            if (run == 0)
                q.bad_runs++;
            run++;
            if (run > q.longest_bad)
                q.longest_bad = run;
        }
    }
    fclose(f);
    q.ok = q.frames > 0;
    q.mean_db = q.frames ? sum / q.frames : 0;
    q.ok = q.ok && seen_good;
    return q;
}

static void write_mp4(const struct opts *o, const char *dir, const char **names, int n) {
    char cmd[8192];
    int len = snprintf(cmd, sizeof(cmd), "ffmpeg -hide_banner -loglevel error -nostdin -y -copyts");
    for (int i = 0; i < n; i++)
        len += snprintf(cmd + len, sizeof(cmd) - (size_t)len, " -c:v hevc -i '%s/%s.ivf'", dir,
                        names[i]);
    len += snprintf(cmd + len, sizeof(cmd) - (size_t)len, " -lavfi '");
    for (int i = 0; i < n; i++)
        len += snprintf(cmd + len, sizeof(cmd) - (size_t)len,
                        "[%d:v]fps=%d:start_time=0,scale=%d:%d[v%d];", i, o->fps,
                        o->w > 640 ? o->w / 2 : o->w, o->w > 640 ? o->h / 2 : o->h, i);
    for (int i = 0; i < n; i++)
        len += snprintf(cmd + len, sizeof(cmd) - (size_t)len, "[v%d]", i);
    len += snprintf(cmd + len, sizeof(cmd) - (size_t)len,
                    "hstack=inputs=%d:shortest=0' -c:v libx264 -preset veryfast -crf 20 "
                    "-pix_fmt yuv420p '%s/compare.mp4'",
                    n, dir);
    if (system(cmd) != 0)
        fprintf(stderr, "mp4 export failed\n");
    else
        printf("wrote %s/compare.mp4 (", dir);
    for (int i = 0; i < n; i++)
        printf("%s%s", names[i], i + 1 < n ? " | " : ")\n");
}

// --------------------------------------------------------------------- main

struct tx_ctx {
    struct airsched *as;
    uint32_t queued;
};

static void enqueue_pkt(void *ctx, const uint8_t *pkt, size_t len) {
    struct tx_ctx *t = ctx;
    if (airsched_enqueue(t->as, pkt, len) == 0)
        t->queued++;
}

int main(int argc, char **argv) {
    struct opts o = {.out_dir = "sim_out",
                     .bind = "open-lr-video",
                     .mode = &OLV_MODES[0],
                     .seconds = 10,
                     .qpmin = 20,
                     .k = 8,
                     .n = 12,
                     .good_db = 30};
    int w = 0, h = 0, fps = 0;
    double refresh = 0;
    enum {
        O_MODE = 256,
        O_INPUT,
        O_LABEL,
        O_SIZE,
        O_FPS,
        O_SEC,
        O_KBPS,
        O_REFRESH,
        O_QPMIN,
        O_K,
        O_N,
        O_SHARD,
        O_CR,
        O_RX,
        O_BIND,
        O_OUT,
        O_MP4,
        O_GATE,
        O_GOOD,
        O_PIR,
        O_MINSHARD,
        O_CHIP
    };
    static const struct option lo[] = {{"mode", 1, 0, O_MODE},
                                       {"input", 1, 0, O_INPUT},
                                       {"label", 1, 0, O_LABEL},
                                       {"size", 1, 0, O_SIZE},
                                       {"fps", 1, 0, O_FPS},
                                       {"seconds", 1, 0, O_SEC},
                                       {"kbps", 1, 0, O_KBPS},
                                       {"refresh", 1, 0, O_REFRESH},
                                       {"qpmin", 1, 0, O_QPMIN},
                                       {"k", 1, 0, O_K},
                                       {"n", 1, 0, O_N},
                                       {"shard", 1, 0, O_SHARD},
                                       {"cr", 1, 0, O_CR},
                                       {"rx", 1, 0, O_RX},
                                       {"bind", 1, 0, O_BIND},
                                       {"out", 1, 0, O_OUT},
                                       {"mp4", 0, 0, O_MP4},
                                       {"gate", 0, 0, O_GATE},
                                       {"good-db", 1, 0, O_GOOD},
                                       {"pir", 0, 0, O_PIR},
                                       {"min-shard", 1, 0, O_MINSHARD},
                                       {"chip", 1, 0, O_CHIP},
                                       {"help", 0, 0, 'h'},
                                       {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case O_MODE:
            if (!(o.mode = olv_mode_find(optarg))) {
                fprintf(stderr, "unknown mode %s\n", optarg);
                return 2;
            }
            break;
        case O_INPUT:
            o.input = optarg;
            break;
        case O_LABEL:
            o.label = optarg;
            break;
        case O_SIZE:
            if (sscanf(optarg, "%dx%d", &w, &h) != 2) {
                usage();
                return 2;
            }
            break;
        case O_FPS:
            fps = atoi(optarg);
            break;
        case O_SEC:
            o.seconds = atof(optarg);
            break;
        case O_KBPS:
            o.kbps = atoi(optarg);
            break;
        case O_REFRESH:
            refresh = atof(optarg);
            break;
        case O_QPMIN:
            o.qpmin = atoi(optarg);
            break;
        case O_K:
            o.k = atoi(optarg);
            break;
        case O_N:
            o.n = atoi(optarg);
            break;
        case O_SHARD:
            o.shard = atoi(optarg);
            break;
        case O_CR:
            o.cr = atoi(optarg);
            break;
        case O_RX:
            if (o.n_rx == MAX_RX) {
                fprintf(stderr, "at most %d receivers\n", MAX_RX);
                return 2;
            }
            o.rx_spec[o.n_rx++] = optarg;
            break;
        case O_BIND:
            o.bind = optarg;
            break;
        case O_OUT:
            o.out_dir = optarg;
            break;
        case O_MP4:
            o.mp4 = true;
            break;
        case O_GATE:
            o.gate = true;
            break;
        case O_PIR:
            o.pir = true;
            break;
        case O_MINSHARD:
            o.min_shard = atoi(optarg);
            break;
        case O_CHIP: {
            int chip = radio_chip_parse(optarg);
            if (chip < 0) {
                fprintf(stderr, "unknown chip %s\n", optarg);
                return 2;
            }
            const struct radio_caps *rc = radio_chip_caps((enum radio_chip)chip);
            if (!olv_mode_supported(o.mode, rc)) {
                fprintf(stderr, "the %s cannot run %s (give --mode first)\n", rc->name,
                        o.mode->name);
                return 2;
            }
            o.max_packet = rc->max_packet;
            break;
        }
        case O_GOOD:
            o.good_db = atof(optarg);
            break;
        default:
            usage();
            return c == 'h' ? 0 : 2;
        }
    }
    o.w = w ? w : o.mode->w;
    o.h = h ? h : o.mode->h;
    o.fps = fps ? fps : o.mode->fps;
    o.refresh_s = refresh > 0 ? refresh : o.mode->refresh_s;
    if (!o.cr)
        o.cr = o.mode->cr;
    if (o.n_rx == 0) {
        o.rx_spec[0] = "loss=0.2,seed=1";
        o.rx_spec[1] = "loss=0.2,seed=2";
        o.n_rx = 2;
    }
    if (!safe_path(o.out_dir) || (o.input && !safe_path(o.input)) ||
        (o.label && (!safe_path(o.label) || strpbrk(o.label, ":\\%\"$`")))) {
        fprintf(stderr, "paths must not contain single quotes; labels no ' : \\ %% \" $ or `\n");
        return 2;
    }
    if (o.k < 1 || o.n < o.k || o.n > FEC_MAX_N || o.cr < 2 || o.cr > 5 || o.fps < 1 ||
        o.seconds * o.fps >= MAX_FRAMES) {
        fprintf(stderr, "invalid FEC, coding rate, fps or length\n");
        return 2;
    }
    if (mkdir(o.out_dir, 0755) != 0 && errno != EEXIST) {
        perror(o.out_dir);
        return 1;
    }

    // ---- link budget
    uint32_t seed = fhss_seed(o.bind);
    uint16_t link_id = ota_link_id(o.bind);
    struct airsched_cfg sc = {.phy = flrc_phy_default(o.mode->bitrate_bps, (uint8_t)o.cr),
                              .hop_us = 10000,
                              .retune_us = 200,
                              .beacon_every = 1,
                              .beacons_per_hop = o.mode->beacons_per_hop,
                              .seed = seed,
                              .n_channels = o.mode->band.n,
                              .max_packet = (uint16_t)o.max_packet,
                              .link_id = link_id,
                              .mode = (uint8_t)o.mode->mode,
                              .fec_k = (uint8_t)o.k,
                              .fec_n = (uint8_t)o.n,
                              .tx_power = 0,
                              .queue_max = AIRSCHED_QMAX};
    unsigned per_hop = 0;
    uint32_t shard_bps = 0;
    size_t best_pkt = airsched_best_packet(&sc, &per_hop, &shard_bps);
    if (best_pkt == 0) {
        fprintf(stderr, "no packet fits in a hop with these settings\n");
        return 1;
    }
    int shard = o.shard ? o.shard : (int)(best_pkt - OTA_HDR_LEN);
    int max_shard = (o.max_packet ? o.max_packet : OTA_MAX_PACKET) - OTA_HDR_LEN;
    if (shard < 1 || shard > max_shard) {
        fprintf(stderr, "shard must be 1..%d\n", max_shard);
        return 2;
    }
    double video_cap_kbps = shard_bps / 1000.0 * o.k / o.n;
    if (!o.kbps)
        o.kbps = (int)(video_cap_kbps * 0.85);
    struct vpkt_tx_cfg tc = {.link_id = link_id,
                             .mode = (uint8_t)o.mode->mode,
                             .k = (uint8_t)o.k,
                             .n = (uint8_t)o.n,
                             .max_shard = (uint16_t)shard,
                             .min_shard = (uint16_t)(o.min_shard ? o.min_shard : shard)};
    size_t block_cap = vpkt_block_capacity(&tc);
    double frame_bytes = o.kbps * 1000.0 / 8 / o.fps;
    int slices = (int)ceil(frame_bytes / (0.8 * (double)block_cap));
    if (slices < 1)
        slices = 1;

    printf("== link: %s, FLRC %.2f Mbit/s CR %s, %u channels, 100 Hz hops\n", o.mode->name,
           o.mode->bitrate_bps / 1e6,
           o.cr == 2   ? "1/2"
           : o.cr == 5 ? "2/3"
           : o.cr == 3 ? "3/4"
                       : "1",
           o.mode->band.n);
    printf("   packet %zu B (shard %d B) x %u per hop -> %.0f kbit/s of shards, %.0f kbit/s "
           "video at RS %d/%d\n",
           (size_t)shard + OTA_HDR_LEN, shard, per_hop, shard_bps / 1000.0, video_cap_kbps, o.k,
           o.n);
    printf("   video %dx%d@%d, CBR %d kbit/s (%.0f B/frame), %d slice(s)/frame, block %zu B, "
           "%s %.2f s\n",
           o.w, o.h, o.fps, o.kbps, frame_bytes, slices, block_cap,
           o.pir ? "intra refresh" : "IDR every", o.refresh_s);
    if (o.kbps > video_cap_kbps)
        printf("   WARNING: CBR target exceeds the link's video capacity; the queue will grow\n");

    // ---- encode and prepare access units
    size_t es_len = 0;
    uint8_t *es = run_encoder(&o, slices, &es_len);
    if (!es)
        return 1;
    struct au_vec aus = {0};
    h265_split_aus(es, es_len, push_au, &aus);
    int n_frames = aus.n;
    printf("== encoded %d frames, %zu bytes (%.0f kbit/s)\n", n_frames, es_len,
           es_len * 8.0 / 1000 / (n_frames / (double)o.fps));

    // ---- outputs
    struct stream_out ref = {0}, merged = {0};
    snprintf(ref.path, sizeof(ref.path), "%s/reference.ivf", o.out_dir);
    ref.ivf = fopen(ref.path, "wb");
    if (!ref.ivf) {
        perror(ref.path);
        return 1;
    }
    ivf_write_header(ref.ivf, (uint16_t)o.w, (uint16_t)o.h, (uint32_t)o.fps, (uint32_t)n_frames);
    if (stream_open(&merged, o.out_dir, "merged", link_id, &o, n_frames))
        return 1;
    static struct receiver rxs[MAX_RX];
    for (int r = 0; r < o.n_rx; r++) {
        struct receiver *R = &rxs[r];
        if (channel_parse(&R->ch, o.rx_spec[r]))
            return 2;
        if (!R->ch.seed) {
            R->ch.seed = 1000u + (uint32_t)r;
            channel_reset(&R->ch);
        }
        char name[16], lp[700];
        snprintf(name, sizeof(name), "rx%d", r + 1);
        if (stream_open(&R->out, o.out_dir, name, link_id, &o, n_frames))
            return 1;
        snprintf(lp, sizeof(lp), "%s/%s.vrxlog", o.out_dir, name);
        R->log = fopen(lp, "wb");
        if (!R->log) {
            perror(lp);
            return 1;
        }
        vrxlog_write_header(R->log);
        struct fhss_rx_cfg fc = fhss_rx_default_cfg(o.mode->band.n, sc.hop_us);
        R->clock_off = 0x10000000u * (uint32_t)r + 12345u;
        fhss_rx_init(&R->sync, seed, &fc, R->clock_off);
        R->sync.park_ch = (uint8_t)((r * 3) % o.mode->band.n);
        R->first_lock_s = -1;
    }

    // ---- virtual-time run
    static struct airsched as;
    airsched_init(&as, &sc, 0);
    struct vpkt_tx tx;
    if (vpkt_tx_init(&tx, &tc)) {
        fprintf(stderr, "bad packetizer config\n");
        return 1;
    }
    struct tx_ctx tctx = {.as = &as};
    uint32_t *q_last = calloc((size_t)n_frames, sizeof(uint32_t));
    double *latency_ms = calloc((size_t)n_frames, sizeof(double));
    struct h265_ps_cache ps = {0};
    static uint8_t prep[VPKT_MAX_FRAME];
    uint32_t sent_video = 0;
    int fi = 0, frames_done = 0;
    uint64_t t = 0;
    const uint64_t frame_us = 1000000u / (uint64_t)o.fps;
    const uint64_t t_end = (uint64_t)n_frames * frame_us + 2000000u;
    uint32_t too_big = 0;

    while ((fi < n_frames || airsched_queued(&as)) && t < t_end) {
        uint64_t t_frame = (uint64_t)fi * 1000000u / (uint64_t)o.fps;
        if (fi < n_frames && t_frame <= t) {
            bool has_ps;
            bool repeat = fi % o.fps == 0; // parameter sets once per second
            size_t plen =
                h265_prepare_au(&ps, aus.p[fi], aus.len[fi], repeat, prep, sizeof(prep), &has_ps);
            if (plen) {
                ivf_write_frame(ref.ivf, (uint64_t)fi, prep, (uint32_t)plen);
                if (vpkt_tx_frame(&tx, prep, plen, enqueue_pkt, &tctx) < 0)
                    too_big++;
            }
            q_last[fi] = tctx.queued;
            fi++;
            continue;
        }
        struct airsched_action a;
        airsched_next(&as, t, &a);
        if (!a.send) {
            uint64_t next = a.wake_us;
            if (fi < n_frames && t_frame < next)
                next = t_frame;
            t = next > t ? next : t + 1;
            continue;
        }
        uint64_t t_done = t + a.airtime_us;
        bool beacon = (a.out[5] & OTA_FLAG_TYPE_MASK) == OTA_TYPE_BEACON;
        if (!beacon) {
            sent_video++;
            while (frames_done < fi && q_last[frames_done] <= sent_video) {
                uint64_t cap_t = (uint64_t)frames_done * 1000000u / (uint64_t)o.fps;
                latency_ms[frames_done] = (double)(t_done - cap_t) / 1000.0;
                frames_done++;
            }
        }
        for (int r = 0; r < o.n_rx; r++) {
            struct receiver *R = &rxs[r];
            uint32_t now_rx = (uint32_t)(t_done + R->clock_off);
            uint8_t rch = fhss_rx_poll(&R->sync, now_rx, NULL);
            R->ch_sent[a.channel]++;
            if (rch != a.channel) {
                R->off_channel++;
                continue;
            }
            if (channel_lost(&R->ch, a.channel, t_done / 1e6)) {
                R->lost++;
                continue;
            }
            R->heard++;
            R->ch_ok[a.channel]++;
            struct ota_hdr hd;
            if (ota_hdr_unpack(a.out, a.len, link_id, &hd) != OTA_OK)
                continue;
            if (beacon) {
                struct ota_beacon b;
                if (ota_beacon_unpack(a.out + OTA_HDR_LEN, a.len - OTA_HDR_LEN, &b) == OTA_OK) {
                    uint32_t hop_start = now_rx - a.airtime_us - b.offset_us;
                    fhss_rx_on_beacon(&R->sync, b.cycle, hd.hop, hop_start);
                    if (R->first_lock_s < 0 && R->sync.state == FHSS_RX_LOCKED)
                        R->first_lock_s = t_done / 1e6;
                }
            } else {
                fhss_rx_on_packet(&R->sync, hd.hop);
                vpkt_rx_push(R->out.rx, a.out, a.len, now_rx);
                vpkt_rx_push(merged.rx, a.out, a.len, (uint32_t)t_done);
            }
            struct vrx_rx_packet rp = {
                .ota_len = (uint16_t)a.len, .rssi = -90, .snr = 20, .hop = hd.hop, .t_us = now_rx};
            memcpy(rp.ota, a.out, a.len);
            uint8_t rec[OTA_MAX_PACKET + VRX_RX_TRAILER];
            size_t rl = vrx_rx_packet_pack(&rp, rec, sizeof(rec));
            vrxlog_write(R->log, t_done, rec, (uint16_t)rl);
        }
        t = t_done + sc.phy.tx_gap_us;
    }
    vpkt_rx_flush(merged.rx);
    fclose(ref.ivf);
    fclose(merged.ivf);
    for (int r = 0; r < o.n_rx; r++) {
        vpkt_rx_flush(rxs[r].out.rx);
        fclose(rxs[r].out.ivf);
        fclose(rxs[r].log);
    }

    // ---- link report
    double lat_sum = 0, lat_max = 0;
    for (int i = 0; i < frames_done; i++) {
        lat_sum += latency_ms[i];
        if (latency_ms[i] > lat_max)
            lat_max = latency_ms[i];
    }
    printf("== air: video sent %.0f kbit/s (after stripping filler/SEI), %u blocks\n",
           tx.bytes_in * 8.0 / 1000 / (n_frames / (double)o.fps), tx.blocks);
    printf("   %u video packets, %u beacons, %u queue drops, %u oversize frames, airtime "
           "%.0f%%\n",
           as.st.sent, as.st.beacons, as.st.dropped, too_big,
           100.0 * (double)as.st.busy_us / (double)(t ? t : 1));
    printf("   capture -> last packet on air: mean %.1f ms, max %.1f ms (%d frames)\n",
           frames_done ? lat_sum / frames_done : 0, lat_max, frames_done);
    for (int r = 0; r < o.n_rx; r++) {
        struct receiver *R = &rxs[r];
        uint32_t total = R->heard + R->lost + R->off_channel;
        printf("== rx%d [%s]: heard %u/%u (%.1f%%), channel loss %u, off channel %u, first lock "
               "%.0f ms, locks %u\n",
               r + 1, o.rx_spec[r], R->heard, total, total ? 100.0 * R->heard / total : 0, R->lost,
               R->off_channel, R->first_lock_s * 1000, R->sync.locks);
        printf("   PER by channel:");
        for (int ch = 0; ch < o.mode->band.n; ch++)
            printf(" %d:%.0f%%", ch,
                   R->ch_sent[ch] ? 100.0 * (1.0 - (double)R->ch_ok[ch] / R->ch_sent[ch]) : 0.0);
        printf("\n");
    }

    // ---- picture report
    const char *names[MAX_RX + 2];
    struct stream_out *streams[MAX_RX + 1];
    int ns = 0;
    for (int r = 0; r < o.n_rx; r++)
        streams[ns++] = &rxs[r].out;
    streams[ns++] = &merged;
    printf("== picture (PSNR vs. lossless decode, good >= %.0f dB)\n", o.good_db);
    printf("   %-7s %8s %8s %8s %8s %7s %8s %9s %10s\n", "stream", "complete", "partial", "lost",
           "good", "mean dB", "startup", "bad runs", "worst run");
    struct quality mq = {0};
    for (int i = 0; i < ns; i++) {
        struct stream_out *s = streams[i];
        int cnt[4] = {0};
        for (int f = 0; f < n_frames; f++)
            cnt[s->state[f]]++;
        struct quality q = measure(&o, o.out_dir, s->name, n_frames);
        if (s == &merged)
            mq = q;
        printf("   %-7s %7.1f%% %7.1f%% %7.1f%% %7.1f%% %7.1f %6.2f s %9d %8.2f s\n", s->name,
               100.0 * cnt[FS_COMPLETE] / n_frames, 100.0 * cnt[FS_PARTIAL] / n_frames,
               100.0 * (cnt[FS_LOST] + cnt[FS_NONE]) / n_frames,
               q.frames ? 100.0 * q.good / q.frames : 0, q.mean_db, (double)q.startup / o.fps,
               q.bad_runs, (double)q.longest_bad / o.fps);
    }

    if (o.mp4) {
        int nn = 0;
        names[nn++] = "reference";
        for (int r = 0; r < o.n_rx; r++)
            names[nn++] = rxs[r].out.name;
        names[nn++] = "merged";
        write_mp4(&o, o.out_dir, names, nn);
    }

    // M2 gate: once the merged picture is up, it always comes back: no bad
    // stretch lasts longer than two refresh periods. A stretch still open at
    // the end of the clip counts with the length it had. Startup (acquisition
    // plus waiting for the first IDR) is reported, not gated.
    double worst_s = (double)mq.longest_bad / o.fps;
    bool pass = mq.ok && worst_s <= 2 * o.refresh_s;
    printf("== M2 gate (merged picture up, then no bad stretch > %.2f s): %s\n", 2 * o.refresh_s,
           pass ? "PASS" : "FAIL");

    vpkt_tx_free(&tx);
    fec_free_cache();
    for (int i = 0; i < ns; i++) {
        free(streams[i]->rx);
        free(streams[i]->state);
    }
    free(q_last);
    free(latency_ms);
    free(aus.p);
    free(aus.len);
    free(es);
    return (o.gate && !pass) ? 1 : 0;
}
