#!/bin/sh
# Fake VRXs for GUI work on the laptop: replays sim/loopback captures through
# vrxd, one VRX per receiver, on ports 8081, 8082, ...
#
#   sim/fake_vrx.sh [CAPTURE_DIR] [-- extra loopback options]
#
# CAPTURE_DIR defaults to build/fake_vrx and is generated with a 30 s
# loopback run if it has no rx*.vrxlog yet. Then, in web/: npm run dev and
# open the URL printed below (localhost is a secure context, so WebCodecs works).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD:-$root/build}
dir=${1:-$build/fake_vrx}
[ $# -gt 0 ] && shift
[ "${1:-}" = "--" ] && shift

if ! ls "$dir"/rx*.vrxlog >/dev/null 2>&1; then
    "$build/sim/loopback" --out "$dir" --seconds 30 "$@"
fi

pids=""
trap 'kill $pids 2>/dev/null' EXIT INT TERM
urls=""
i=1
for log in "$dir"/rx*.vrxlog; do
    port=$((8080 + i))
    "$build/vrx/vrxd" --replay "$log" --loop --listen 127.0.0.1 --port "$port" --id "$i" \
        --www "$root/web/dist" &
    pids="$pids $!"
    urls="$urls${urls:+,}ws://127.0.0.1:$port/rx"
    i=$((i + 1))
done
echo
echo "GUI (npm run dev in web/): http://localhost:5173/?vrx=$urls"
echo "or served by the fake VRX:  http://127.0.0.1:8081/?vrx=$urls  (after npm run build)"
wait
