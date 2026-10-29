#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OEM pre-build hook for the open-lr-video board configs (copied next to the
# stock board configs by firmware/build-image.sh; RK_PRE_BUILD_OEM_SCRIPT).
#
# Runs the stock Luckfox hook, then stops RkLunch.sh from starting Rockchip's
# rkipc camera app when /etc/olv.conf exists: rkipc would hold the camera
# and the encoder. RkLunch.sh still loads the kernel modules (sensor, ISP,
# encoder drivers) that the air unit's camera source needs.
here=$(dirname "$(realpath "$0")")
[ -f "$here/luckfox-buildroot-oem-pre.sh" ] && bash "$here/luckfox-buildroot-oem-pre.sh"

lunch="$RK_PROJECT_PACKAGE_OEM_DIR/usr/bin/RkLunch.sh"
if [ -f "$lunch" ] && ! grep -q "olv.conf" "$lunch"; then
    sed -i 's|^\([[:space:]]*\)rkipc\b|\1[ -e /etc/olv.conf ] \|\| rkipc|' "$lunch"
    echo "olv: rkipc disabled in $lunch when /etc/olv.conf exists:"
    grep -n "rkipc" "$lunch" | grep -v "ini" || true
fi
