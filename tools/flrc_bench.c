// SPDX-License-Identifier: GPL-3.0-or-later
// M0 bench tool: FLRC on a fixed channel between two Luckfox + LR2021 boards.
//
//   flrc_bench tx     send numbered packets
//   flrc_bench rx     receive, log CSV per packet, PER and RSSI per second
//   flrc_bench ping   send, wait for the echo, report round-trip time
//   flrc_bench pong   echo every good packet straight back
//   flrc_bench retune time retune and calibration (no RF needed)
//
// RF safety: the NiceRF module has an external PA, so the module output is
// well above the LR2021 setting. Bench runs use the lowest power, with 30-40
// dB of attenuation between the modules and never without antenna or load.
// --power above 0 dBm is refused unless --outdoor is given.
#define _POSIX_C_SOURCE 200809L

#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "airtime.h"
#include "fhss.h"
#include "olv_util.h"
#include "radio.h"

#define MAGIC 0x424C564Fu // "OLVB"

static volatile sig_atomic_t stop;

static void on_signal(int s) {
    (void)s;
    stop = 1;
}

struct opts {
    const char *mode;
    struct radio_pins pins;
    struct radio_flrc flrc;
    unsigned count, len, interval_ms, timeout_ms;
    bool outdoor;
    const char *csv;
};

static void usage(void) {
    fprintf(stderr, "usage: flrc_bench MODE --busy N --irq N --nreset N [options]\n"
                    "  MODE                tx | rx | ping | pong | retune\n"
                    "  --spidev PATH       default /dev/spidev0.0\n"
                    "  --spi-hz N          default 8000000\n"
                    "  --busy/--irq/--nreset N   GPIO numbers (bank*32+line, see docs/wiring.md)\n"
                    "  --rf-pins A,B,C     module PA/LNA control GPIOs (-1 = none)\n"
                    "  --rf-table S,R,T,RH,TH   pin levels per mode as bitmasks (stby, rx, tx,\n"
                    "                      rx 2.4G, tx 2.4G), from the module datasheet\n"
                    "  --tcxo-mv N         TCXO voltage on DIO3, 0 for a crystal (default 0)\n"
                    "  --freq HZ           default 868000000\n"
                    "  --bitrate BPS       default 520000 (2600000 ... 260000)\n"
                    "  --cr N              2=1/2 3=3/4 4=uncoded 5=2/3 (default 3)\n"
                    "  --power DBM         LR2021 output (default -9); > 0 needs --outdoor\n"
                    "  --outdoor           allow > 0 dBm (antennas fitted, outdoors only)\n"
                    "  --len N             packet length incl. 12-byte bench header (default 255)\n"
                    "  --count N           packets to send (default 1000, 0 = until Ctrl-C)\n"
                    "  --interval-ms N     gap between packets (default 10)\n"
                    "  --timeout-ms N      ping: wait for echo (default 100)\n"
                    "  --csv FILE          rx/ping: per-packet log\n");
}

static uint32_t now_us(void) {
    return radio_now_us();
}

static void sleep_us(uint32_t us) {
    struct timespec ts = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000};
    nanosleep(&ts, NULL);
}

static size_t make_packet(uint8_t *b, size_t len, uint32_t seq) {
    olv_put_u32(b, MAGIC);
    olv_put_u32(b + 4, seq);
    olv_put_u32(b + 8, now_us());
    uint32_t st = seq | 1;
    for (size_t i = 12; i < len; i++)
        b[i] = (uint8_t)olv_xorshift32(&st);
    return len;
}

static bool check_packet(const uint8_t *b, size_t len, uint32_t *seq) {
    if (len < 12 || olv_get_u32(b) != MAGIC)
        return false;
    *seq = olv_get_u32(b + 4);
    uint32_t st = *seq | 1;
    for (size_t i = 12; i < len; i++)
        if (b[i] != (uint8_t)olv_xorshift32(&st))
            return false;
    return true;
}

// Waits for TX_DONE; returns microseconds from start to done or -1.
static long wait_tx(struct radio *r, uint32_t t0) {
    uint8_t buf[256];
    size_t n;
    struct radio_rx_info info;
    for (int i = 0; i < 100; i++) {
        enum radio_event ev = radio_poll(r, 50000, buf, sizeof(buf), &n, &info);
        if (ev == RADIO_EV_TX_DONE)
            return (long)(info.t_us - t0);
        if (ev == RADIO_EV_ERROR)
            return -1;
    }
    return -1;
}

