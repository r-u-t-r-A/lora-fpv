// SPDX-License-Identifier: GPL-3.0-or-later
#include "cam_sensors.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Modes from the drivers in the luckfox-pico SDK kernel
// (drivers/media/i2c/<name>.c, linear 10-bit modes), IQ files from its
// media/isp/.../isp_iqfiles, I2C addresses from rv1103-luckfox-pico-ipc.dtsi.
const struct cam_sensor CAM_SENSORS[] = {
    {"sc3336", "Luckfox SC3336 3MP Camera (A)", "CONFIG_VIDEO_SC3336",
     "sc3336_CMK-OT2119-PC1_30IRC-F16", 0x30, 2304, 1296, 30},
    {"sc4336", "SC4336 4MP camera (Luckfox kit)", "CONFIG_VIDEO_SC4336", "sc4336_OT01_40IRC_F16",
     0x30, 2560, 1440, 25},
    {"sc530ai", "SC530AI 5MP camera (Luckfox kit)", "CONFIG_VIDEO_SC530AI",
     "sc530ai_CMK-OT2115-PC1_30IRC-F16", 0x30, 2880, 1620, 30},
};
const size_t CAM_N_SENSORS = sizeof(CAM_SENSORS) / sizeof(CAM_SENSORS[0]);

const struct cam_sensor *cam_sensor_find(const char *name) {
    for (size_t i = 0; i < CAM_N_SENSORS; i++)
        if (!strcmp(CAM_SENSORS[i].name, name))
            return &CAM_SENSORS[i];
    return NULL;
}

// "4-0030": an I2C device (bus-address) bound to the driver.
static bool is_i2c_dev(const char *s) {
    const char *dash = strchr(s, '-');
    if (!dash || dash == s || strlen(dash + 1) != 4)
        return false;
    for (const char *p = s; p < dash; p++)
        if (!isdigit((unsigned char)*p))
            return false;
    for (const char *p = dash + 1; *p; p++)
        if (!isxdigit((unsigned char)*p))
            return false;
    return true;
}

const struct cam_sensor *cam_detect(const char *sysfs) {
    for (size_t i = 0; i < CAM_N_SENSORS; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bus/i2c/drivers/%s", sysfs, CAM_SENSORS[i].name);
        DIR *d = opendir(path);
        if (!d)
            continue;
        bool bound = false;
        struct dirent *e;
        while (!bound && (e = readdir(d)))
            bound = is_i2c_dev(e->d_name);
        closedir(d);
        if (bound)
            return &CAM_SENSORS[i];
    }
    return NULL;
}

void cam_fit_size(const struct cam_sensor *s, int *w, int *h) {
    if (*w <= 0 || *w > s->w)
        *w = s->w;
    if (*h <= 0 || *h > s->h)
        *h = s->h;
    *w &= ~1;
    *h &= ~1;
}

int cam_fit_fps(const struct cam_sensor *s, int fps) {
    return fps <= 0 || fps > s->fps ? s->fps : fps;
}

int cam_refresh_cols(int w, int fps, double refresh_s) {
    int ctus = (w + 63) / 64;
    double frames = refresh_s * fps;
    int cols = frames >= 1 ? (int)ceil(ctus / frames) : ctus;
    return cols < 1 ? 1 : cols > ctus ? ctus : cols;
}
