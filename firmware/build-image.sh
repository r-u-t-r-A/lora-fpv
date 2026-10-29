#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds flashable Luckfox Pico Mini (RV1103) images with open-lr-video
# added to the stock luckfox-pico SDK Buildroot image.
#
# What it does, in order:
#   1. cross-compiles air, vrxd and flrc_bench with the SDK toolchain;
#   2. builds the web GUI (or takes a prebuilt one) and the test-card clips
#      the air unit sends while there is no camera source;
#   3. stages a rootfs overlay (firmware/overlay + generated config) as
#      project/cfg/BoardConfig_IPC/overlay/overlay-olv in the SDK;
#   4. writes an OLV board config next to the stock Pico Mini one (same
#      settings + our overlay + an OEM hook that keeps rkipc off), and
#      optionally enables busybox udhcpd so a VRX gives the PC an address;
#   5. runs the SDK build (full the first time, then only "firmware") once
#      per role and copies each image to firmware/out/.
#
# The SDK is only modified inside project/cfg/BoardConfig_IPC (our files,
# all named *olv*/*OLV*), its .BoardConfig.mk link (restored at exit) and,
# unless --no-dhcp, one line of its busybox config.
set -euo pipefail

usage() {
    cat <<EOF
usage: firmware/build-image.sh --sdk DIR [options]

  --sdk DIR          luckfox-pico SDK checkout (default \$LUCKFOX_SDK)
  --clone            clone the SDK into --sdk first if it is not there
  --medium M         spi_nand (Pico Mini B, default) | sd_card (Pico Mini A)
  --role R           air | vrx | both (default both: one image per role)
  --radio R          e28 (Ebyte E28-2G4M27S, default) | nicerf (LR2021)
  --mode MODE        air link mode (default: hq1300 for e28, lr520 for nicerf)
  --bind PHRASE      bind phrase for air and VRX (default open-lr-video)
  --vrx-id N         VRX id: USB address 10.55.N.1 (default 1)
  --web-dist DIR     use this prebuilt GUI (web/dist); default: web/dist if
                     it has olv.wasm, else build it if emcc is on the PATH
  --web-from-ci      download the GUI from the latest green CI run (needs gh)
  --air-autostart    air image starts transmitting at boot (default: not until
                     enabled=1 is set in /etc/olv.conf; the VRX always starts)
  --no-dhcp          do not enable udhcpd in the SDK's busybox
  --full             run the full SDK build even if it was built before
  --out DIR          where to put the images (default firmware/out)
  -h, --help

Needs: cmake, rsync, python3, ffmpeg with libx265 (test cards), and the SDK's
own host packages (see its README). The first SDK build takes a long time.
EOF
}

die() {
    echo "build-image: $*" >&2
    exit 1
}
step() { echo -e "\n=== $*"; }

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
sdk=${LUCKFOX_SDK:-}
clone=0
medium=spi_nand
role=both
radio=e28
mode=
bind=open-lr-video
vrx_id=1
web_dist=
web_from_ci=0
dhcp=1
air_autostart=0
full=0
out="$here/out"

while [ $# -gt 0 ]; do
    case "$1" in
    --sdk) sdk=$2; shift ;;
    --clone) clone=1 ;;
    --medium) medium=$2; shift ;;
    --role) role=$2; shift ;;
    --radio) radio=$2; shift ;;
    --mode) mode=$2; shift ;;
    --bind) bind=$2; shift ;;
    --vrx-id) vrx_id=$2; shift ;;
    --web-dist) web_dist=$2; shift ;;
    --web-from-ci) web_from_ci=1 ;;
    --no-dhcp) dhcp=0 ;;
    --air-autostart) air_autostart=1 ;;
    --full) full=1 ;;
    --out) out=$2; shift ;;
    -h | --help) usage; exit 0 ;;
    *) usage; die "unknown option $1" ;;
    esac
    shift
done

