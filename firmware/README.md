# Firmware images for the Luckfox Pico Mini

`build-image.sh` turns the stock luckfox-pico SDK Buildroot image into an
open-lr-video air unit or VRX: the same image Luckfox ships, plus our
programs, boot scripts and config. One script run gives one image per role.

Status: the script has been exercised against a stand-in SDK (real
toolchain, real stock board configs, fake `build.sh`); the full SDK build and
the images themselves have not been tried yet. The air unit has no camera
source yet: it sends a looping test card (`testsrc2` pattern, H.265, sized for
the link).

## Build

The SDK supports Ubuntu 22.04. On another distribution (e.g. Fedora), run
the build in the container from `Containerfile`:

```sh
# first time: clones the SDK (~1 GB) into ~/luckfox-pico and does the full
# SDK build (an hour or more), then packs an air and a VRX image
firmware/build-in-container.sh --sdk ~/luckfox-pico --clone --web-from-ci

# later runs only re-pack (minutes)
firmware/build-in-container.sh --sdk ~/luckfox-pico --web-from-ci
```

On Ubuntu 22.04 with the SDK's packages installed, call
`firmware/build-image.sh` directly with the same options.

| Option | Default | |
|---|---|---|
| `--medium` | `spi_nand` | `spi_nand` = Pico Mini B (128 MB flash), `sd_card` = Pico Mini A |
| `--role` | `both` | `air`, `vrx` or `both` |
| `--radio` | `e28` | `e28` (SX1280, 2.4 GHz, hq1300 only) or `nicerf` (LR2021) |
| `--mode` | `hq1300` / `lr520` | air link mode |
| `--bind` | `open-lr-video` | bind phrase, air and VRX |
| `--vrx-id` | `1` | the VRX's USB address is `10.55.<id>.1` |
| `--web-from-ci` | | GUI from the latest green CI run (needs `gh auth login`); or `--web-dist DIR`, or have `emcc` installed |
| `--air-autostart` | | the air image transmits from boot (default: only after `enabled=1`) |
| `--no-dhcp` | | leave the SDK's busybox alone (no DHCP server on the VRX) |

Images land in `firmware/out/pico-mini-<medium>-<role>/` (`update.img` and
the partition images).

What the script changes in the SDK checkout: it adds
`project/cfg/BoardConfig_IPC/BoardConfig-*-OLV.mk`, `olv-oem-pre.sh` and
`overlay/overlay-olv/`; it points `.BoardConfig.mk` at the OLV config during
the build and restores it afterwards; and unless `--no-dhcp` it sets
`CONFIG_UDHCPD=y` in the SDK's busybox config.

## Flash

**Pico Mini B (SPI NAND)**: hold the BOOT button while plugging in USB
(maskrom mode), then

```sh
sudo ~/luckfox-pico/tools/linux/Linux_Upgrade_Tool/upgrade_tool uf \
    firmware/out/pico-mini-spi_nand-vrx/update.img
```

**Pico Mini A (SD card)**: write the image directory to a micro-SD card with
the SDK's tools (see the Luckfox wiki, "Flashing the image" for Linux), e.g.
`sudo ./blkenvflash /dev/sdX` from inside the image directory if the SDK
generated that script.

## What is on the image

| Path | |
|---|---|
| `/usr/bin/olv-air`, `olv-vrxd`, `flrc_bench` | our programs (static C++ runtime, uClibc) |
| `/etc/olv.conf` | `role=air` or `role=vrx`, air mode/bind/source, extra arguments |
| `/etc/olv-radio.conf` | radio wiring (from `conf/olv-radio-<radio>.conf`), see `docs/wiring.md` |
| `/etc/vrx.conf` | VRX id, band, mode, bind phrase (also set via the GUI's `/api/config`) |
| `/etc/luckfox.cfg` | turns on SPI0 (spidev0.0) at boot through Luckfox's `luckfox-config load` |
| `/etc/init.d/S99olv` | starts `/usr/bin/olv-run`, which runs air or vrxd and restarts it if it exits |
| `/etc/init.d/S99usb0config` | VRX: USB address `10.55.<id>.1`, DHCP for the PC; air: stock `172.32.0.93` |
| `/usr/share/olv/testcard-<mode>.ivf` | test card the air unit loops (`source=testcard`) |
| `/usr/share/olv/www/` | the GUI, served by vrxd on port 8080 |
| `/etc/olv-build` | git version and build options |

Rockchip's `rkipc` camera app is kept from starting (it would hold the
camera); the kernel modules it loads still are.

## On the board

```sh
ssh root@10.55.1.1            # VRX (air unit: root@172.32.0.93); stock password luckfox
cat /var/log/olv.log          # what olv-air / olv-vrxd print
/etc/init.d/S99olv restart    # after editing /etc/olv.conf or /etc/olv-radio.conf
vi /etc/olv.conf              # enabled=1/0: start at boot or not (0 for flrc_bench)
```

GUI: `http://10.55.1.1:8080/` in Chrome. WebCodecs needs a secure context:
add `http://10.55.1.1:8080` under
`chrome://flags/#unsafely-treat-insecure-origin-as-secure`.

RF safety: the air image does **not** transmit at boot unless it was built
with `--air-autostart` or you set `enabled=1` in `/etc/olv.conf` (then
`/etc/init.d/S99olv start`). Do that only with an antenna or the attenuator
chain on the module. It then sends at the radio conf's `power_dbm`: -18 dBm
at the chip for the E28, about +7 dBm at the antenna port, so keep bench
setups behind at least 47 dB of attenuation.
