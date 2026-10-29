# CLAUDE.md — open-lr-video

Open-source, long-range, one-way digital video link built on the Semtech LR2021
(FLRC modulation), for a UGV running automatic ArduPilot missions. Inspired by
the BPYL d.o.o. demo (Luckfox RV1103 + LR2021, 868 MHz, 1 W, 100 Hz hopping,
FEC, intra refresh, broadcast only, diversity receive). Licence: GPL-3.0 for
all code, CERN-OHL-S for any hardware.

Full design doc: https://claude.ai/artifact/2Gy3uuAEk57jukhhHWV6YF
This file is the working summary; if the two disagree, this file wins.

## The system in one paragraph

An air unit (Luckfox Pico Mini RV1103 + 1080p camera + NiceRF
LoRa2021F33-2G4 1 W module, driven directly by the RV1103 over SPI) captures video, encodes H.265 in hard CBR with
column intra refresh, slices it, adds Reed-Solomon FEC and broadcasts it over
FLRC with 100 Hz frequency hopping. Nothing on the ground ever transmits. On
the ground, one or more identical VRX boxes (Luckfox RV1103 + LR2021 on SPI)
each appear to a Linux PC as a USB network adapter. A web GUI in Chrome
connects to every VRX over WebSocket, merges packets (first good copy of each
shard wins), rebuilds FEC in WASM, decodes H.265 with WebCodecs and draws a
MAVLink OSD.

## Fixed decisions (do not reopen without asking the user)

- Two modes only, both FLRC, both up to 1 W (the user holds licences for 1 W
  on 868 MHz and 2.4 GHz). A 2.4 GHz-only build is also supported
  (changed 2026-10-29): see "Radio chips" below.
  - **HQ**: 2.4 GHz, FLRC 2.6 or 1.3 Mbit/s, 1280x720, 60 fps / 30 fps.
  - **LR**: 868 MHz, FLRC 520 kbit/s (optional 260 kbit/s robust preset),
    640x360 to 848x480, 30 fps.
  - No LoRa video mode.
- Codec: H.265 in both modes. Ground PC is Linux; Chrome decodes via VA-API.
  Software fallback: libde265 compiled to WASM.
- No microcontroller in between: the RV1103 drives the LR2021 directly over
  SPI (`spidev`) plus GPIOs (BUSY, IRQ, NRESET, any PA/RF-switch enables on
  the NiceRF module, via the kernel GPIO character device v2 uAPI; no
  libgpiod, whose v1/v2 APIs differ between Buildroot versions). Hop clock, beacons and radio control run
  in the Luckfox userspace apps. (Changed 2026-10-29; the Pico 2 modem from the
  design doc is dropped.)
- Radio access goes through the C interface `common/radio.h`. Hardware
  backend: `radio/` = RadioLib 7.8.1 (submodule pinned to the release tag)
  with `radio/linux_hal.cpp` (spidev + GPIO chardev; IRQ edges polled with
  kernel timestamps, no callback threads); the chip is picked at run time
  from `/etc/olv-radio.conf` (`chip=` or `module=`).
- Radio chips (added 2026-10-29):
  - **LR2021** (`LR2021` class, NiceRF LoRa2021F33-2G4): both bands, all
    four modes, 255-byte packets.
  - **SX1280** (`SX1280` class, Ebyte E28-2G4M27S, preset
    `module=e28-2g4m27s`): 2.4 GHz only, FLRC up to 1.3 Mbit/s, so **hq1300
    only**; FLRC payload max 127 bytes (~14 % less video capacity); PA/LNA
    switched by the host on TXEN/RXEN; PA ~25 dB, LNA ~12 dB (from the
    ExpressLRS E28 target, to be measured).
  - Chip limits live in `common/radio_caps.c`; air, VRX and sim check modes
    against them. Do not mix LR2021 and SX1280 in one link until their FLRC
    interop is tested on the bench.
  `sim/` drives `common/` in virtual time and needs no radio.