[ -n "$sdk" ] || { usage; die "--sdk is required (or set LUCKFOX_SDK)"; }
case "$medium" in spi_nand) MEDIUM=SPI_NAND ;; sd_card) MEDIUM=SD_CARD ;; *) die "--medium spi_nand|sd_card" ;; esac
case "$role" in air) roles=air ;; vrx) roles=vrx ;; both) roles="air vrx" ;; *) die "--role air|vrx|both" ;; esac
case "$radio" in
e28) chip=sx1280; modes=hq1300; : "${mode:=hq1300}" ;;
nicerf) chip=lr2021; modes="lr520 lr260 hq2600 hq1300"; : "${mode:=lr520}" ;;
*) die "--radio e28|nicerf" ;;
esac
case " $modes " in *" $mode "*) ;; *) die "the $radio radio cannot run mode $mode (modes: $modes)" ;; esac
[[ "$vrx_id" =~ ^[0-9]+$ ]] && [ "$vrx_id" -ge 1 ] && [ "$vrx_id" -le 254 ] || die "--vrx-id 1..254"
for t in cmake rsync python3; do command -v $t >/dev/null || die "$t not found"; done

# ------------------------------------------------------------------ SDK
if [ ! -e "$sdk/build.sh" ]; then
    [ $clone = 1 ] || die "$sdk is not a luckfox-pico SDK (use --clone to fetch it)"
    step "cloning the luckfox-pico SDK into $sdk"
    git clone --depth 1 https://github.com/LuckfoxTECH/luckfox-pico.git "$sdk"
elif [ "$(git -C "$sdk" config --get core.sparseCheckout 2>/dev/null)" = true ]; then
    step "SDK is a sparse checkout (toolchain only): fetching the rest"
    git -C "$sdk" sparse-checkout disable
fi
sdk=$(cd "$sdk" && pwd)
cfgdir=$sdk/project/cfg/BoardConfig_IPC
stock_cfg=BoardConfig-$MEDIUM-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk
olv_cfg=BoardConfig-$MEDIUM-Buildroot-RV1103_Luckfox_Pico_Mini-OLV.mk
[ -f "$cfgdir/$stock_cfg" ] || die "stock board config $stock_cfg not found in $cfgdir"
tc=$sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
[ -x "$tc-gcc" ] || die "SDK toolchain not found at $tc-gcc"

work=$out/work
mkdir -p "$work"

# ------------------------------------------------------ 1. our programs
step "cross-compiling air, vrxd, flrc_bench"
git -C "$repo" submodule update --init third_party/RadioLib >/dev/null
cmake -S "$repo" -B "$work/build-rv1103" -DCMAKE_TOOLCHAIN_FILE="$repo/cmake/luckfox-rv1103.cmake" \
    -DLUCKFOX_SDK="$sdk" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/build-rv1103" -j"$(nproc)" --target air vrxd flrc_bench

# ------------------------------------------------- 2. GUI and test cards
step "web GUI"
www=
if [ -n "$web_dist" ]; then
    [ -f "$web_dist/index.html" ] || die "--web-dist $web_dist has no index.html"
    www=$web_dist
elif [ $web_from_ci = 1 ]; then
    command -v gh >/dev/null || die "--web-from-ci needs the GitHub CLI (gh)"
    run=$(cd "$repo" && gh run list --workflow ci --branch main --status success --limit 1 \
        --json databaseId --jq '.[0].databaseId')
    [ -n "$run" ] || die "no successful CI run found"
    rm -rf "$work/web-dist" && (cd "$repo" && gh run download "$run" -n web-dist -D "$work/web-dist")
    www=$work/web-dist
elif [ -f "$repo/web/dist/olv.wasm" ] && [ -f "$repo/web/dist/index.html" ]; then
    www=$repo/web/dist
elif command -v emcc >/dev/null && command -v npm >/dev/null; then
    (cd "$repo/web" && npm ci && npm run wasm && npm run build)
    www=$repo/web/dist
else
    echo "WARNING: no GUI: no web/dist, no emcc. The VRX will stream on ws://10.55.$vrx_id.1:8080/rx"
    echo "         but not serve the GUI. Use --web-from-ci or --web-dist, or run the GUI on the PC."
fi
[ -n "$www" ] && echo "GUI from $www"

