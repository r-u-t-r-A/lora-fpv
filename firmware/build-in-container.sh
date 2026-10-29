#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs firmware/build-image.sh inside the Ubuntu 22.04 container from
# firmware/Containerfile (podman or docker), with this repository and the
# SDK mounted at the same paths as on the host. Options are passed through;
# --sdk is required here so the SDK directory can be mounted.
#
#   firmware/build-in-container.sh --sdk ~/luckfox-pico --clone --role both
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
engine=$(command -v podman || command -v docker || true)
[ -n "$engine" ] || { echo "need podman or docker" >&2; exit 1; }

# Directories given to --sdk, --out and --web-dist are mounted at the same
# absolute path (created first for --sdk and --out).
args=("$@")
mounts=("$repo")
sdk=
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[i]}" in
    --sdk | --out | --web-dist)
        d=${args[i + 1]:-}
        [ -n "$d" ] || { echo "${args[i]} needs a directory" >&2; exit 2; }
        [ "${args[i]}" = --web-dist ] || mkdir -p "$d"
        d=$(cd "$d" && pwd)
        args[i + 1]=$d
        mounts+=("$d")
        [ "${args[i]}" = --sdk ] && sdk=$d
        ;;
    esac
done
[ -n "$sdk" ] || { echo "usage: $0 --sdk DIR [build-image.sh options]" >&2; exit 2; }

image=olv-fw
if ! "$engine" image exists "$image" 2>/dev/null && ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    "$engine" build -t "$image" -f "$here/Containerfile" "$here"
fi

tty=(-i)
[ -t 0 ] && tty=(-it)
opts=(--rm "${tty[@]}" -w "$repo")
if [ "$(basename "$engine")" = podman ]; then
    # Rootless podman: keep our uid so the files stay ours; :z for SELinux.
    opts+=(--userns=keep-id)
    for m in "${mounts[@]}"; do opts+=(-v "$m:$m:z"); done
else
    opts+=(-u "$(id -u):$(id -g)")
    for m in "${mounts[@]}"; do opts+=(-v "$m:$m"); done
fi
exec "$engine" run "${opts[@]}" "$image" "$repo/firmware/build-image.sh" "${args[@]}"
