#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stand-in for olv-cam on the host: plays an IVF clip to stdout in real time.

Takes olv-cam's arguments (--size, --fps, --kbps, ... are accepted and
ignored; --fps paces the output) plus:
  --ivf FILE         clip to play (looped)
  --fail-after S     exit with status 1 after S seconds (a camera that dies)
  --fail-start       exit with status 1 at once (no camera)
The arguments it got are printed to stderr, so tests can check them.
"""
import argparse
import struct
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ivf", required=True)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--fail-after", type=float, default=0)
    ap.add_argument("--fail-start", action="store_true")
    a, _ = ap.parse_known_args()
    print("fake_cam:", " ".join(sys.argv[1:]), file=sys.stderr, flush=True)
    if a.fail_start:
        print("fake_cam: no camera found", file=sys.stderr)
        return 1
    with open(a.ivf, "rb") as f:
        hdr = bytearray(f.read(32))
        frames = []
        while True:
            fh = f.read(12)
            if len(fh) < 12:
                break
            n = struct.unpack("<I", fh[:4])[0]
            frames.append(fh + f.read(n))
    struct.pack_into("<II", hdr, 16, a.fps, 1)
    out = sys.stdout.buffer
    out.write(hdr)
    t0 = time.monotonic()
    i = 0
    try:
        while not a.fail_after or time.monotonic() - t0 < a.fail_after:
            out.write(frames[i % len(frames)])
            out.flush()
            i += 1
            time.sleep(max(0.0, t0 + i / a.fps - time.monotonic()))
    except BrokenPipeError:
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
