// SPDX-License-Identifier: GPL-3.0-or-later
// Video sources for the air unit, each yielding H.265 access units:
//   - a file: an IVF of access units (e.g. a test card), or a raw Annex-B
//     stream on stdin ("-");
//   - a camera: a child process (olv-cam on the board) writing an IVF stream
//     to a pipe, read without blocking so the hop loop never waits on it.
#ifndef OLV_VSRC_H
#define OLV_VSRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

struct vsrc_file {
    FILE *f;
    bool ivf, loop;
    uint8_t *buf; // stdin: accumulated stream
    size_t len, cap;
};

int vsrc_file_open(struct vsrc_file *s, const char *path, bool loop);
// Next access unit into au (cap bytes). Returns its length, 0 at end.
size_t vsrc_file_next(struct vsrc_file *s, uint8_t *au, size_t cap);
// Back to the first access unit (IVF files only).
void vsrc_file_rewind(struct vsrc_file *s);
void vsrc_file_close(struct vsrc_file *s);

struct vsrc_cam {
    pid_t pid;
    int fd; // read end of the child's stdout, -1 when not running
    bool have_hdr;
    int w, h, fps; // from the IVF header
    uint8_t *buf;  // one IVF frame header + the largest access unit
    size_t len, cap;
    uint32_t frames;
};

// Starts argv[0] with its stdout on a pipe. The child runs at normal
// priority even when the caller is SCHED_FIFO. Returns 0 or -1.
int vsrc_cam_start(struct vsrc_cam *c, char *const argv[], size_t max_au);
// Reads what the pipe has. Returns the length of a complete access unit
// copied to au, 0 if none is complete yet, -1 when the camera has exited
// (EOF, bad stream or an access unit over max_au); then call vsrc_cam_stop.
long vsrc_cam_poll(struct vsrc_cam *c, uint8_t *au, size_t cap);
// Stops the child (SIGTERM, then SIGKILL after 2 s) and reaps it. Returns
// its wait status, or -1 if it was not running.
int vsrc_cam_stop(struct vsrc_cam *c);
// Asks the camera for an IDR picture (SIGUSR1).
void vsrc_cam_request_idr(const struct vsrc_cam *c);

#endif
