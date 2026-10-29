# open-lr-video protocol

Status: draft, matches `common/` as of this commit. Every protocol change
updates this file and the shared headers in the same commit.

All multi-byte fields are little-endian. Implementations:
`common/ota.h`, `common/vrx_proto.h`, `common/fhss.h`, `common/vpkt.h`.

## 1. Over-the-air packet

One LR2021 FLRC packet, hardware CRC on. A packet with a bad CRC is an erasure
and is never passed up. Maximum packet: 255 bytes (design value; confirm in M0).

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `link_id` | low 16 bits of FNV-1a-32(bind phrase); mismatches dropped |
| 2 | 2 | `seq` | packet counter, wraps |
| 4 | 1 | `hop` | hop index within the current cycle, written by the air scheduler at TX time |
| 5 | 1 | `flags` | bits 0-1 type, bit 2 IDR/param sets, bit 3 end of frame, bits 4-5 mode |
| 6 | 2 | `frame` | video frame number, wraps |
| 8 | 1 | `block` | FEC block within the frame |
| 9 | 1 | `shard` | 0..n-1, `>= k` is parity |
| 10 | 1 | `k` | data shards in this block |
| 11 | 1 | `n` | total shards in this block |

Types: 0 video, 1 telemetry, 2 beacon, 3 reserved (rejected).
Modes (bits 4-5): 0 LR 868 MHz FLRC 520 k, 1 LR 260 k robust, 2 HQ 2.4 GHz
FLRC 2.6 M, 3 HQ 1.3 M.

For video packets, the receiver rejects `k == 0`, `k > n`, `shard >= n`.
Beacon and telemetry packets ignore `frame`/`block`/`shard`/`k`/`n`.

### 1.1 Beacon payload (type 2, 12 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `cycle`: hop cycle counter |
| 4 | 2 | `hop_us`: dwell per hop (10000) |
| 6 | 1 | `n_channels` |
| 7 | 1 | `fec_k` (informational) |
| 8 | 1 | `fec_n` (informational) |
| 9 | 1 | `tx_power` dBm, signed |
| 10 | 2 | `offset_us`: hop start to start of this beacon's transmission |

The air unit sends a beacon as the first packet of every hop; in the HQ
modes (40 channels) it sends a second one from mid-hop (`beacons_per_hop` =
2 in `common/modes.h`), so one lost beacon does not cost a parked receiver a
whole extra park period. The header `hop` plus the beacon `cycle` let a
parked receiver compute the full sequence, and the RX-done timestamp minus
beacon airtime minus `offset_us` gives the hop start on the receiver's clock
(either beacon works; `offset_us` says where in the hop it was sent).

## 2. Video framing and FEC

One H.265 access unit (Annex B) is one frame. Before packetizing, the air app
removes AUD, SEI and filler-data NALs and, every second, prepends the cached VPS/SPS/PPS if
the frame has none (`h265_prepare_au`).

The frame is cut into chunks, at NAL boundaries where possible (the last NAL
start that fits), otherwise mid-NAL. Each chunk is one FEC block:

```
block payload = blk_hdr u16 | chunk bytes | zero padding to k * shard_size
blk_hdr: bits 0-13 chunk length
         bit 14 CONT  chunk starts inside a NAL begun in the previous block
         bit 15 MORE  chunk's last NAL continues in the next block
```

The block payload is split into `k` equal shards; Reed-Solomon (zfex,
Vandermonde, GF(2^8)) adds `n - k` parity shards; each shard is one packet,
so all packets of a block have the same length. Defaults: k = 8, n = 12,
shard size up to 243 bytes (one block carries up to 1942 bytes of video).
Blocks whose shards would be smaller than 32 bytes use fewer shards:
`k' = ceil(payload / 32)`, `n' = k' + ceil(k' (n - k) / k)`.

All packets of the last block carry the EOF flag. Packets carrying an IRAP
NAL or a parameter set carry the IDR flag.

