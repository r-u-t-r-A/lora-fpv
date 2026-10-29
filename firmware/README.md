# Building and flashing the firmware (Luckfox Pico Mini)

The firmware is the stock Luckfox Buildroot image from the luckfox-pico SDK
plus our programs, boot scripts and config. `build-image.sh` builds it, one
image per role (air unit, VRX); `flash.sh` puts it on a board.

**Status (2026-10-29):** the scripts have been run against a stand-in SDK
(real SDK toolchain and stock board configs, fake SDK `build.sh`), on Fedora
and in the Ubuntu 22.04 container, and the SD writer was checked byte by
byte on an image file. The full SDK build, flashing and booting a real board
have **not** been done yet: expect to report a problem or two the first
time.

The air unit sends the camera (SC3336, SC4336 or SC530AI; see
[docs/camera.md](../docs/camera.md)). With no camera it loops a "NO CAMERA"
test card, sized to fit the link. The camera program `olv-cam` has been
cross-built but not yet run on a board.

Contents: [1 Prerequisites](#1-prerequisites) ·
[2 Build](#2-build) · [3 Flash](#3-flash) · [4 First boot](#4-first-boot) ·
[5 Day-to-day](#5-day-to-day) · [6 Reference](#6-reference) ·
[7 Troubleshooting](#7-troubleshooting)

## 1. Prerequisites

- **Board:** Luckfox Pico Mini **B** (128 MB SPI NAND) or Pico Mini **A**
  (no flash, boots from a micro-SD card). Find out which you have: the B
  has a flash chip next to the SoC and is sold as "Mini B".
- **PC:** Linux x86-64. The SDK only supports Ubuntu 22.04, so on any other
  distribution (Fedora, Arch, ...) the build runs in a container: install
  `podman` (or `docker`). Reserve 20+ GB of disk for the SDK and its build
  (the SDK does not document an exact figure).
- **This repository** with submodules:
  ```sh
  git clone --recurse-submodules git@github.com:r-u-t-r-A/lora-fpv.git
  cd lora-fpv
  ```
- **The web GUI** needs Emscripten to build. Either have `emcc` installed,
  or take the build CI already made: install the GitHub CLI and run
  `gh auth login` once, then use `--web-from-ci` below.
- **For flashing:** a USB data cable (some cables are charge-only), and for
  the Mini A a micro-SD card and a card reader.
- **Radio wiring:** the images assume the pins in
  [docs/wiring.md](../docs/wiring.md) (E28: BUSY GPIO1_C6, DIO1 GPIO1_D3,
  NRESET GPIO1_D2, TXEN GPIO1_C5, RXEN GPIO1_C4, SPI0 on GPIO1_C0..C3). If
  you wire differently, edit `conf/olv-radio-e28.conf` before building or
  `/etc/olv-radio.conf` on the board afterwards.

## 2. Build

### 2.1 First build

```sh
firmware/build-in-container.sh --sdk ~/luckfox-pico --clone --web-from-ci
```

What happens:

1. The container image `olv-fw` (Ubuntu 22.04 + the SDK's host packages +
   ffmpeg) is built, once, from `firmware/Containerfile`.
2. `--clone` clones the SDK into `~/luckfox-pico` (about 1 GB).
3. Our programs are cross-compiled with the SDK toolchain; the GUI is
   fetched; test-card clips are generated with `sim/loopback`.
4. The SDK builds U-Boot, the kernel, Buildroot, the Rockchip media
   libraries and the image. **This first SDK build takes an hour or more.**
5. Two images come out:
   ```
   firmware/out/pico-mini-spi_nand-air/
   firmware/out/pico-mini-spi_nand-vrx/
   ```
   each with `update.img`, the partition images, `olv-build.txt` (git
   version and options), `partitions.txt` and `medium.txt`.

For a **Pico Mini A** add `--medium sd_card` (output:
`pico-mini-sd_card-<role>/`). On Ubuntu 22.04 with the packages from the
SDK's README installed, run `firmware/build-image.sh` with the same options
instead of `build-in-container.sh`.

### 2.2 Options

| Option | Default | |
|---|---|---|
| `--medium` | `spi_nand` | `spi_nand` = Pico Mini B, `sd_card` = Pico Mini A |
| `--role` | `both` | `air`, `vrx` or `both` |
| `--radio` | `e28` | `e28` (SX1280, 2.4 GHz, hq1300 only) or `nicerf` (LR2021) |
| `--mode` | `hq1300` (e28) / `lr520` | air link mode |
| `--bind` | `open-lr-video` | bind phrase; air and VRXs must match |
| `--vrx-id` | `1` | VRX USB address `10.55.<id>.1`; give each VRX its own id |
| `--web-from-ci` | | GUI from the latest green CI run (needs `gh auth login`) |
| `--web-dist DIR` | | use a GUI you built (`web/dist`) |
| `--air-autostart` | | the air image transmits from boot (default: only after `enabled=1`, see 4.4) |
| `--no-dhcp` | | do not enable the DHCP server in the SDK's busybox |
| `--full` | | force the full SDK build |
| `--out DIR` | `firmware/out` | where the images go |

Without `--web-from-ci`/`--web-dist` and without `emcc`, the images are
built without the GUI (the VRX still streams; you can run the GUI on the PC
with `cd web && npm run dev`).

### 2.3 Rebuilding

Run the same command again (without `--clone`). Only our part and the image
packing are redone, which takes minutes. The full SDK build runs again only
with `--full` or when `--medium` changes (another medium means another
U-Boot and partition table).

Two VRXs: build the second one with its own id, e.g.
`--role vrx --vrx-id 2` (or change `id=` in `/etc/vrx.conf` on the board,
section 5.2).

### 2.4 What the build changes in the SDK checkout

- adds `project/cfg/BoardConfig_IPC/BoardConfig-<MEDIUM>-Buildroot-RV1103_Luckfox_Pico_Mini-OLV.mk`
  (a copy of the stock Pico Mini config plus our overlay and OEM hook),
  `olv-oem-pre.sh` and `overlay/overlay-olv/`;
- points `.BoardConfig.mk` at the OLV config during the build and restores
  it afterwards, so the SDK's own `./build.sh` keeps your previous board;
- unless `--no-dhcp`: sets `CONFIG_UDHCPD=y` in
  `sysdrv/tools/board/buildroot/busybox.config` (and in Buildroot's copy,
  forcing a busybox rebuild if it was built without it).

## 3. Flash

`firmware/flash.sh` reads `medium.txt` in the image directory and does the
right thing for the board.

### 3.1 Pico Mini B (SPI NAND), over USB

1. Unplug the board. **Hold the BOOT button**, plug the USB cable into the
   PC, release BOOT after a second. The board is now in maskrom (download)
   mode: `lsusb` shows a Rockchip device (`ID 2207:...`).
2. Flash:
   ```sh
   firmware/flash.sh --sdk ~/luckfox-pico firmware/out/pico-mini-spi_nand-vrx
   ```
   It waits up to 60 s for the Rockchip device, then runs the SDK's
   `upgrade_tool uf update.img` with `sudo`. Success ends with
   `Upgrade firmware ok.` and the board reboots into the new image.
3. Repeat with the other board and `pico-mini-spi_nand-air`.

Equivalent by hand:
`sudo ~/luckfox-pico/tools/linux/Linux_Upgrade_Tool/upgrade_tool uf firmware/out/pico-mini-spi_nand-vrx/update.img`.

### 3.2 Pico Mini A (micro-SD card)

1. Put the card in the reader and find its device: `lsblk` before and after
   inserting it (e.g. `/dev/sdb` or `/dev/mmcblk0`). **Everything on it is
   erased.**
2. Unmount it if your desktop mounted it: `udisksctl unmount -b /dev/sdb1`
   (for each partition).
3. Write:
   ```sh
   firmware/flash.sh firmware/out/pico-mini-sd_card-vrx --sd /dev/sdb
   ```
   The script refuses devices that are not removable or USB-attached,
   mounted, or too small, shows the device and asks you to type `yes`. It
   writes each partition image at its offset from `partitions.txt`
   (env at 0, idblock at 32 KiB, U-Boot, boot, userdata, rootfs).
4. Put the card in the board and power it.

Alternatively write an image file and use any card writer:
`firmware/flash.sh firmware/out/pico-mini-sd_card-vrx --sd-image vrx-sd.img`,
then e.g. `sudo dd if=vrx-sd.img of=/dev/sdb bs=4M conv=fsync`.

A Pico Mini **B** only boots from SD if its SPI NAND is erased first
(Luckfox wiki); flash the B over USB (3.1) instead.

## 4. First boot

### 4.1 Connect the PC

The VRX appears on the PC as a USB network adapter. With the default DHCP
server it gives the PC `10.55.<id>.2` by itself (NetworkManager does this
automatically for a new wired connection). Without DHCP (`--no-dhcp`, or
if it did not work), set the address by hand:

```sh
ip link                                   # find the new interface, e.g. enx2a3b...
sudo ip addr add 10.55.1.2/24 dev enx2a3b...
sudo ip link set enx2a3b... up
```

The air unit keeps the stock Luckfox address `172.32.0.93`; give the PC
`172.32.0.98/24` on its interface the same way.

### 4.2 Log in and check

```sh
ssh root@10.55.1.1                        # air unit: root@172.32.0.93; password luckfox
cat /etc/olv-build                        # version and build options
ls /dev/spidev0.0                         # SPI0 is on (from /etc/luckfox.cfg)
cat /var/log/olv.log                      # output of olv-vrxd / olv-air
olv-cam --list                            # air unit: which camera is fitted
```

Expected on a VRX with the radio wired: `olv-run: role vrx: ...`, then
`vrxd 1: sx1280, scanning hq1300` and `ws://0.0.0.0:8080/rx`. If the radio
does not answer you will see RadioLib errors and the program restarting every
2 s; see section 7.

### 4.3 GUI

Open `http://10.55.1.1:8080/` in Chrome. WebCodecs (the H.265 decoder)
needs a secure context: once, add `http://10.55.1.1:8080` (and
`http://10.55.2.1:8080` for a second VRX) under
`chrome://flags/#unsafely-treat-insecure-origin-as-secure`, then restart
Chrome. The GUI finds VRXs at `10.55.1.1` to `10.55.8.1` by itself.

### 4.4 Start the air unit's transmitter

The air image does **not** transmit at boot (unless built with
`--air-autostart`), so it never transmits into a module without an antenna.
With the antenna or the attenuator chain on the module:

```sh
ssh root@172.32.0.93
sed -i 's/^enabled=0/enabled=1/' /etc/olv.conf    # start at every boot from now on
/etc/init.d/S99olv start
tail -f /var/log/olv.log                          # air: hq1300 on sx1280, ...
                                                  # air: camera: 1280x720@30, sending camera video
```

It sends at the radio conf's `power_dbm`: -18 dBm at the chip for the E28,
about **+7 dBm at the antenna port**; on the bench keep at least 47 dB of
attenuation between the modules. Above 0 dBm at the chip needs
`air_args=--outdoor` and belongs outdoors with antennas fitted.

## 5. Day-to-day

### 5.1 Service control

```sh
/etc/init.d/S99olv stop | start | restart
vi /etc/olv.conf          # role, enabled, air mode/bind/source, extra arguments
vi /etc/olv-radio.conf    # pins, power (then restart)
```

Before using `flrc_bench` (docs/m0-bench.md), stop the service so it does
not hold the radio: `/etc/init.d/S99olv stop` (and `enabled=0` to keep it
off across reboots).

### 5.2 Change a VRX's id or the bind phrase

Edit `/etc/vrx.conf` (`id=`, `bind=`; also settable from the GUI's
config API) and reboot: the USB address follows the id. For the air unit
the bind phrase is `bind=` in `/etc/olv.conf`.

### 5.3 Try new code without reflashing

The cross-built programs are in `firmware/out/work/build-rv1103/` after any
build; rebuild just them and copy them over:

```sh
cmake --build firmware/out/work/build-rv1103 --target air olv-cam vrxd flrc_bench
ssh root@10.55.1.1 /etc/init.d/S99olv stop
scp firmware/out/work/build-rv1103/vrx/vrxd root@10.55.1.1:/usr/bin/olv-vrxd
ssh root@10.55.1.1 /etc/init.d/S99olv start
```

(air unit: `air/air` to `/usr/bin/olv-air`, `air/olv-cam` to `/usr/bin/olv-cam`; bench tool:
`tools/flrc_bench` to `/usr/bin/flrc_bench`.) Reflash only when the boot
scripts, the SDK image or the GUI change.

### 5.4 Reflash

Same as section 3. The SPI NAND is fully rewritten, including `/etc`:
config changes made on the board are lost unless you also put them into
`firmware/conf/` or the build options.

## 6. Reference

### 6.1 Files on the image

| Path | |
|---|---|
| `/usr/bin/olv-air`, `olv-vrxd`, `flrc_bench` | our programs (static C++ runtime, uClibc) |
| `/usr/bin/olv-cam` | camera -> H.265 (Rockchip MPI; uses the stock `/oem/usr/lib` media libraries), started by `olv-air` |
| `/usr/bin/olv-run` | runs the role's program and restarts it if it exits |
| `/etc/olv.conf` | `role=air` / `vrx`, `enabled`, air `mode`, `bind`, `source` (`camera`, `testcard` or an .ivf), `camera_args`, `air_args`, `vrx_args` |
| `/etc/olv-radio.conf` | radio module, pins, power (from `conf/olv-radio-<radio>.conf`) |
| `/etc/vrx.conf` | VRX `id`, `band`, `mode`, `bind` |
| `/etc/luckfox.cfg` | turns on SPI0 through Luckfox's `luckfox-config load` at boot |
| `/etc/init.d/S99olv` | starts `olv-run` (after `S99luckfoxconfigload` has enabled SPI) |
| `/etc/init.d/S99usb0config` | replaces the stock one: VRX `10.55.<id>.1` + DHCP, air `172.32.0.93` |
| `/usr/share/olv/nocamera-<mode>.ivf` | looped while the camera is missing or restarting |
| `/usr/share/olv/testcard-<mode>.ivf` | the test card (`source=testcard`) |
| `/oem/usr/share/iqfiles/` | rkaiq IQ files of the supported sensors |
| `/usr/share/olv/www/` | the GUI, served by vrxd on port 8080 |
| `/etc/olv-build` | git version and build options |
| `/var/log/olv.log` | program output (in RAM; rotated at 256 KB) |

Rockchip's `rkipc` camera app, which the stock image starts from
`/oem/usr/bin/RkLunch.sh`, is kept off when `/etc/olv.conf` exists (it would
hold the camera); the kernel modules `RkLunch.sh` loads (sensor drivers, ISP,
encoder) are still loaded. The kernel gets the SC4336 and SC530AI drivers on
top of the stock SC3336 one (`olv-camera.config`).

### 6.2 Files in this directory

| File | |
|---|---|
| `build-image.sh` | the build (section 2) |
| `build-in-container.sh`, `Containerfile` | runs it in Ubuntu 22.04 |
| `flash.sh` | flashing (section 3) |
| `olv-oem-pre.sh` | SDK OEM hook: stock hook + keep rkipc off |
| `overlay/` | files copied as-is into the rootfs |
| `conf/olv-radio-*.conf` | radio conf templates per module |

## 7. Troubleshooting

| Symptom | Check |
|---|---|
| Container build fails on apt | network/proxy; retry `podman build -t olv-fw -f firmware/Containerfile firmware` |
| SDK build stops with missing host tools | build in the container, or install the package list from the SDK README |
| `no GUI` warning | `gh auth login`, then `--web-from-ci`; or install emcc |
| `lsusb` shows no `2207:` device | hold BOOT *while* plugging in; try another (data) cable and port |
| `upgrade_tool` fails to open the device | it needs root (`flash.sh` uses sudo); or install the SDK's `tools/linux/Linux_Upgrade_Tool/rockusb.rules` into `/etc/udev/rules.d/` |
| No USB network on the PC | `dmesg` on the PC for an RNDIS/ECM device; `ip link`; set the address by hand (4.1) |
| No `/dev/spidev0.0` | `cat /etc/luckfox.cfg`; run `luckfox-config load` and look for errors; `luckfox-config` can also enable SPI0 interactively |
| `olv.log`: RadioLib error -2 (chip not found) or SPI errors | wiring, module power, `spidev`/pins in `/etc/olv-radio.conf`; `cat /sys/kernel/debug/pinctrl/*/pinmux-pins` to see that the GPIOs are free |
| `olv.log`: `refusing ... without --outdoor` | power above 0 dBm at the chip: lower `power_dbm`, or `air_args=--outdoor` outdoors only |
| `olv.log`: `camera: stopped ... after 0 frames`, GUI shows NO CAMERA | `olv-cam --list` (sensor bound?), `dmesg \| grep -iE "sc3336\|sc4336\|sc530ai\|rkisp"`, ribbon cable seated; the olv-cam error just above the line says which step failed ([docs/camera.md](../docs/camera.md)) |
| GUI loads but shows no picture | Chrome flag (4.3); is the air unit transmitting (4.4)? `curl http://10.55.1.1:8080/api/stats` shows `sync` and packet counts |

When something fails, paste the command, its output, `/etc/olv-build` and
`/var/log/olv.log` from the board.

Sources for the flashing steps: Luckfox wiki,
[Pico Plus/Mini flash image](https://wiki.luckfox.com/Luckfox-Pico-Plus-Mini/Flash-image/),
[image flashing (RV1106)](https://wiki.luckfox.com/Luckfox-Pico-RV1106/Getting-Started/Image-flashing),
[login and USB network](https://wiki.luckfox.com/Luckfox-Pico-Plus-Mini/Login).