static int run_tx(struct radio *r, const struct opts *o) {
    uint8_t b[256];
    uint32_t sent = 0, fails = 0;
    long tx_sum = 0, tx_max = 0;
    for (uint32_t seq = 0; !stop && (o->count == 0 || seq < o->count); seq++) {
        size_t n = make_packet(b, o->len, seq);
        uint32_t t0 = now_us();
        if (radio_tx(r, b, n)) {
            fails++;
            continue;
        }
        long d = wait_tx(r, t0);
        if (d < 0) {
            fails++;
            continue;
        }
        sent++;
        tx_sum += d;
        if (d > tx_max)
            tx_max = d;
        if (o->interval_ms)
            sleep_us(o->interval_ms * 1000u);
    }
    struct flrc_phy phy = flrc_phy_default(o->flrc.bitrate_bps, o->flrc.cr);
    printf(
        "tx: sent %u, failed %u; start-to-TX-done mean %ld us, max %ld us (model airtime %u us)\n",
        sent, fails, sent ? tx_sum / (long)sent : 0, tx_max, flrc_airtime_us(&phy, o->len));
    return fails ? 1 : 0;
}

static int run_rx(struct radio *r, const struct opts *o) {
    FILE *csv = o->csv ? fopen(o->csv, "w") : NULL;
    if (csv)
        fprintf(csv, "t_us,seq,len,crc_ok,payload_ok,rssi_dbm,snr_db\n");
    if (radio_rx(r))
        return 1;
    uint32_t good = 0, crc = 0, corrupt = 0, first = UINT32_MAX, last = 0;
    uint32_t win_good = 0, win_crc = 0, win_first = UINT32_MAX, win_last = 0;
    double rssi_sum = 0, snr_sum = 0;
    uint32_t next_report = now_us() + 1000000u;
    uint8_t b[256];
    while (!stop) {
        size_t n = 0;
        struct radio_rx_info info;
        enum radio_event ev = radio_poll(r, 100000, b, sizeof(b), &n, &info);
        if (ev == RADIO_EV_RX_DONE) {
            uint32_t seq = 0;
            bool ok = info.crc_ok && check_packet(b, n, &seq);
            if (!info.crc_ok) {
                crc++;
                win_crc++;
            } else if (!ok) {
                corrupt++; // CRC passed but payload wrong: should never happen
            } else {
                good++;
                win_good++;
                rssi_sum += info.rssi_dbm;
                snr_sum += info.snr_q4 / 4.0;
                if (first == UINT32_MAX)
                    first = seq;
                if (win_first == UINT32_MAX)
                    win_first = seq;
                last = seq;
                win_last = seq;
            }
            if (csv)
                fprintf(csv, "%u,%u,%zu,%d,%d,%d,%.2f\n", info.t_us, ok ? seq : 0, n, info.crc_ok,
                        ok, info.rssi_dbm, info.snr_q4 / 4.0);
        } else if (ev == RADIO_EV_ERROR) {
            fprintf(stderr, "rx: radio error, restarting RX\n");
            radio_rx(r);
        }
        if ((int32_t)(now_us() - next_report) >= 0) {
            uint32_t expect = win_first == UINT32_MAX ? 0 : win_last - win_first + 1;
            printf("rx: %u good, %u crc err, PER %.1f%% (window), RSSI %.1f dBm, SNR %.1f dB\n",
                   win_good, win_crc, expect ? 100.0 * (1.0 - (double)win_good / expect) : 100.0,
                   good ? rssi_sum / good : 0, good ? snr_sum / good : 0);
            fflush(stdout);
            win_good = win_crc = 0;
            win_first = UINT32_MAX;
            next_report += 1000000u;
        }
    }
    uint32_t expect = first == UINT32_MAX ? 0 : last - first + 1;
    printf("rx total: %u good of %u sent (seq %u..%u), %u crc errors, %u corrupt, PER %.2f%%, "
           "RSSI %.1f dBm, SNR %.1f dB\n",
           good, expect, first, last, crc, corrupt,
           expect ? 100.0 * (1.0 - (double)good / expect) : 100.0, good ? rssi_sum / good : 0,
           good ? snr_sum / good : 0);
    if (csv)
        fclose(csv);
    return 0;
}