step "test cards (what the air unit sends until the camera source exists)"
command -v ffmpeg >/dev/null || die "ffmpeg (with libx265) is needed for the test cards"
cmake -S "$repo" -B "$work/build-host" -DOLV_RADIO=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/build-host" -j"$(nproc)" --target loopback
cards=$work/testcards/$chip # clips are sized for the chip's packet limit
mkdir -p "$cards"
for m in $modes; do
    clip=$cards/testcard-$m.ivf
    if [ ! -f "$clip" ] || [ "$work/build-host/sim/loopback" -nt "$clip" ]; then
        # Sized for the chip's packet limit, so it fits the link it is sent on.
        "$work/build-host/sim/loopback" --mode "$m" --chip "$chip" --seconds 10 \
            --out "$cards/$m" >"$cards/$m.log" 2>&1 ||
            die "test card for $m failed, see $cards/$m.log"
        cp "$cards/$m/reference.ivf" "$clip"
    fi
    echo "  $m: $(grep -m1 '^ *video [0-9]' "$cards/$m.log" | sed 's/^ *//')"
done

# ------------------------------------------------------ 3. rootfs overlay
stage() { # stage ROLE: assemble the overlay for one role into $work/overlay-olv
    local r=$1 o=$work/overlay-olv enabled=1
    # An air unit powered up without antenna or load must not transmit.
    [ "$r" = air ] && [ $air_autostart = 0 ] && enabled=0
    rm -rf "$o" && mkdir -p "$o/usr/bin" "$o/usr/share/olv" "$o/etc"
    rsync -a "$here/overlay/" "$o/"
    for b in air/air:olv-air vrx/vrxd:olv-vrxd tools/flrc_bench:flrc_bench; do
        "$tc-strip" -o "$o/usr/bin/${b#*:}" "$work/build-rv1103/${b%%:*}"
    done
    cp "$cards"/testcard-*.ivf "$o/usr/share/olv/"
    [ -n "$www" ] && rsync -a "$www/" "$o/usr/share/olv/www/"
    cp "$here/conf/olv-radio-$radio.conf" "$o/etc/olv-radio.conf"
    cat >"$o/etc/olv.conf" <<EOF
# open-lr-video board role, read by /etc/init.d/S99olv and S99usb0config.
role=$r              # air | vrx
enabled=$enabled            # 1: start at boot. Air: only with antenna or attenuators fitted!
# air unit
mode=$mode           # link mode: $modes
bind=$bind
source=testcard      # testcard (/usr/share/olv/testcard-<mode>.ivf) or a path to an .ivf
air_args=            # extra air options, e.g. --power -12; >0 dBm needs --outdoor
# VRX: id, band, mode and bind phrase are in /etc/vrx.conf
vrx_args=
EOF
    cat >"$o/etc/vrx.conf" <<EOF
id=$vrx_id
band=auto
mode=auto
bind=$bind
EOF
    cat >"$o/etc/olv-build" <<EOF
open-lr-video $(git -C "$repo" describe --always --dirty)
built $(date -u '+%F %T') UTC, role $r, radio $radio ($chip), $MEDIUM
EOF
    rm -rf "$cfgdir/overlay/overlay-olv"
    cp -a "$o" "$cfgdir/overlay/overlay-olv"
}

# --------------------------------------------------- 4. board config etc.
step "board config $olv_cfg"
cp "$here/olv-oem-pre.sh" "$cfgdir/olv-oem-pre.sh"
python3 - "$cfgdir/$stock_cfg" "$cfgdir/$olv_cfg" "$olv_cfg" <<'EOF'
import re, sys
src, dst, name = sys.argv[1:]
s = open(src).read()
s = re.sub(r'(?m)^export LF_ORIGIN_BOARD_CONFIG=.*$', 'export LF_ORIGIN_BOARD_CONFIG=' + name, s)
s = re.sub(r'(?m)^export RK_PRE_BUILD_OEM_SCRIPT=.*$', 'export RK_PRE_BUILD_OEM_SCRIPT=olv-oem-pre.sh', s)
m = re.search(r'(?m)^export RK_POST_OVERLAY="([^"]*)"', s)
if not m:
    sys.exit('no RK_POST_OVERLAY in ' + src)
