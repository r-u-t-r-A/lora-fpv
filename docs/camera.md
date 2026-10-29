# Camera (air unit)

The air unit's camera is a MIPI CSI-2 sensor on the Luckfox Pico Mini's
camera connector. It feeds the RV1103's ISP, which runs Rockchip's 3A
(auto exposure, white balance, ...) from a per-sensor IQ tuning file. The
hardware H.265 encoder sits behind the ISP. `olv-cam` (air/src/olv_cam.c)
drives this chain through Rockchip MPI (rockit) and rkaiq. `olv-air` starts
it with settings sized for the radio link.

**Status (2026-10-30):**
- Built and checked on the host:
  - the air unit's camera input: `sim/cam_source.py` with `sim/fake_cam.py`
    standing in for the camera;
  - sensor detection: `air/tests/test_cam_sensors.c`;
  - the "NO CAMERA" fallback card.
- Cross-compiled against the SDK's prebuilt libraries, but never run on a
  board: `olv-cam` itself.
- See [Bench steps](#bench-steps) for what to run first.

## Supported sensors

| Sensor | Module | Output used | IQ file |
|---|---|---|---|
| **SC3336** | Luckfox "SC3336 3MP Camera (A)", the stock Pico camera | 2304x1296 @ 30 | `sc3336_CMK-OT2119-PC1_30IRC-F16` |
| **SC4336** | SC4336 4MP module (Luckfox kit) | 2560x1440 @ 25 | `sc4336_OT01_40IRC_F16` |
| **SC530AI** | SC530AI 5MP module (Luckfox kit) | 2880x1620 @ 30 | `sc530ai_CMK-OT2115-PC1_30IRC-F16` |

All three:
- have Rockchip kernel drivers and IQ files in the luckfox-pico SDK;
- already have device-tree nodes on the Pico's camera port
  (`rv1103-luckfox-pico-ipc.dtsi`: I2C4 at 0x30, `pwdn` on GPIO3_C5, 2 lanes);
- are plug-in, with no wiring.

The stock kernel config builds only the SC3336. The image build adds the
other two drivers (`olv-camera.config`) and all three IQ files.

**Which one is fitted:**
- At boot, Luckfox's `insmod_ko.sh` loads every sensor driver. Only the
  driver that finds its chip binds to the I2C device.
- `olv-cam` then looks under `/sys/bus/i2c/drivers/<sensor>/` for that
  binding.
- `olv-cam --list` shows the result.

**Frame rate:** none of these sensors gives 60 fps in its linear mode. So
`hq2600` (60 fps by design) runs at 30 fps (25 on the SC4336) with the full
bit rate. The encoder gets the real rate, and the air unit uses it.

**The table lives in `air/src/cam_sensors.c`.** Adding a sensor means four
things:
- a table entry;
- a driver in the SDK kernel;
- an IQ file in the SDK;
- a device-tree node on the camera port: the stock one, or a DTS patch.

`build-image.sh` reads the table for the kernel config fragment and the IQ
file list.

### Not supported: the ESP32 cameras (OV2640, OV3660, OV5640)

The cameras used by ESP32 FPV projects such as hx-esp32-cam-fpv are
parallel-bus (DVP) modules on 24-pin FPCs:

| Camera | Why it can't be used |
|---|---|
| **OV2640, OV3660** | DVP only. The Pico Mini only has 2-lane MIPI CSI. |
| **OV5640** | Exists as MIPI modules, but the SDK's OV5640 driver needs two changes before the RV1103's CSI receiver can use it (its `get_mbus_config` and a `V4L2_CID_LINK_FREQ` control). It is a YUV sensor, so it would bypass the ISP through a separate, untested path. Reports also mention needing an external 24 MHz clock. Dropped for now. |

They are also fixed-focus phone-grade sensors from around 2005. The
SC3336/SC4336 are newer and the ISP tunes them properly.

## What olv-cam does

```
sensor --MIPI--> ISP (rkaiq 3A, IQ file) --scaled NV12--> VENC H.265 --IVF--> stdout --> olv-air
```

| Setting | Value | From |
|---|---|---|
| Picture | mode's (1280x720 HQ, 640x360 LR); the ISP scales down | air `--size` |
| Frame rate | min(mode, sensor) | air `--fps` |
| Rate control | H.265 CBR, 1 s statistics window (the API minimum), QP 20..51 | air `--kbps`: 85 % of the link's video capacity |
| GOP | P only, one IDR at start; GOP length 65536 (API maximum, ~36 min at 30 fps) | |
| Intra refresh | column mode, `ceil(CTU columns / (refresh_s x fps))` columns per frame (64x64 CTUs assumed) | air `--refresh`: mode's (0.5 s HQ, 1-2 s LR) |
| Slices | at most one FEC block of bytes each (`vpkt_block_capacity`) | air `--slice-bytes` |
| Parameter sets | olv-air re-sends VPS/SPS/PPS every second (`h265_prepare_au`) | |

**How it runs inside olv-air:**
- `olv-air` runs `olv-cam` as a child process at normal priority, even when
  `olv-air` itself runs SCHED_FIFO.
- It reads the child's output from a pipe without ever blocking the hop
  loop.

**If the camera exits** (no sensor, a crash, a cable pulled):
- the air unit loops `/usr/share/olv/nocamera-<mode>.ivf`, a test pattern
  with "NO CAMERA" on it;
- it retries the camera every 5 s;
- the VRXs and the GUI see the switch only as a new IDR picture.

**If the camera stalls:** after 2 s without a picture, `olv-cam` exits
itself, which triggers the same fallback.

`olv-cam --help` lists its options. `camera_args=` in `/etc/olv.conf` passes
extras, e.g. `--flip --mirror` for an upside-down mount.

## Bench steps

On the air unit (`ssh root@172.32.0.93`, password `luckfox`), with the
camera plugged in. Paste the output back.

1. Sensor found, IQ file present:
   ```sh
   dmesg | grep -iE "sc3336|sc4336|sc530ai|rkisp|rkcif" | tail -20
   olv-cam --list
   ls /oem/usr/share/iqfiles/
   ```
2. Stop the air service so the camera and the radio are free:
   `/etc/init.d/S99olv stop`.
3. Record 5 s without the radio:
   ```sh
   olv-cam --size 1280x720 --fps 30 --kbps 800 --slice-bytes 1500 --seconds 5 --out /tmp/cam.ivf
   ```
   Then on the PC:
   ```sh
   scp root@172.32.0.93:/tmp/cam.ivf .
   ffprobe -show_frames -select_streams v cam.ivf | grep -c pict_type
   ffplay cam.ivf
   ```
   Expected: about 150 frames, an image with correct colours, and an output
   rate close to 800 kbit/s.
4. Check that the encoder really does column intra refresh and byte-limited
   slices. These are the open M2 questions in CLAUDE.md; the API accepts
   them, but what the hardware does is not known yet. On the PC:
   ```sh
   ffprobe -show_frames cam.ivf | grep pict_type | sort | uniq -c   # expect 1 I, rest P
   ffmpeg -i cam.ivf -c copy -bsf:v trace_headers -f null - 2>&1 | grep -c "slice_segment_address"
   ```
   The second number divided by the frame count is the number of slices per
   frame. A refresh sweep should be visible as a moving column when you drop
   packets with `sim/loopback --input cam.ivf` (feeds the clip through the
   simulated link).
5. With the transmitter set up as in firmware/README.md section 4.4, run
   `/etc/init.d/S99olv start` and check `/var/log/olv.log` for
   `air: camera: 1280x720@30, sending camera video`.

**If step 3 fails:**
- `RK_MPI_SYS_Init failed` or VI errors: is `rkipc` still running
  (`ps | grep rkipc`)?
- `rkaiq init failed`: is the sensor's IQ file in `/oem/usr/share/iqfiles`?