static int run_ping(struct radio *r, const struct opts *o) {
    FILE *csv = o->csv ? fopen(o->csv, "w") : NULL;
    if (csv)
        fprintf(csv, "seq,rtt_us,rssi_dbm,snr_db\n");
    uint8_t b[256], rb[256];
    uint32_t ok = 0, lost = 0;
    long rtt_sum = 0, rtt_min = 1L << 30, rtt_max = 0;
    for (uint32_t seq = 0; !stop && (o->count == 0 || seq < o->count); seq++) {
        size_t n = make_packet(b, o->len, seq);
        uint32_t t0 = now_us();
        if (radio_tx(r, b, n) || wait_tx(r, t0) < 0 || radio_rx(r)) {
            lost++;
            continue;
        }
        bool got = false;
        struct radio_rx_info info;
        while (!got && (int32_t)(now_us() - t0) < (int32_t)(o->timeout_ms * 1000u)) {
            size_t rn = 0;
            if (radio_poll(r, 5000, rb, sizeof(rb), &rn, &info) != RADIO_EV_RX_DONE)
                continue;
            uint32_t rseq;
            if (info.crc_ok && check_packet(rb, rn, &rseq) && rseq == seq)
                got = true;
        }
        radio_standby(r);
        if (!got) {
            lost++;
            continue;
        }
        long rtt = (long)(info.t_us - t0);
        ok++;
        rtt_sum += rtt;
        if (rtt < rtt_min)
            rtt_min = rtt;
        if (rtt > rtt_max)
            rtt_max = rtt;
        if (csv)
            fprintf(csv, "%u,%ld,%d,%.2f\n", seq, rtt, info.rssi_dbm, info.snr_q4 / 4.0);
        if (o->interval_ms)
            sleep_us(o->interval_ms * 1000u);
    }
    struct flrc_phy phy = flrc_phy_default(o->flrc.bitrate_bps, o->flrc.cr);
    uint32_t air = flrc_airtime_us(&phy, o->len);
    printf("ping: %u echoed, %u lost; RTT min %ld / mean %ld / max %ld us; model airtime %u us, "
           "so turnaround (RTT - 2 x airtime) ~ %ld us\n",
           ok, lost, ok ? rtt_min : 0, ok ? rtt_sum / (long)ok : 0, rtt_max, air,
           ok ? rtt_min - 2 * (long)air : 0);
    if (csv)
        fclose(csv);
    return 0;
}

static int run_pong(struct radio *r) {
    uint8_t b[256];
    uint32_t echoed = 0;
    if (radio_rx(r))
        return 1;
    while (!stop) {
        size_t n = 0;
        struct radio_rx_info info;
        enum radio_event ev = radio_poll(r, 100000, b, sizeof(b), &n, &info);
        if (ev != RADIO_EV_RX_DONE || !info.crc_ok)
            continue;
        uint32_t t0 = now_us();
        if (radio_tx(r, b, n) == 0 && wait_tx(r, t0) >= 0)
            echoed++;
        radio_rx(r);
    }
    printf("pong: echoed %u\n", echoed);
    return 0;
}

static int run_retune(struct radio *r, const struct opts *o) {
    struct fhss_band band = o->flrc.freq_hz > 2000000000u ? FHSS_BAND_HQ_2G4 : FHSS_BAND_LR_868;
    unsigned iters = o->count ? o->count : 1000;
    long sum = 0, mx = 0, mn = 1L << 30;
    for (unsigned i = 0; i < iters && !stop; i++) {
        uint32_t f = fhss_freq_hz(&band, (uint8_t)((i * 7) % band.n));
        uint32_t t0 = now_us();
        if (radio_set_freq(r, f))
            return 1;
        long d = (long)(now_us() - t0);
        sum += d;
        if (d > mx)
            mx = d;
        if (d < mn)
            mn = d;
    }
    uint32_t t0 = now_us();
    int c = radio_calibrate(r, fhss_freq_hz(&band, band.n / 2));
    long cal = (long)(now_us() - t0);
    printf("retune (%u channels from %.2f MHz, %u x): min %ld / mean %ld / max %ld us; full "
           "calibration %ld us%s\n",
           band.n, band.start_hz / 1e6, iters, mn, sum / (long)iters, mx, cal,
           c ? " (failed)" : "");
    return 0;
}

static int parse_list(const char *s, int *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = -1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    int i = 0;
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t && i < n;
         t = strtok_r(NULL, ",", &save))
        out[i++] = (int)strtol(t, NULL, 0);
    return i;
}

