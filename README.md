# open-lr-video

Open-source, long-range, one-way digital video link for a UGV, built on the
Semtech LR2021 (FLRC modulation): H.265 with intra refresh, Reed-Solomon FEC,
100 Hz frequency hopping, broadcast only, diversity reception merged in the
browser.

- Air unit: Luckfox Pico Mini (RV1103) + camera + NiceRF LoRa2021F33-2G4
  (1 W, 868 MHz + 2.4 GHz), LR2021 on the RV1103's SPI.
- Ground: one or more identical VRX boxes (Luckfox RV1103 + LR2021), each a
  USB network adapter to a Linux PC; Chrome GUI merges all of them.
- Two modes: **HQ** 2.4 GHz FLRC 2.6/1.3 Mbit/s 720p, **LR** 868 MHz FLRC
  520/260 kbit/s 360p-480p.

Status: early development (M0/M2 groundwork). See `CLAUDE.md` for the plan
and `docs/protocol.md` for the wire format.

## Build and test (host)

```sh
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Needs CMake >= 3.16 and a C11 compiler. `sim/loopback` also needs `ffmpeg`
with libx265 on the PATH.

## Simulator

```sh
./build/sim/loopback --mode lr520 --mp4      # x265 -> FEC -> hopping -> 2 lossy RX -> merge -> decode
./build/sim/loopback --help                  # loss models, FEC shape, modes
```

Prints link budget, per-receiver PER, picture quality against a lossless
decode, and the M2 gate; `--mp4` writes a side-by-side comparison.

## Real-time link on one PC

`air_sim` and `vrxd_sim` are the real air and VRX programs with a simulated
radio (UDP multicast on loopback; `OLV_SIM_CHANNEL=loss=0.2` per receiver):

```sh
./build/air/air_sim --source build/m2_lr520/reference.ivf --loop &
OLV_SIM_CHANNEL=loss=0.2,seed=1 ./build/vrx/vrxd_sim --radio --port 8081 --listen 127.0.0.1 &
curl 127.0.0.1:8081/api/stats          # sync state, lock time, PER per channel
python3 sim/m1_link.py build build/m2_lr520/reference.ivf   # M1 gate
```

On the boards the same programs are built as `air` and `vrxd` (LR2021 via
RadioLib); wiring comes from `/etc/olv-radio.conf` (`common/radio_conf.h`).

## Web GUI

```sh
cd web && npm ci
npm run wasm        # needs Emscripten (emcc); CI builds it
npm test && npm run build
../sim/fake_vrx.sh  # two fake VRXs replaying a simulated flight
npm run dev         # open the URL fake_vrx.sh prints
```

## Licence

Code: GPL-3.0-or-later (`LICENSE`). Hardware: CERN-OHL-S. Third-party
components and their licences: `third_party/README.md`.