- Air and VRX Linux software: C11 on the luckfox-pico SDK (Buildroot, uClibc,
  toolchain `arm-rockchip830-linux-uclibcgnueabihf`), Rockchip MPI (rockit) for
  camera/ISP/encoder.
- Web GUI: TypeScript (strict), Vite, no framework required; FEC via zfex
  compiled with Emscripten.
- Merge happens in the browser, not on a VRX. VRXs are dumb and identical.
- MAVLink OSD: air Luckfox reads MAVLink v2 from a spare ArduPilot serial port
  (baud configurable, default 57600), forwards a few Hz of summary telemetry
  inside the video stream. Display only; commands stay on the existing
  telemetry link.

## Repository layout (create this)

```
common/          shared C: ota.h, vrx_proto.h, fhss, fec, h265, vpkt (packetizer/reassembler), radio.h
third_party/     zfex (vendored), RadioLib (git submodule), libde265 (submodule)
radio/           LR2021 + SX1280 backends for the RV1103: RadioLib + Linux spidev/GPIO HAL (C++ shim)
air/             air-unit app for RV1103: capture -> encode -> slice -> FEC -> hop clock -> radio
vrx/             VRX daemon for RV1103: radio + RX hop sync -> WebSocket; static HTTP; config REST; usb-gadget scripts
web/             Chrome GUI (TypeScript + WASM FEC + WebCodecs + OSD)
sim/             PC-only tools: lossy channel simulator, packet replay, loopback pipeline
tools/           bench scripts: sensitivity sweeps, PER logging, plotting
docs/            wiring, protocol spec, test reports
hardware/        wiring diagrams now, KiCad carrier boards later
```

Everything in `common/`, `air/` (except the MPI glue) and `vrx/` must also build
for the x86-64 Linux host, so the whole chain can be tested in `sim/` without
hardware. Keep Rockchip-specific code behind a small interface
(`air/src/enc_rk.c` vs `air/src/enc_sw.c` using a file or v4l2 source + x265
for host builds).

## Protocol

### Over-the-air packet (one LR2021 FLRC packet)

Max payload per packet: 255 bytes total on the LR2021 (RadioLib's
RADIOLIB_LR2021_MAX_PACKET_LENGTH; still check the datasheet in M0), 127 on
the SX1280; the air sizes packets to its chip (`--max-packet` to go lower).
Header 12 bytes, little-endian, packed:

```c
struct ota_hdr {
    uint16_t link_id;   // FNV-1a of bind phrase, low 16 bits; reject mismatches
    uint16_t seq;       // packet counter, wraps
    uint8_t  hop;       // hop index within the current cycle
    uint8_t  flags;     // bits 0-1 type: 0 video, 1 telemetry, 2 beacon
                        // bit 2 IDR/param-set present, bit 3 end of frame, bits 4-5 mode
    uint16_t frame;     // video frame number
    uint8_t  block;     // FEC block number within frame
    uint8_t  shard;     // shard index 0..n-1 (shards >= k are parity)
    uint8_t  k;         // data shards in this block
    uint8_t  n;         // total shards in this block
};
```

Hardware CRC on. A packet with a bad CRC is an erasure, never passed up.

### FEC

- Inner: FLRC coding rate 3/4 by default (1/2 for the robust preset). The
  LR2021 also offers 2/3 and uncoded.
- Outer: Reed-Solomon erasure (zfex) per block; default k=8, n=12; one block
  per slice; shards of a block are spread across at least 3 hops.
- Shard payload = encoded slice bytes split evenly, last shard zero-padded,
  real length carried in a 2-byte prefix of shard 0 (bits 0-13 length,
  bit 14 CONT, bit 15 MORE for NALs split across blocks). Small blocks use
  fewer shards (min shard 32 B) at the same parity ratio. Implemented once in
  `common/vpkt.c`, used by air, sim and (via WASM) the browser.

### Hopping

- 100 Hz, 10 ms dwell. Air side owns the clock.
- Channel lists: LR 868 MHz: 10 channels, 863.5 to 869.35 MHz in 650 kHz
  steps. HQ 2.4 GHz: 40 channels, 2402 to 2480 MHz in 2 MHz steps. Make both
  configurable.
