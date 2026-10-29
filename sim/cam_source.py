#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Camera source on the laptop: air_sim with sim/fake_cam.py as the camera.

The camera plays for a while, dies, the fallback clip (the NO CAMERA card on
the board) takes over, the camera is restarted. One VRX listens. Checks:
  - air passes the link-sized encoder settings to the camera,
  - camera video, then the fallback, then camera video again,
  - video kept flowing through the outage (frame count ~ fps x time),
  - the VRX locked and received packets.

usage: cam_source.py BUILD_DIR IVF
"""
import json
import os
import random
import re
import subprocess
import sys
import time
import urllib.request


def main():
    build, ivf = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    seconds = 5.0
    env = dict(os.environ)
    env["OLV_SIM_ETHER"] = f"239.255.77.1:{random.randint(40000, 49000)}"
    port = random.randint(20000, 30000)
    conf_dir = os.path.join(build, "cam_source", str(os.getpid()))
    os.makedirs(conf_dir, exist_ok=True)
    radio_conf = os.path.join(conf_dir, "radio.conf")
    open(radio_conf, "w").close()
    vrx_conf = os.path.join(conf_dir, "vrx.conf")
    with open(vrx_conf, "w") as f:
        f.write("id=1\nmode=lr520\nband=auto\nbind=open-lr-video\n")
    vrx = subprocess.Popen(
        [f"{build}/vrx/vrxd_sim", "--radio", "--radio-conf", radio_conf, "--config", vrx_conf,
         "--listen", "127.0.0.1", "--port", str(port)],
        env=dict(env, OLV_SIM_CHANNEL="loss=0.05"), stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    try:
        air = subprocess.Popen(
            [f"{build}/air/air_sim", "--source", "camera", "--mode", "lr520",
             "--radio-conf", radio_conf, "--seconds", str(seconds),
             "--camera-cmd", os.path.join(here, "fake_cam.py"),
             "--camera-args", f"--ivf {ivf} --fail-after 1.5",
             "--fallback", ivf, "--camera-retry", "1"],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(seconds - 0.5)  # while air is still sending
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/stats", timeout=2) as r:
            st = json.load(r)
        out = air.communicate(timeout=20)[0]
    finally:
        vrx.terminate()
        vrx.wait(timeout=5)
    print(out)
    events = [l for l in out.splitlines() if "air: camera:" in l]
    m = re.findall(r"air: (\d+) frames \((\d+) camera\)", out)
    frames, cam_frames = (int(m[-1][0]), int(m[-1][1])) if m else (0, 0)
    seen = " ".join("C" if "sending camera" in l else "S" if "stopped" in l else "" for l in events)
    checks = {
        "camera got the link settings": all(
            o in out for o in ("--size 640x360", "--fps 30", "--kbps", "--slice-bytes", "--out -")),
        "camera, fallback, camera again": re.search(r"C\s+S\s+C", seen) is not None,
        f"video kept flowing ({frames} frames in ~{seconds - 1:.0f}+ s)": frames >= 30 * (seconds - 1),
        f"camera frames ({cam_frames})": cam_frames >= 30,
        f"VRX locked, {st.get('pkts_ok', 0)} packets": st.get("sync") == "locked"
        and st.get("pkts_ok", 0) > 100,
    }
    ok = air.returncode == 0
    for name, good in checks.items():
        print(f"{'ok  ' if good else 'FAIL'} {name}")
        ok &= good
    print("camera source:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
