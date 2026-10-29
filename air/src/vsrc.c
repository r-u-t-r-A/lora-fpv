// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE // pipe2, sched_setscheduler

#include "vsrc.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "h265.h"
#include "olv_util.h"

// ------------------------------------------------------------------- files

int vsrc_file_open(struct vsrc_file *s, const char *path, bool loop) {
    memset(s, 0, sizeof(*s));
    s->loop = loop;
    if (!strcmp(path, "-")) {
        s->f = stdin;
        s->cap = 1 << 20;
        s->buf = malloc(s->cap);
        return s->buf ? 0 : -1;
    }
    s->f = fopen(path, "rb");
    if (!s->f) {
        perror(path);
        return -1;
    }
    uint8_t h[32];
    if (fread(h, 1, 32, s->f) != 32 || memcmp(h, "DKIF", 4) != 0) {
        fprintf(stderr, "%s: not an IVF file (use - for an Annex-B stream on stdin)\n", path);
        fclose(s->f);
        s->f = NULL;
        return -1;
    }
    s->ivf = true;
    return 0;
}

size_t vsrc_file_next(struct vsrc_file *s, uint8_t *au, size_t cap) {
    if (s->ivf) {
        for (int attempt = 0; attempt < 2; attempt++) {
            uint8_t fh[12];
            if (fread(fh, 1, 12, s->f) == 12) {
                uint32_t n = olv_get_u32(fh);
                if (n > cap) {
                    fseek(s->f, n, SEEK_CUR);
                    return vsrc_file_next(s, au, cap);
                }
                if (fread(au, 1, n, s->f) == n)
                    return n;
            }
            if (!s->loop)
                return 0;
            fseek(s->f, 32, SEEK_SET);
        }
        return 0;
    }
    // Annex B on stdin: an AU is complete once the next one has started.
    for (;;) {
        size_t pos = 0, first = SIZE_MAX, second = SIZE_MAX;
        struct h265_nal nal;
        bool vcl = false;
        while (h265_next_nal(s->buf, s->len, &pos, &nal)) {
            bool starts = (nal.type == H265_NAL_AUD || nal.type == H265_NAL_VPS ||
                           (h265_is_vcl(nal.type) && h265_first_slice(s->buf, &nal)));
            if (first == SIZE_MAX) {
                first = nal.start;
            } else if (starts && vcl) {
                second = nal.start;
                break;
            }
            if (h265_is_vcl(nal.type))
                vcl = true;
        }
        if (second != SIZE_MAX) {
            size_t n = second - first;
            if (n > cap)
                n = cap;
            memcpy(au, s->buf + first, n);
            memmove(s->buf, s->buf + second, s->len - second);
            s->len -= second;
            return n;
        }
        if (s->len == s->cap) {
            s->cap *= 2;
            s->buf = realloc(s->buf, s->cap);
        }
        size_t r = fread(s->buf + s->len, 1, s->cap - s->len, s->f);
        if (r == 0) {
            if (s->len == 0 || first == SIZE_MAX)
                return 0;
            size_t n = s->len - first < cap ? s->len - first : cap; // last AU
            memcpy(au, s->buf + first, n);
            s->len = 0;
            return n;
        }
        s->len += r;
    }
}

void vsrc_file_rewind(struct vsrc_file *s) {
    if (s->ivf)
        fseek(s->f, 32, SEEK_SET);
}

void vsrc_file_close(struct vsrc_file *s) {
    free(s->buf);
    if (s->f && s->f != stdin)
        fclose(s->f);
    memset(s, 0, sizeof(*s));
}

// ------------------------------------------------------------------ camera

int vsrc_cam_start(struct vsrc_cam *c, char *const argv[], size_t max_au) {
    if (!c->buf) {
        c->cap = max_au + 12;
        c->buf = malloc(c->cap);
        if (!c->buf)
            return -1;
    }
    c->fd = -1;
    c->pid = 0;
    c->have_hdr = false;
    c->len = 0;
    c->frames = 0;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) {
        perror("air: camera pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("air: fork");
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        struct sched_param sp = {.sched_priority = 0};
        sched_setscheduler(0, SCHED_OTHER, &sp);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        dup2(p[1], STDOUT_FILENO);
        execvp(argv[0], argv);
        fprintf(stderr, "air: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    c->pid = pid;
    c->fd = p[0];
    return 0;
}

long vsrc_cam_poll(struct vsrc_cam *c, uint8_t *au, size_t cap) {
    if (c->fd < 0)
        return -1;
    for (;;) {
        size_t need = c->have_hdr ? 12 : 32;
        if (c->len >= need) {
            if (!c->have_hdr) {
                if (memcmp(c->buf, "DKIF", 4) != 0) {
                    fprintf(stderr, "air: camera output is not an IVF stream\n");
                    return -1;
                }
                c->w = olv_get_u16(c->buf + 12);
                c->h = olv_get_u16(c->buf + 14);
                uint32_t rate = olv_get_u32(c->buf + 16), scale = olv_get_u32(c->buf + 20);
                c->fps = scale ? (int)(rate / scale) : 0;
                c->have_hdr = true;
                memmove(c->buf, c->buf + 32, c->len - 32);
                c->len -= 32;
                continue;
            }
            uint32_t n = olv_get_u32(c->buf);
            if (n > cap || n + 12 > c->cap) {
                fprintf(stderr, "air: camera access unit of %u B is too large\n", n);
                return -1;
            }
            if (c->len >= 12 + n) {
                memcpy(au, c->buf + 12, n);
                memmove(c->buf, c->buf + 12 + n, c->len - 12 - n);
                c->len -= 12 + n;
                c->frames++;
                return (long)n;
            }
        }
        // Read only up to the end of the current record, so a complete
        // access unit is handed over as soon as it has arrived.
        size_t want = c->have_hdr && c->len >= 12 ? 12 + olv_get_u32(c->buf) : need;
        ssize_t r = read(c->fd, c->buf + c->len, want - c->len);
        if (r > 0) {
            c->len += (size_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        return -1; // EOF or error: the camera has gone
    }
}

int vsrc_cam_stop(struct vsrc_cam *c) {
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    if (c->pid <= 0)
        return -1;
    int status = -1;
    pid_t pid = c->pid;
    c->pid = 0;
    if (waitpid(pid, &status, WNOHANG) == pid)
        return status;
    kill(pid, SIGTERM);
    for (int i = 0; i < 200; i++) { // the MPI teardown takes a moment
        if (waitpid(pid, &status, WNOHANG) == pid)
            return status;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return status;
}

void vsrc_cam_request_idr(const struct vsrc_cam *c) {
    if (c->pid > 0)
        kill(c->pid, SIGUSR1);
}