- Sequence: per cycle, a Fisher-Yates shuffle of the channel list driven by
  xorshift32 seeded with FNV-1a(bind phrase) XOR cycle number. Every channel
  appears once per cycle (ExpressLRS-style).
- Beacon (type 2) is the first packet of every hop (HQ: plus a second from
  mid-hop): cycle u32, hop_us, channel count, FEC defaults, TX power, offset
  from hop start. The header `hop` field
  is written at transmit time by the air scheduler.
- RX acquisition: park on one channel until a beacon arrives (cycles are
  reshuffled, so worst case 2n-1 hops: 190 ms LR, 790 ms HQ), take cycle and
  hop from it, start the local timer. After 3
  missed hops, coast on the local clock for 1 s, then re-park. With
  `mode=auto` in `/etc/vrx.conf` the VRX moves to the next candidate mode
  after each park timeout; `band=2400` (or `868`) restricts the scan to one
  band, and the chip's limits restrict it further (SX1280: hq1300 only).
- `common/fhss.c` is shared by the air, VRX and host code and must have unit tests
  proving air and ground generate identical sequences.

### Luckfox <-> LR2021

The RV1103 is SPI master to the LR2021 or SX1280 (`spidev`, up to the chip's
SPI limit; measure what is reliable on the wiring). GPIOs: BUSY (input),
IRQ (LR2021 DIO5 / SX1280 DIO1; input, edge events via the GPIO chardev; the
kernel event timestamp is the RX/TX done time), NRESET (output), plus the
module's PA/LNA enables (NiceRF: per its datasheet; E28: TXEN, RXEN). Hop timing runs in userspace on Linux:
measure retune time and timer jitter on the RV1103 early (M0/M1); if jitter is
too large, use SCHED_FIFO + mlockall first; offloading the hop timer to the
SoC's small MCU core (if the SDK exposes it) is a later option, not a plan.

### VRX -> browser

- Each VRX: USB RNDIS/ECM gadget with a unique subnet, `10.55.<id>.1/24`,
  `<id>` from `/etc/vrx.conf` (default derived from board serial). Do not keep
  the Luckfox default USB IP; two boards would collide.
- `GET /` serves the web GUI; `GET/POST /api/config` for band, mode, bind
  phrase; `ws://10.55.<id>.1:8080/rx` streams binary records (`common/vrx_proto.h`): RX_PACKET =
  ota bytes + rssi i16 + snr i8 + hop u8 + t_us u32, plus STATUS.
- The GUI probes `10.55.1.1` to `10.55.8.1` and connects to every VRX found.
- WebCodecs needs a secure context. Dev: add the VRX origins in
  `chrome://flags/#unsafely-treat-insecure-origin-as-secure`. Release: per-VRX
  self-signed TLS + wss.

## Encoder settings (RV1103, Rockchip MPI)

| | HQ (2.4 GHz) | LR (868 MHz) |
|---|---|---|
| Resolution | 1280x720 | 640x360 (try 848x480) |
| FPS | 60 at 2.6 Mbit/s, 30 at 1.3 Mbit/s | 30 |
| CBR target | ~1.7 / ~0.85 Mbit/s | ~330 kbit/s |
| Full intra-refresh period | ~0.5 s | 1 to 2 s |

Both: H.265, hard CBR, VBV <= 1 frame, QP floor, infinite GOP (one IDR at
start or mode change), SPS/PPS/VPS repeated every 1 s, column intra refresh,
slices sized to one FEC block, no B-frames, 1 reference frame. First thing to
verify in M2: that the RV1103 MPI exposes column intra refresh and slice
control. If not, fall back to row refresh, then to short GOPs.

## Milestones and exit gates

- **M0 Bench bring-up**: RV1103 + RadioLib (Linux HAL) drives LR2021; FLRC packets on 868
  MHz and 2.4 GHz between two modules. Gate: sensitivity per rate measured
  through an attenuator, logged in `docs/reports/`.
- **M1 Hopping modem**: 100 Hz hopping, TX-only sync, VRX daemon forwards to
  WebSocket. Gate: RX locks in under 1 s, PER logged per channel.
