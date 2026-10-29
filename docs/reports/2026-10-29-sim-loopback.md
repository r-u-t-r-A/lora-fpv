# 2026-10-29: first loopback simulation (M2 groundwork)

All numbers come from `sim/loopback` (host only, no hardware). FLRC timing is
**estimated** (`common/airtime.h`: 32-bit preamble, 32-bit sync, 16-bit
header, 16-bit CRC, 150 us gap between packets, 200 us retune per hop). M0
must replace these with measurements.

## 1. Link capacity is below the CLAUDE.md CBR targets

100 Hz hopping with a beacon at the start of every hop, packets never
crossing a hop boundary, packet size chosen for the most payload per hop
(`airsched_best_packet`):

| Mode | Best packet | Packets/hop | Shard throughput | Video at RS 8/12 | CLAUDE.md CBR target |
|---|---|---|---|---|---|
| LR 520k, CR 3/4 | 203 B | 2 | 305 kbit/s | **203 kbit/s** | ~330 kbit/s |
| LR 260k, CR 3/4 | 189 B | 1 | 141 kbit/s | **94 kbit/s** | (robust preset) |
| HQ 2.6M, CR 3/4 | 246 B | 8 | 1497 kbit/s | **998 kbit/s** | ~1.7 Mbit/s |
| HQ 1.3M, CR 3/4 | 255 B | 4 | 777 kbit/s | **518 kbit/s** | ~0.85 Mbit/s |

The targets look like they assume no inner coding and no RS overhead
(2.6 Mbit/s x 0.65). Ways to close the gap, to be decided:

- FLRC CR 1 (uncoded) with stronger RS: +33 % raw throughput.
- RS 8/11 or 10/13 instead of 8/12.
- Beacon every 2nd hop (worst-case acquisition doubles: HQ 1.6 s).
- Measured gap/retune times may be better or worse than the guesses.
- With 255-byte packets and 10 ms hops, LR 260k cannot send a full packet at
  all (10.9 ms on air), so the shard size must follow the mode.

## 2. x265 column intra refresh does not recover cleanly

Test: encode testsrc2 640x360@30, 173 kbit/s, 1 s refresh, start decoding
at frame 30 (as a receiver that joins late or lost a frame does), compare
with the full decode.

| x265 settings | Time to reach 30 dB after join |
|---|---|
| ultrafast, intra-refresh=1 | > 7 s (never within the clip) |
| + temporal-mvp=0, sao=0, deblock=0 | > 7 s |
| veryfast, intra-refresh=1 | 2.1 s |
| medium, intra-refresh=1 | 5.3 s |
| ultrafast, IDR every 1 s (no PIR) | at the next IDR (<= 1 s) |

So x265's PIR leaks errors into already refreshed areas. This says nothing
about the RV1103 hardware encoder, which is what matters: its intra refresh
must be tested the same way in M2. Until then the simulator uses periodic
IDR (`--pir` switches to PIR).

## 3. Loopback results (LR 520k, 10 s testsrc2, IDR every 1 s)

Encoder target 173 kbit/s; after stripping filler/SEI the stream is
~145 kbit/s; airtime 73 %, capture-to-air latency 38 ms mean, 99 ms max.

| Channel | rx1 good | rx2 good | merged good | merged complete frames |
|---|---|---|---|---|
| 20 % random loss on each RX | 29 % | 35 % | **90 %** | 98.3 % |
| 5 % + bursts (GE 0.02/0.3) | 47 % | 60 % | **90 %** | 97.0 % |
| 10 % + two jammed channels / 1 s outage | 19 % | 67 % | **83 %** | 96.7 % |
| 20 % random, one RX, RS 8/16 | 70 % | | | 96.0 % |

"Good" = PSNR >= 30 dB against the lossless decode; the merged stream's
bad frames are the first ~1 s (acquisition, then waiting for the first
IDR). M2 gate as implemented (merged picture always recovers within two
refresh periods and ends good): **PASS** for LR 520k and HQ 2.6M.

A single receiver at 20 % loss does recover after every loss, but with
periodic IDR each lost block costs up to a second of damaged picture, so it
is good only ~30 % of the time. Diversity, stronger RS, or a clean intra
refresh on the RV1103 fix that.

Reproduce:

```sh
ninja -C build && ./build/sim/loopback --mode lr520 --mp4
```
