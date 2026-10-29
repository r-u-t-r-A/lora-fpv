#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Flashes an image directory made by firmware/build-image.sh.
#
#   SPI NAND (Pico Mini B), board in maskrom mode (hold BOOT while plugging in):
#     firmware/flash.sh --sdk ~/luckfox-pico firmware/out/pico-mini-spi_nand-vrx
#   SD card (Pico Mini A), card in a reader on the PC:
#     firmware/flash.sh firmware/out/pico-mini-sd_card-vrx --sd /dev/sdX
#   SD card image file (to write later with any tool, e.g. dd or Fedora Media Writer):
#     firmware/flash.sh firmware/out/pico-mini-sd_card-vrx --sd-image vrx.img
#
# SD cards: each partition image (env.img, idblock.img, uboot.img, ...) is
# written at its offset from the board config's partition table
# (partitions.txt), as the Rockchip boot ROM and U-Boot expect.
set -euo pipefail

usage() {
    sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}
die() {
    echo "flash: $*" >&2
    exit 1
}

dir=
sdk=${LUCKFOX_SDK:-}
sd=
sd_image=
yes=0
while [ $# -gt 0 ]; do
    case "$1" in
    --sdk) sdk=$2; shift ;;
    --sd) sd=$2; shift ;;
    --sd-image) sd_image=$2; shift ;;
    --yes) yes=1 ;;
    -h | --help) usage ;;
    -*) die "unknown option $1" ;;
    *) dir=$1 ;;
    esac
    shift
done
[ -n "$dir" ] || usage
[ -d "$dir" ] || die "$dir is not a directory"
medium=$(cat "$dir/medium.txt" 2>/dev/null || true)
[ -n "$medium" ] || die "$dir/medium.txt missing: not made by firmware/build-image.sh?"
[ -f "$dir/olv-build.txt" ] && sed 's/^/  /' "$dir/olv-build.txt"

# ------------------------------------------------------------- SPI NAND
if [ "$medium" = spi_nand ]; then
    [ -z "$sd$sd_image" ] || die "this is a SPI NAND image; --sd/--sd-image are for sd_card images"
    [ -f "$dir/update.img" ] || die "$dir/update.img missing"
    tool=$(command -v upgrade_tool || true)
    [ -n "$tool" ] || [ -z "$sdk" ] || tool=$sdk/tools/linux/Linux_Upgrade_Tool/upgrade_tool
    [ -n "$tool" ] && [ -x "$tool" ] || die "upgrade_tool not found: give --sdk or put it on the PATH"
    echo "Waiting for a Rockchip device in maskrom/loader mode (USB vendor 2207)."
    echo "Hold BOOT on the board while plugging in USB, then release it."
    for _ in $(seq 60); do
        lsusb 2>/dev/null | grep -q " 2207:" && break
        sleep 1
    done
    lsusb | grep " 2207:" || die "no Rockchip device after 60 s (check the cable: it must carry data)"
    # upgrade_tool keeps a config and log next to itself; run it from there.
    img=$(cd "$dir" && pwd)/update.img
    (cd "$(dirname "$tool")" && sudo ./"$(basename "$tool")" uf "$img")
    echo "Done if upgrade_tool printed 'Upgrade firmware ok.'; the board reboots into the new image."
    exit 0
fi

# -------------------------------------------------------------- SD card
[ "$medium" = sd_card ] || die "unknown medium '$medium'"
if { [ -n "$sd" ] && [ -n "$sd_image" ]; } || { [ -z "$sd" ] && [ -z "$sd_image" ]; }; then
    die "give one of --sd /dev/sdX or --sd-image FILE"
fi
parts=$(cat "$dir/partitions.txt" 2>/dev/null || true)
[ -n "$parts" ] || die "$dir/partitions.txt missing"

# "32K(env),512K@32K(idblock),256K(uboot),..., -(rootfs)" -> name offset size (bytes; size -1 = rest)
to_bytes() {
    local v=${1%[KkMmGg]} u=${1: -1}
    case "$u" in
    K | k) echo $((v * 1024)) ;;
    M | m) echo $((v * 1024 * 1024)) ;;
    G | g) echo $((v * 1024 * 1024 * 1024)) ;;
    *) echo "$1" ;;
    esac
}
layout=()
pos=0
IFS=, read -ra defs <<<"$parts"
for d in "${defs[@]}"; do
    [[ "$d" =~ ^([^@(]+)(@([^()]+))?\(([^)]+)\)$ ]] || die "cannot parse partition '$d'"
    size=${BASH_REMATCH[1]} off=${BASH_REMATCH[3]} name=${BASH_REMATCH[4]}
    [ -n "$off" ] && pos=$(to_bytes "$off")
    if [ "$size" = - ]; then bytes=-1; else bytes=$(to_bytes "$size"); fi
    layout+=("$name $pos $bytes")
    [ "$bytes" -ge 0 ] && pos=$((pos + bytes))
done

echo "Partition layout:"
total=0
for l in "${layout[@]}"; do
    read -r name off bytes <<<"$l"
    f=$dir/$name.img
    if [ -f "$f" ]; then
        fs=$(stat -c %s "$f")
        [ "$bytes" -lt 0 ] || [ "$fs" -le "$bytes" ] || die "$name.img ($fs B) is larger than its partition ($bytes B)"
        printf "  %-9s at %10d  %10s  <- %s (%d B)\n" "$name" "$off" "$([ "$bytes" -lt 0 ] && echo rest || echo "$bytes")" "$name.img" "$fs"
        end=$((off + fs))
        [ $end -gt $total ] && total=$end
    else
        printf "  %-9s at %10d  %10s  (no %s.img, left as is)\n" "$name" "$off" "$([ "$bytes" -lt 0 ] && echo rest || echo "$bytes")" "$name"
    fi
done

if [ -n "$sd_image" ]; then
    target=$sd_image
    rm -f "$target"
    truncate -s "$total" "$target"
    sudo=()
else
    target=$sd
    [ -b "$target" ] || die "$target is not a block device"
    dev=$(basename "$(readlink -f "$target")")
    [ -e "/sys/block/$dev" ] || die "$target is a partition; give the whole card, e.g. /dev/sdb or /dev/mmcblk0"
    removable=$(cat "/sys/block/$dev/removable" 2>/dev/null || echo 0)
    tran=$(lsblk -dno TRAN "$target" 2>/dev/null || true)
    size=$(lsblk -dbno SIZE "$target")
    [ "$removable" = 1 ] || [ "$tran" = usb ] || [[ "$dev" == mmcblk* ]] ||
        die "$target is not removable or USB-attached: refusing (is it really the SD card?)"
    [ "$size" -ge "$total" ] || die "$target ($size B) is smaller than the image ($total B)"
    lsblk -no MOUNTPOINT "$target" | grep -q . && die "$target has mounted partitions: unmount them first"
    echo
    lsblk -o NAME,SIZE,TRAN,MODEL,MOUNTPOINT "$target"
    if [ $yes = 0 ]; then
        read -r -p "Erase and write $target ($((size / 1000000)) MB)? Type yes: " ok
        [ "$ok" = yes ] || die "not confirmed"
    fi
    sudo=(sudo)
fi

for l in "${layout[@]}"; do
    read -r name off bytes <<<"$l"
    f=$dir/$name.img
    [ -f "$f" ] || continue
    echo "writing $name.img"
    "${sudo[@]}" dd if="$f" of="$target" bs=64K seek="$off" oflag=seek_bytes conv=notrunc,fsync status=none
done
"${sudo[@]}" sync
echo "Done: $target. Put the card in the board and power it."