int main(int argc, char **argv) {
    struct opts o = {.pins = {.spidev = "/dev/spidev0.0",
                              .spi_hz = 8000000,
                              .busy = -1,
                              .irq = -1,
                              .nreset = -1,
                              .rf_pin = {-1, -1, -1}},
                     .flrc = {.freq_hz = 868000000u,
                              .bitrate_bps = 520000,
                              .cr = 3,
                              .preamble_bits = 32,
                              .sync_word = 0x2D014B1Du,
                              .crc_bytes = 2,
                              .power_dbm = -9,
                              .tcxo_mv = 0},
                     .count = 1000,
                     .len = 255,
                     .interval_ms = 10,
                     .timeout_ms = 100};
    enum {
        O_SPIDEV = 256,
        O_SPIHZ,
        O_BUSY,
        O_IRQ,
        O_NRESET,
        O_RFPINS,
        O_RFTABLE,
        O_TCXO,
        O_FREQ,
        O_BR,
        O_CR,
        O_POWER,
        O_OUTDOOR,
        O_LEN,
        O_COUNT,
        O_INTERVAL,
        O_TIMEOUT,
        O_CSV
    };
    static const struct option lo[] = {{"spidev", 1, 0, O_SPIDEV},
                                       {"spi-hz", 1, 0, O_SPIHZ},
                                       {"busy", 1, 0, O_BUSY},
                                       {"irq", 1, 0, O_IRQ},
                                       {"nreset", 1, 0, O_NRESET},
                                       {"rf-pins", 1, 0, O_RFPINS},
                                       {"rf-table", 1, 0, O_RFTABLE},
                                       {"tcxo-mv", 1, 0, O_TCXO},
                                       {"freq", 1, 0, O_FREQ},
                                       {"bitrate", 1, 0, O_BR},
                                       {"cr", 1, 0, O_CR},
                                       {"power", 1, 0, O_POWER},
                                       {"outdoor", 0, 0, O_OUTDOOR},
                                       {"len", 1, 0, O_LEN},
                                       {"count", 1, 0, O_COUNT},
                                       {"interval-ms", 1, 0, O_INTERVAL},
                                       {"timeout-ms", 1, 0, O_TIMEOUT},
                                       {"csv", 1, 0, O_CSV},
                                       {"help", 0, 0, 'h'},
                                       {0, 0, 0, 0}};
    int c, tbl[RADIO_RF_MODES];
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case O_SPIDEV:
            o.pins.spidev = optarg;
            break;
        case O_SPIHZ:
            o.pins.spi_hz = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case O_BUSY:
            o.pins.busy = atoi(optarg);
            break;
        case O_IRQ:
            o.pins.irq = atoi(optarg);
            break;
        case O_NRESET:
            o.pins.nreset = atoi(optarg);
            break;
        case O_RFPINS:
            parse_list(optarg, o.pins.rf_pin, RADIO_RF_PINS);
            break;
        case O_RFTABLE:
            parse_list(optarg, tbl, RADIO_RF_MODES);
            for (int i = 0; i < RADIO_RF_MODES; i++)
                o.pins.rf_table[i] = (uint8_t)(tbl[i] < 0 ? 0 : tbl[i]);
            break;
        case O_TCXO:
            o.flrc.tcxo_mv = (uint16_t)atoi(optarg);
            break;
        case O_FREQ:
            o.flrc.freq_hz = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case O_BR:
            o.flrc.bitrate_bps = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case O_CR:
            o.flrc.cr = (uint8_t)atoi(optarg);
            break;
        case O_POWER:
            o.flrc.power_dbm = (int8_t)atoi(optarg);
            break;
        case O_OUTDOOR:
            o.outdoor = true;
            break;
        case O_LEN:
            o.len = (unsigned)atoi(optarg);
            break;
        case O_COUNT:
            o.count = (unsigned)atoi(optarg);
            break;
        case O_INTERVAL:
            o.interval_ms = (unsigned)atoi(optarg);
            break;
        case O_TIMEOUT:
            o.timeout_ms = (unsigned)atoi(optarg);
            break;
        case O_CSV:
            o.csv = optarg;
            break;
        default:
            usage();
            return c == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) {
        usage();
        return 2;
    }
    o.mode = argv[optind];
    if (o.pins.busy < 0 || o.pins.irq < 0 || o.pins.nreset < 0) {
        fprintf(stderr, "--busy, --irq and --nreset are required (no default wiring yet)\n");
        return 2;
    }
    if (o.flrc.power_dbm > 0 && !o.outdoor) {
        fprintf(stderr,
                "refusing %d dBm without --outdoor (the module PA adds gain; bench "
                "tests stay at <= 0 dBm through 30-40 dB attenuation)\n",
                o.flrc.power_dbm);
        return 2;
    }
    if (o.len < 12 || o.len > 255) {
        fprintf(stderr, "--len must be 12..255\n");
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct radio *r = radio_open(&o.pins, &o.flrc);
    if (!r)
        return 1;
    printf("LR2021 up: %.3f MHz, FLRC %u bit/s, CR code %u, %d dBm at the chip, %u-byte packets\n",
           o.flrc.freq_hz / 1e6, o.flrc.bitrate_bps, o.flrc.cr, o.flrc.power_dbm, o.len);
    int rc;
    if (!strcmp(o.mode, "tx"))
        rc = run_tx(r, &o);
    else if (!strcmp(o.mode, "rx"))
        rc = run_rx(r, &o);
    else if (!strcmp(o.mode, "ping"))
        rc = run_ping(r, &o);
    else if (!strcmp(o.mode, "pong"))
        rc = run_pong(r);
    else if (!strcmp(o.mode, "retune"))
        rc = run_retune(r, &o);
    else {
        usage();
        rc = 2;
    }
    radio_close(r);
    return rc;
}
