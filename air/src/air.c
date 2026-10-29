// SPDX-License-Identifier: GPL-3.0-or-later
// Air unit: video source -> H.265 access units -> packetizer + FEC -> air
// scheduler (100 Hz hop clock, beacons) -> radio.
//
// Video sources (air/src/vsrc.c): an IVF file of H.265 access units (e.g. a
// test card or sim/loopback's reference.ivf), a raw Annex-B stream on stdin
// (e.g. ffmpeg ... -c:v libx265 -x265-params aud=1:... -f hevc -), or
// "camera": olv-cam (air/src/olv_cam.c, Rockchip MPI) as a child process,
// sized and rate-controlled for this link, with a fallback clip (the NO
// CAMERA test card) whenever the camera is not delivering.
#define _GNU_SOURCE // clock_nanosleep, ppoll, sched_setscheduler, mlockall

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
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
#include "vsrc.h"

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

// Sleeps until t or until fd is readable, whichever is first.
static void wait_fd_until_us(int fd, uint64_t t) {
    uint64_t now = mono_us();
    if (t <= now)
        return;
    struct timespec ts = {(time_t)((t - now) / 1000000u), (long)((t - now) % 1000000u) * 1000};
    struct pollfd p = {.fd = fd, .events = POLLIN};
    ppoll(&p, 1, &ts, NULL);
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
            "usage: air --source FILE.ivf|-|camera [options]\n"
            "  --mode lr520|lr260|hq2600|hq1300   (default lr520; hq1300 on a 2.4 GHz-only chip)\n"
            "  --bind PHRASE        (default open-lr-video)\n"
            "  --fps N              frame rate of the source (default: mode)\n"
            "  --loop               loop an IVF source\n"
            "  --k N --n N          FEC shape (default 8/12)\n"
            "  --shard N            shard bytes (default: best fit per hop)\n"
            "  --max-packet N       packet size limit (default: the chip's, 255 LR2021 / 127\n"
            "                       SX1280)\n"
            "  --cr N               FLRC coding rate (2=1/2 3=3/4 4=1 5=2/3; default: mode)\n"
            "  --radio-conf PATH    wiring (default " RADIO_CONF_PATH ")\n"
            "  --power DBM          chip output (default from radio conf, -9)\n"
            "  --outdoor            allow > 0 dBm\n"
            "  --rt                 SCHED_FIFO + mlockall\n"
            "  --seconds S          stop after S seconds\n"
            "camera source (--source camera):\n"
            "  --camera-cmd PATH    camera program (default olv-cam); it gets --size, --fps,\n"
            "                       --kbps, --refresh and --slice-bytes and writes IVF to stdout\n"
            "  --camera-args ARGS   extra arguments for it, space-separated (e.g. \"--flip\")\n"
            "  --size WxH           picture (default: mode)\n"
            "  --kbps N             CBR target (default: 85%% of the link's video capacity)\n"
            "  --refresh S          intra refresh sweep period (default: mode)\n"
            "  --fallback FILE.ivf  looped while the camera is not delivering (NO CAMERA card)\n"
            "  --camera-retry S     restart a failed camera after S seconds (default 5)\n");
}

