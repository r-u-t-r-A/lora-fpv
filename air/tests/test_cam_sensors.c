// SPDX-License-Identifier: GPL-3.0-or-later
// Camera sensor table, detection from a fake sysfs, picture fitting.
#define _XOPEN_SOURCE 700 // mkdtemp (uClibc hides it under plain POSIX)
#include "cam_sensors.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void mkdirs(const char *root, const char *rel) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    for (char *s = p + strlen(root) + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static void test_table(void) {
    CHECK(CAM_N_SENSORS >= 3);
    for (size_t i = 0; i < CAM_N_SENSORS; i++) {
        const struct cam_sensor *s = &CAM_SENSORS[i];
        CHECK(cam_sensor_find(s->name) == s);
        CHECK(s->iq && strncmp(s->iq, s->name, strlen(s->name)) == 0); // IQ file is per sensor
        CHECK(s->w >= 1280 && s->h >= 720 && s->fps >= 25);
    }
    CHECK(cam_sensor_find("ov2640") == NULL);
}

static void test_detect(void) {
    char root[] = "/tmp/olv-sysfs-XXXXXX";
    REQUIRE(mkdtemp(root) != NULL);
    CHECK(cam_detect(root) == NULL);
    // Drivers loaded but none bound (no camera): the stock image loads them all.
    mkdirs(root, "bus/i2c/drivers/sc3336");
    mkdirs(root, "bus/i2c/drivers/sc4336");
    mkdirs(root, "bus/i2c/drivers/sc530ai");
    CHECK(cam_detect(root) == NULL);
    mkdirs(root, "bus/i2c/drivers/sc4336/module"); // not a device
    CHECK(cam_detect(root) == NULL);
    mkdirs(root, "bus/i2c/drivers/sc4336/4-0030");
    const struct cam_sensor *s = cam_detect(root);
    CHECK(s && !strcmp(s->name, "sc4336"));
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
}

static void test_fit(void) {
    const struct cam_sensor *s = cam_sensor_find("sc3336");
    REQUIRE(s);
    int w = 1280, h = 720;
    cam_fit_size(s, &w, &h);
    CHECK(w == 1280 && h == 720);
    w = 4000, h = 3001;
    cam_fit_size(s, &w, &h);
    CHECK(w == 2304 && h == 1296);
    w = 641, h = 361;
    cam_fit_size(s, &w, &h);
    CHECK(w == 640 && h == 360);
    CHECK(cam_fit_fps(s, 60) == 30);
    CHECK(cam_fit_fps(s, 25) == 25);
    CHECK(cam_fit_fps(cam_sensor_find("sc4336"), 30) == 25);
    // 1280 px = 20 CTUs; a 0.5 s sweep at 30 fps = 15 frames -> 2 columns/frame.
    CHECK(cam_refresh_cols(1280, 30, 0.5) == 2);
    CHECK(cam_refresh_cols(640, 30, 1.0) == 1);
    CHECK(cam_refresh_cols(1280, 30, 0) == 20);
}

int main(void) {
    T_RUN(test_table);
    T_RUN(test_detect);
    T_RUN(test_fit);
    T_DONE();
}