Transmit order: shard 0 of every block of the frame, then shard 1 of every
block, and so on, so a fade hits several blocks lightly instead of one block
hard.

### 2.1 Receiver

The receiver (one `vpkt_rx` per GUI, fed by every VRX) keeps the first copy of
each `(frame, block, shard)`, recovers a block as soon as `k` shards are in,
and emits frames in order. A frame is emitted as soon as all blocks up to the
EOF block are recovered; an incomplete frame is given up when packets for a
frame `max_wait_frames` newer (default 2) arrive, or `timeout_us` after its
first packet. In an incomplete frame, NALs that touch a lost block are dropped,
so the decoder only sees whole NAL units. Frames with no packets at all are
reported as lost.

## 3. Frequency hopping

- 100 Hz, 10 ms dwell. The air unit owns the clock.
- LR 868 MHz: 10 channels, 863.5 to 869.35 MHz, 650 kHz steps.
  HQ 2.4 GHz: 40 channels, 2402 to 2480 MHz, 2 MHz steps. Both configurable.
- Per cycle `c`: seed `s = FNV-1a-32(bind phrase)`, state `x = s XOR c`
  (`0x9E3779B9` if that is zero); `perm = [0..n-1]`; for `i = n-1` down to 1:
  `x = xorshift32(x)` (13, 17, 5), `j = x mod (i + 1)`, swap `perm[i]`,
  `perm[j]`. Hop `h` of cycle `c` uses channel `perm[h]`.
- Reference vectors (bind phrase `open-lr-video`, seed `0xf53d2da0`):
  - n = 10, cycle 0: `6 9 7 1 3 5 0 2 8 4`
  - n = 10, cycle 1: `8 9 0 3 4 2 6 7 1 5`
  - n = 10, cycle 12345: `0 3 8 9 1 7 4 2 5 6`
  - n = 40, cycle 7: `9 25 18 23 20 33 28 14 27 0 4 12 16 35 34 24 15 38 39
    26 37 30 32 6 31 8 1 29 21 22 17 36 5 3 19 10 2 13 7 11`

### 3.1 Receiver sync

- PARKED: listen on one channel until a beacon arrives. Because each cycle is
  reshuffled, the gap between two uses of a channel is up to 2n - 1 hops, so
  the park timeout is 2n + 1 hops (LR 210 ms, HQ 810 ms). On timeout, move to
  the next channel; the VRX alternates bands on repeated timeouts.
- LOCKED: hop on the local clock; each beacon re-aligns it; any valid packet
  on the expected hop marks the hop as heard.
- COASTING: after 3 consecutive hops with nothing heard, keep hopping for
  1 s; any packet re-locks. Then PARKED again, on the channel that was due
  next.

## 4. VRX to browser

`ws://10.55.<id>.1:8080/rx`, binary messages. Each message holds one or more
records `type u8 | len u16 | payload`:

| Type | Payload |
|---|---|
| 2 RX_PACKET | OTA bytes, then `rssi` i16 dBm, `snr` i8 (dB x 4), `hop` u8, `t_us` u32 (VRX monotonic clock) |
| 4 STATUS | 28 bytes, see `struct vrx_status` |
| 5 LOG | UTF-8 text |

Configuration uses `GET/POST /api/config` (JSON) on the same VRX.

## 5. Open issues

- FLRC timing constants in `common/airtime.h` are guesses until M0.
- Link capacity is below the CBR targets in CLAUDE.md (see
  `docs/reports/2026-10-29-sim-loopback.md`).
- WebCodecs requires a key frame after `configure()`. With column intra
  refresh there is only one IDR, so a GUI that starts late (or rebuilds its
  decoder after an error) marks the next complete frame carrying parameter
  sets as "key". Whether Chrome's HEVC decoder accepts that has to be tested
  in M3; the libde265 WASM fallback has no such restriction.