int main(int argc, char **argv) {
    const char *src_path = NULL, *bind = "open-lr-video", *conf = RADIO_CONF_PATH;
    const struct olv_mode *mode = NULL;
    int fps = 0, k = 8, n = 12, shard = 0, cr = 0, power = 1000, max_packet = 0;
    const char *cam_cmd = "olv-cam", *cam_args = "", *fallback = NULL;
    int w = 0, h = 0, kbps = 0;
    bool loop = false, outdoor = false, rt = false;
    double seconds = 0, refresh = 0, cam_retry = 5;
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
        O_MAXPKT,
        O_CAMCMD,
        O_CAMARGS,
        O_SIZE,
        O_KBPS,
        O_REFRESH,
        O_FALLBACK,
        O_RETRY
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
                                       {"camera-cmd", 1, 0, O_CAMCMD},
                                       {"camera-args", 1, 0, O_CAMARGS},
                                       {"size", 1, 0, O_SIZE},
                                       {"kbps", 1, 0, O_KBPS},
                                       {"refresh", 1, 0, O_REFRESH},
                                       {"fallback", 1, 0, O_FALLBACK},
                                       {"camera-retry", 1, 0, O_RETRY},
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
        case O_CAMCMD:
            cam_cmd = optarg;
            break;
        case O_CAMARGS:
            cam_args = optarg;
            break;
        case O_SIZE:
            if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "--size wants WxH\n");
                return 2;
            }
            break;
        case O_KBPS:
            kbps = atoi(optarg);
            break;
        case O_REFRESH:
            refresh = atof(optarg);
            break;
        case O_FALLBACK:
            fallback = optarg;
            break;
        case O_RETRY:
            cam_retry = atof(optarg);
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
    bool camera = !strcmp(src_path, "camera");
    struct vsrc_file file;
    bool have_file = false;
    if (camera) {
        if (fallback && !(have_file = vsrc_file_open(&file, fallback, true) == 0))
            fprintf(stderr, "air: fallback %s unusable; no video while the camera is down\n",
                    fallback);
    } else {
        if (vsrc_file_open(&file, src_path, loop))
            return 1;
        have_file = true;
    }
    // Camera: picture, rate control and slices sized for this link. One
    // slice per FEC block, so a lost block costs one slice, not a frame.
    static struct vsrc_cam cam = {.fd = -1};
    char cam_size[32], cam_fps[16], cam_kbps[16], cam_refresh[16], cam_slice[16];
    static char cam_extra[512];
    char *cam_argv[48];
    if (camera) {
        double cap_kbps = shard_bps / 1000.0 * k / n;
        snprintf(cam_size, sizeof(cam_size), "%dx%d", w ? w : mode->w, h ? h : mode->h);
        snprintf(cam_fps, sizeof(cam_fps), "%d", fps);
        snprintf(cam_kbps, sizeof(cam_kbps), "%d", kbps ? kbps : (int)(cap_kbps * 0.85));
        snprintf(cam_refresh, sizeof(cam_refresh), "%.2f", refresh > 0 ? refresh : mode->refresh_s);
        snprintf(cam_slice, sizeof(cam_slice), "%zu", vpkt_block_capacity(&tc));
        size_t na = 0;
        cam_argv[na++] = (char *)cam_cmd;
        const char *fixed[] = {"--size",        cam_size,  "--fps",     cam_fps,
                               "--kbps",        cam_kbps,  "--refresh", cam_refresh,
                               "--slice-bytes", cam_slice, "--out",     "-"};
        for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
            cam_argv[na++] = (char *)fixed[i];
        snprintf(cam_extra, sizeof(cam_extra), "%s", cam_args);
        for (char *t = strtok(cam_extra, " \t"); t && na < 47; t = strtok(NULL, " \t"))
            cam_argv[na++] = t;
        cam_argv[na] = NULL;
        if (kbps > cap_kbps)
            fprintf(stderr, "air: WARNING: --kbps %d exceeds the link's video capacity (%.0f)\n",
                    kbps, cap_kbps);
        printf("air: camera command:");
        for (size_t i = 0; i < na; i++)
            printf(" %s", cam_argv[i]);
        printf("\n");
    }

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
    uint64_t retry_us = (uint64_t)(cam_retry * 1e6), cam_retry_at = t0;
    uint32_t frames = 0, overruns = 0, tx_errors = 0, late_frames = 0, cam_frames = 0;
    bool eof = false, cam_live = false; // cam_live: the camera is delivering
    int cfg_fps = fps;
    uint8_t cur_ch = 0xFF;

    while (!stop && !(eof && airsched_queued(&as) == 0)) {
        uint64_t now = mono_us();
        if (seconds > 0 && now - t0 >= (uint64_t)(seconds * 1e6))
            break;
        size_t len = 0;
        if (camera && cam.fd < 0 && now >= cam_retry_at) {
            cam_retry_at = now + retry_us;
            if (vsrc_cam_start(&cam, cam_argv, sizeof(au)) == 0)
                printf("air: camera: started %s (pid %d)\n", cam_cmd, (int)cam.pid);
        }
        if (cam.fd >= 0) {
            long got_len = vsrc_cam_poll(&cam, au, sizeof(au));
            if (got_len > 0) {
                len = (size_t)got_len;
                if (!cam_live) {
                    cam_live = true;
                    if (cam.fps > 0)
                        fps = cam.fps;
                    printf("air: camera: %dx%d@%d, sending camera video\n", cam.w, cam.h, cam.fps);
                }
                cam_frames++;
            } else if (got_len < 0) {
                uint32_t got = cam.frames;
                int st = vsrc_cam_stop(&cam);
                char why[32];
                if (st >= 0 && WIFEXITED(st))
                    snprintf(why, sizeof(why), "exit %d", WEXITSTATUS(st));
                else if (st >= 0 && WIFSIGNALED(st))
                    snprintf(why, sizeof(why), "signal %d", WTERMSIG(st));
                else
                    snprintf(why, sizeof(why), "gone");
                printf("air: camera: stopped (%s) after %u frames; %s, retry in %.0f s\n", why, got,
                       have_file ? "sending the fallback" : "no video", cam_retry);
                cam_live = false;
                fps = cfg_fps;
                frame_us = 1000000u / (uint64_t)fps;
                next_frame = now;
                cam_retry_at = now + retry_us;
                if (have_file)
                    vsrc_file_rewind(&file); // the clip starts with an IDR
            }
        }
        if (!len && have_file && !cam_live && !eof && now >= next_frame) {
            len = vsrc_file_next(&file, au, sizeof(au));
            if (len == 0 && !camera)
                eof = true;
            next_frame += frame_us;
            if (now > next_frame + frame_us) { // fell behind: do not try to catch up
                late_frames++;
                next_frame = now + frame_us;
            }
            if (len == 0)
                continue;
        }
        if (len) {
            bool has_ps;
            size_t plen = h265_prepare_au(&ps, au, len, frames % (uint32_t)fps == 0, prep,
                                          sizeof(prep), &has_ps);
            if (plen)
                vpkt_tx_frame(&tx, prep, plen, enqueue, &ectx);
            frames++;
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
            if (have_file && !cam_live && !eof && next_frame < wake)
                wake = next_frame;
            if (camera && cam.fd < 0 && cam_retry_at < wake)
                wake = cam_retry_at;
            if (cam.fd >= 0)
                wait_fd_until_us(cam.fd, wake); // a camera frame wakes us early
            else
                sleep_until_us(wake);
        }
        if (now >= next_report) {
            printf("air: %u frames (%u camera), %u pkts, %u beacons, queue %u, drops %u, overruns "
                   "%u, late frames %u, tx errors %u\n",
                   frames, cam_frames, as.st.sent, as.st.beacons, airsched_queued(&as),
                   as.st.dropped, overruns, late_frames, tx_errors);
            fflush(stdout);
            next_report += 1000000u;
        }
    }
    printf("air: done, %u frames, %u packets, %u beacons, %u drops, %u overruns\n", frames,
           as.st.sent, as.st.beacons, as.st.dropped, overruns);
    vsrc_cam_stop(&cam);
    free(cam.buf);
    radio_close(r);
    vpkt_tx_free(&tx);
    fec_free_cache();
    if (have_file)
        vsrc_file_close(&file);
    return 0;
}
