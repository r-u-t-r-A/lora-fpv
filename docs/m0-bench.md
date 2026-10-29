# M0 bench bring-up: FLRC between two Luckfox + LR2021 boards

Goal (CLAUDE.md, M0): FLRC packets on 868 MHz and 2.4 GHz between two
modules, sensitivity per bit rate measured through an attenuator, logged in
`docs/reports/`. The tool is `tools/flrc_bench`. Paste every output back so
the numbers go into the report and into `common/airtime.h`.

## RF safety

- Bench: LR2021 at its minimum power (`--power -9`, the default), and 30 to
  40 dB of attenuation between the two modules. The NiceRF PA adds a lot of
  gain on top of the LR2021 setting; check the attenuators' power rating.
- Never transmit with an RF port open (no antenna, no load).
- 1 W only outdoors with antennas fitted (`--outdoor` unlocks > 0 dBm).

## 1. Build

```sh
git submodule update --init third_party/RadioLib
cmake -S . -B build-rv1103 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/luckfox-rv1103.cmake \
      -DLUCKFOX_SDK=$HOME/luckfox-pico
ninja -C build-rv1103 flrc_bench vrxd
```

If this fails with `GPIO_V2_GET_LINE_IOCTL` undeclared, the toolchain's
kernel headers predate Linux 5.10: paste the error and I will add a fallback
to the v1 GPIO API.

## 2. Board setup (each board)

1. Enable SPI0 with `luckfox-config` (or the device tree), reboot, check:
   `ls /dev/spidev*` (paste the output).
2. Find the GPIO lines: `cat /sys/kernel/debug/gpio` (paste it), and note the
   GPIO numbers you wired in `docs/wiring.md`.
3. Copy the tool: `scp build-rv1103/tools/flrc_bench root@<board>:/root/`
   (or `adb push`).

Below, `PINS` stands for your wiring, e.g.
`--busy 54 --irq 55 --nreset 56 --rf-pins 57,58 --rf-table 0,1,2,1,2`.

## 3. Bring-up without RF

```sh
./flrc_bench retune PINS --freq 868000000 --count 1000
./flrc_bench retune PINS --freq 2440000000 --count 1000
```

Expect `LR2021 up: ...` then retune and calibration times. These two numbers
replace the 200 us retune guess in `sim/loopback`.

## 4. Link test, 868 MHz (attenuator between the boards)

Board B: `./flrc_bench rx PINS --freq 868000000 --bitrate 520000 --csv rx.csv`
Board A: `./flrc_bench tx PINS --freq 868000000 --bitrate 520000 --count 2000`

Expect near 0 % PER at high signal. Board A prints start-to-TX-done time vs.
the model airtime: paste both outputs.

## 5. Ping-pong (turnaround)

Board B: `./flrc_bench pong PINS --freq 868000000 --bitrate 520000`
Board A: `./flrc_bench ping PINS --freq 868000000 --bitrate 520000 --count 500 --csv ping.csv`

The printed turnaround is the TX/RX switching plus software latency.

## 6. Sensitivity sweep

For each bit rate (`--bitrate` 2600000, 1300000 at 2.44 GHz; 520000, 260000
at 868 MHz) and coding rate (`--cr 3`, `--cr 4`): add attenuation in steps
until PER passes 1 %, 10 %, 50 %. Note the attenuator value and the RSSI that
`rx` prints at each step. Fill in:

| Band | Bit rate | CR | Total attenuation at 1 % PER | RSSI at 1 % PER | at 10 % | at 50 % |
|---|---|---|---|---|---|---|
| 868 | 520k | 3/4 | | | | |
| 868 | 260k | 3/4 | | | | |
| 2440 | 2.6M | 3/4 | | | | |
| 2440 | 1.3M | 3/4 | | | | |

## 7. Packet timing

```sh
./flrc_bench tx PINS --bitrate 520000 --len 255 --interval-ms 0 --count 2000
./flrc_bench tx PINS --bitrate 520000 --len 64 --interval-ms 0 --count 2000
```

With `--interval-ms 0` the mean start-to-TX-done time is airtime plus
per-packet overhead; two lengths separate the two. That gives the
`tx_gap_us` and bit-count constants in `common/airtime.h`.
