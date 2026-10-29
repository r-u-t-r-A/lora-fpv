#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M1 gate on the laptop: real-time air_sim -> simulated ether -> vrxd_sim.

Starts the air unit, lets it run, then starts receivers that join the running
transmission (like a VRX switched on in the field), and checks:
  - every receiver locks in under 1 s (CLAUDE.md M1 gate),
  - stays locked,
  - per-channel beacon loss matches the configured channel loss.
Prints the per-channel PER table. Exit status 0 = pass.

usage: m1_link.py BUILD_DIR IVF [--mode lr520] [--loss 0.2] [--receivers 2] [--seconds 4]
                  [--radio "module=e28-2g4m27s"] [--vrx-mode auto] [--vrx-band 2400]
                  [--lock-limit-ms 1000]

--radio is written to the radio conf of the air unit and every VRX (the
simulated radio then enforces that chip's limits); --vrx-mode/--vrx-band set
what the VRXs scan (default: exactly --mode).
"""
import argparse
import json
import os
import random
import subprocess
import sys
import time
import urllib.request


def stats(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/stats", timeout=2) as r:
        return json.load(r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("ivf")
    ap.add_argument("--mode", default="lr520")
    ap.add_argument("--loss", type=float, default=0.2)
    ap.add_argument("--receivers", type=int, default=2)
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--radio", default="", help="radio conf lines, ';'-separated")
    ap.add_argument("--vrx-mode", default=None)
    ap.add_argument("--vrx-band", default="auto")
    ap.add_argument("--lock-limit-ms", type=float, default=1000.0)
    a = ap.parse_args()
    vrx_mode = a.vrx_mode or a.mode

    env = dict(os.environ)
    env["OLV_SIM_ETHER"] = f"239.255.77.1:{random.randint(40000, 49000)}"
    base_port = random.randint(20000, 30000)
    procs = []
    conf_dir = os.path.join(a.build, "m1_link", str(os.getpid()))
    os.makedirs(conf_dir, exist_ok=True)
    radio_conf = os.path.join(conf_dir, "radio.conf")
    with open(radio_conf, "w") as f:
        f.write("".join(line.strip() + "\n" for line in a.radio.split(";") if line.strip()))
    try:
        air = subprocess.Popen(
            [f"{a.build}/air/air_sim", "--source", a.ivf, "--loop", "--mode", a.mode,
             "--radio-conf", radio_conf, "--seconds", str(a.seconds + 4)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(air)
        # Air is transmitting before anyone listens; join at a random point of
        # the hop sequence so repeated runs sample the acquisition time.
        time.sleep(1.0 + random.random())

        ports = []
        for i in range(a.receivers):
            port = base_port + i
            conf = os.path.join(conf_dir, f"vrx{i + 1}.conf")
            with open(conf, "w") as f:
                f.write(f"id={i + 1}\nmode={vrx_mode}\nband={a.vrx_band}\nbind=open-lr-video\n")
            e = dict(env, OLV_SIM_CHANNEL=f"loss={a.loss},seed={random.randint(1, 1 << 30)}")
            procs.append(subprocess.Popen(
                [f"{a.build}/vrx/vrxd_sim", "--radio", "--radio-conf", radio_conf,
                 "--config", conf, "--listen", "127.0.0.1", "--port", str(port)],
                env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            ports.append(port)
        time.sleep(a.seconds)

        ok = True
        for i, port in enumerate(ports):
            s = stats(port)
            ch = s.pop("channels")
            lock = s["first_lock_ms"]
            print(f"rx{i + 1}: {s['mode']} (of {s['modes']} scanned, {s['mode_switches']} switches), "
                  f"sync {s['sync']}, first lock {lock:.0f} ms, locks {s['locks']}, "
                  f"good {s['pkts_ok']}, crc {s['pkts_crc']}")
            print("   ch   MHz       hops beacons  PER%")
            for c in ch:
                print(f"   {c['ch']:2d}  {c['mhz']:9.3f} {c['hops']:5d} {c['beacons']:7d} "
                      f"{c['beacon_loss_pct']:5.1f}")
            hops = sum(c["hops"] for c in ch)
            beacons = sum(c["beacons"] for c in ch)
            bph = 2 if a.mode.startswith("hq") else 1  # common/modes.h beacons_per_hop
            per = 1 - beacons / (hops * bph) if hops else 1
            checks = {
                f"locked on {a.mode}": s["sync"] == "locked" and s["mode"] == a.mode,
                f"lock < {a.lock_limit_ms:.0f} ms": 0 <= lock < a.lock_limit_ms,
                "one lock": s["locks"] == 1,
                f"PER {per * 100:.1f}% ~ {a.loss * 100:.0f}%": abs(per - a.loss) < 0.08,
            }
            for name, good in checks.items():
                print(f"   {'ok  ' if good else 'FAIL'} {name}")
                ok &= good
        air.terminate()
        out = air.communicate(timeout=5)[0].strip().splitlines()
        print("air:", out[0] if out else "(no output)")
        print("air:", out[-1] if out else "")
        print("M1 gate (simulated ether):", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
