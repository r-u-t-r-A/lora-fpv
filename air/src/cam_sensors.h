// SPDX-License-Identifier: GPL-3.0-or-later
// Camera sensors the air unit supports on the Luckfox Pico (MIPI CSI-2, 2
// lanes, through the RV1103 ISP with Rockchip's rkaiq 3A and an IQ tuning
// file), and how to tell which one is fitted. Host-buildable: the detection
// reads sysfs below a root path, so tests can fake it. See docs/camera.md.
#ifndef OLV_CAM_SENSORS_H
#define OLV_CAM_SENSORS_H

#include <stddef.h>
#include <stdint.h>

struct cam_sensor {
    const char *name;    // kernel driver (and module) name, e.g. "sc3336"
    const char *module;  // camera module it comes on
    const char *kconfig; // kernel option that builds the driver
    const char *iq;      // rkaiq IQ file (in the iqfiles dir, without .json)
    uint8_t i2c_addr;    // 7-bit address on the camera connector's I2C bus
    int w, h, fps;       // sensor output the driver streams (the ISP scales it)
};

extern const struct cam_sensor CAM_SENSORS[];
extern const size_t CAM_N_SENSORS;

const struct cam_sensor *cam_sensor_find(const char *name);

// The sensor whose driver is bound to an I2C device, looking in
// <sysfs>/bus/i2c/drivers/<name>/ (sysfs is "/sys" on the board). A driver
// whose probe failed (no such chip) has no device there. NULL: none found.
const struct cam_sensor *cam_detect(const char *sysfs);

// Output picture for a requested size: the request clamped to the sensor
// (the ISP only scales down), rounded down to even numbers.
void cam_fit_size(const struct cam_sensor *s, int *w, int *h);

// Frame rate the sensor can deliver for a requested one.
int cam_fit_fps(const struct cam_sensor *s, int fps);

// Columns of 64x64 CTUs refreshed per frame so a column intra refresh sweeps
// a picture of width w in about refresh_s seconds.
int cam_refresh_cols(int w, int fps, double refresh_s);

#endif