- **M2 Video on the bench**: RV1103 H.265 with intra refresh; full chain in
  `sim/` with configurable loss. Gate: picture recovers with 20% random packet
  loss.
- **M3 First end-to-end link**: air unit -> 2 VRXs -> Chrome GUI merging both.
  Also test 1 W 2.4 GHz video beside the ELRS 2.4 GHz receiver. Gate:
  glass-to-glass latency measured.
- **M4 Field tests**: both modes at 1 W on the UGV; 1 vs 2 VRXs vs analog.
  Gate: range report published.
- **M5 Public release**: air/VRX images, GUI, BOM, wiring, build guide, region
  power profiles with a low-power default. Gate: someone else builds one.

## Where to start (first session)

1. Scaffold the repo layout above, top-level CMake for host builds, CI with
   GitHub Actions (host build + unit tests + web build).
2. `common/ota.h`, `common/vrx_proto.h` with pack/unpack + unit tests.
3. `common/fhss.c` + tests (determinism, every channel once per cycle).
4. FEC wrapper over zfex (`common/fec.c`) + tests (any k of n recovers).
5. `sim/loopback`: encode a test clip with x265 using the settings above ->
   slice -> FEC -> packetize -> drop packets (random + bursty, two independent
   "receivers") -> merge -> decode -> write .mp4 and print stats. This is the
   main test bench until hardware arrives.
6. `radio/` skeleton: RadioLib with a Linux spidev/GPIO HAL behind
   `common/radio.h`; `tools/flrc_bench` (tx, rx, ping/pong, retune) on a fixed
   channel between two Luckfox boards, cross-compiled with
   `cmake/luckfox-rv1103.cmake`. Steps for the user: `docs/m0-bench.md`.
7. `web/` skeleton: connect to N WebSocket URLs, merge by (frame, block,
   shard), WASM FEC, WebCodecs decode to canvas, per-VRX stats panel.
   `sim/` must be able to serve fake VRX WebSockets so the GUI can be developed
   on the laptop alone.

## Working rules

- You cannot touch hardware. When something needs flashing, wiring or an RF
  test, write exact steps and ask the user to run them and paste the output.
- RF safety on the bench: never transmit into a module with no antenna or
  load. Bench tests at 0 dBm or less, through 30 to 40 dB of attenuation.
  1 W only outdoors with antennas fitted. (The E28's minimum output is about
  +7 dBm; until the user decides otherwise, its bench steps use >= 47 dB of
  attenuation and the tools print the estimated port power.)
- Prefer measuring over guessing: anything about LR2021 timing (retune time,
  max payload, TX/RX turnaround) gets a test in `tools/` and a number in
  `docs/`.
- C: C11, `-Wall -Wextra -Werror`, clang-format (LLVM style, 4 spaces), no
  dynamic allocation in the radio/hop hot path. TypeScript: `strict`, no `any`.
- Every protocol change updates `docs/protocol.md` and the shared headers in
  the same commit.
- Keep all code GPL-3.0 compatible; check licences before vendoring anything.

## Reference links

- Semtech LR2021: https://www.semtech.com/products/wireless-rf/lora-plus/lr2021
- FLRC on LR2021 (Semtech blog): https://blog.semtech.com/fast-long-range-communication-semtech-lr2021-enables-high-speed-image-transfer-without-compromise
- RadioLib LR2021 class: https://jgromes.github.io/RadioLib/class_l_r2021.html
- NiceRF LoRa2021F33-2G4 (1 W, 868 + 2.4 GHz): https://www.nicerf.com/lora-module/lora2021f33-2g4.html
- lr2021 Rust crate (reference only): https://docs.rs/crate/lr2021/0.14.0
- Prior art to read: wfb-ng (FEC + multi-RX aggregation), ExpressLRS (FHSS +
  sync), OpenHD/OpenIPC (low-bitrate FPV tuning), zfex (RS erasure codes).
- Inspiration video: https://www.youtube.com/watch?v=yNwP5HvxMqQ