s = s[:m.start()] + 'export RK_POST_OVERLAY="%s overlay-olv"' % m.group(1) + s[m.end():]
hdr = ('#!/bin/bash\n# Generated by open-lr-video firmware/build-image.sh from %s:\n'
       '# same board, plus the overlay-olv rootfs overlay and olv-oem-pre.sh.\n' % src.rsplit('/', 1)[1])
open(dst, 'w').write(hdr + s.replace('#!/bin/bash\n', '', 1))
EOF
grep -E "RK_POST_OVERLAY|RK_PRE_BUILD_OEM_SCRIPT|RK_KERNEL_DTS|RK_BOOT_MEDIUM" "$cfgdir/$olv_cfg" | sed 's/^/  /'

if [ $dhcp = 1 ]; then
    step "busybox udhcpd (VRX hands the PC an address on USB)"
    bb_src=$sdk/sysdrv/tools/board/buildroot/busybox.config
    br=$(ls -d "$sdk"/sysdrv/source/buildroot/buildroot-* 2>/dev/null | head -n 1 || true)
    for f in "$bb_src" ${br:+"$br/package/busybox/busybox.config"}; do
        [ -f "$f" ] || continue
        if grep -q '^# CONFIG_UDHCPD is not set' "$f"; then
            sed -i 's/^# CONFIG_UDHCPD is not set/CONFIG_UDHCPD=y/' "$f"
            echo "  enabled CONFIG_UDHCPD in ${f#$sdk/}"
        fi
    done
    # Busybox already built without it: rebuild busybox in the next SDK build.
    if [ -n "$br" ] && [ -d "$br/output/target" ] && [ ! -e "$br/output/target/usr/sbin/udhcpd" ]; then
        echo "  busybox was built without udhcpd: cleaning it so the SDK rebuilds it"
        make -C "$br" busybox-dirclean >/dev/null
        full=1
    fi
fi

# --------------------------------------------------------- 5. SDK build
link=$sdk/.BoardConfig.mk
prev=$(readlink "$link" 2>/dev/null || true)
restore() {
    if [ -n "$prev" ]; then ln -sfn "$prev" "$link"; else rm -f "$link"; fi
}
trap restore EXIT
ln -rsfn "$cfgdir/$olv_cfg" "$link"

# A full build is needed the first time and whenever the board config
# changes (another medium means another U-Boot, partition table, rootfs type).
rootfs_tar=$sdk/output/out/sysdrv_out/rootfs_uclibc_rv1106.tar
stamp=$sdk/output/.olv-board-config
[ -f "$rootfs_tar" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$olv_cfg" ] || full=1
first=1
for r in $roles; do
    step "image for role $r"
    stage "$r"
    if [ $first = 1 ] && [ $full = 1 ]; then
        echo "full SDK build (uboot, kernel, rootfs, media, app, firmware): this takes a while"
        rm -f "$stamp"
        (cd "$sdk" && ./build.sh all)
        echo "$olv_cfg" >"$stamp"
    else
        (cd "$sdk" && ./build.sh firmware)
    fi
    first=0
    dst=$out/pico-mini-$medium-$r
    rm -rf "$dst" && mkdir -p "$dst"
    cp -a "$sdk"/output/image/. "$dst"/
    cp "$work/overlay-olv/etc/olv-build" "$dst/olv-build.txt"
    # Partition table for firmware/flash.sh (SD cards are written partition
    # by partition at these offsets).
    sed -n 's/^export RK_PARTITION_CMD_IN_ENV="\(.*\)"/\1/p' "$cfgdir/$olv_cfg" >"$dst/partitions.txt"
    echo "$medium" >"$dst/medium.txt"
    echo "  -> $dst"
done

step "done"
for r in $roles; do echo "  $out/pico-mini-$medium-$r"; done
cat <<EOF

Flash (firmware/README.md, section 3):
  SPI NAND (Pico Mini B): hold BOOT while plugging in USB, then
      firmware/flash.sh --sdk $sdk $out/pico-mini-$medium-<role>
  SD card (Pico Mini A):
      firmware/flash.sh $out/pico-mini-$medium-<role> --sd /dev/sdX
EOF
