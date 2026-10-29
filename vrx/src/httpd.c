// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L

#include "httpd.h"
#include "sha1.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define IN_CAP 16384
#define WS_OUT_CAP (256 * 1024) // ~1 s of HQ traffic
#define MAX_FILE (8 * 1024 * 1024)

enum cstate { C_FREE = 0, C_HTTP, C_WS };

struct client {
    enum cstate state;
    int fd;
    bool close_after_send;
    size_t in_len;
    uint8_t in[IN_CAP];
    uint8_t *out;
    size_t out_len, out_off, out_cap;
};

struct httpd {
    struct httpd_cfg cfg;
    int lfd;
    struct client c[HTTPD_MAX_CLIENTS];
    uint32_t ws_dropped;
};

static void client_close(struct client *c) {
    if (c->fd >= 0)
        close(c->fd);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static bool out_reserve(struct client *c, size_t extra, size_t limit) {
    if (c->out_off == c->out_len)
        c->out_off = c->out_len = 0;
    if (c->out_off > 0 && c->out_len + extra > c->out_cap) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    size_t need = c->out_len + extra;
    if (need > limit)
        return false;
    if (need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < need)
            cap *= 2;
        uint8_t *p = realloc(c->out, cap);
        if (!p)
            return false;
        c->out = p;
        c->out_cap = cap;
    }
    return true;
}

static bool out_append(struct client *c, const void *data, size_t len, size_t limit) {
    if (!out_reserve(c, len, limit))
        return false;
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    return true;
}

static void flush_out(struct client *c) {
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                client_close(c);
            return;
        }
        c->out_off += (size_t)n;
    }
    if (c->close_after_send)
        client_close(c);
}

static const char *cors = "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                          "Access-Control-Allow-Headers: Content-Type\r\n";

static void respond(struct client *c, int status, const char *ctype, const void *body, size_t len) {
    const char *reason = status == 200   ? "OK"
                         : status == 204 ? "No Content"
                         : status == 400 ? "Bad Request"
                         : status == 404 ? "Not Found"
                         : status == 413 ? "Payload Too Large"
                                         : "Error";
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-cache\r\n%sConnection: close\r\n\r\n",
                     status, reason, ctype, len, cors);
    out_append(c, hdr, (size_t)n, MAX_FILE + 1024);
    if (len)
        out_append(c, body, len, MAX_FILE + 1024);
    c->close_after_send = true;
}

static const char *content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (!strcmp(dot, ".html"))
        return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js") || !strcmp(dot, ".mjs"))
        return "text/javascript";
    if (!strcmp(dot, ".css"))
        return "text/css";
    if (!strcmp(dot, ".wasm"))
        return "application/wasm";
    if (!strcmp(dot, ".json"))
        return "application/json";
    if (!strcmp(dot, ".svg"))
        return "image/svg+xml";
    if (!strcmp(dot, ".png"))
        return "image/png";
    return "application/octet-stream";
}

static void serve_file(struct httpd *h, struct client *c, const char *path) {
    if (!h->cfg.www_root || strstr(path, "..")) {
        respond(c, 404, "text/plain", "not found\n", 10);
        return;
    }
    char full[1024];
    const char *rel = (!strcmp(path, "/")) ? "/index.html" : path;
    snprintf(full, sizeof(full), "%s%s", h->cfg.www_root, rel);
    char *q = strchr(full, '?');
    if (q)
        *q = 0;
    struct stat st;
    FILE *f = NULL;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAX_FILE ||
        !(f = fopen(full, "rb"))) {
        respond(c, 404, "text/plain", "not found\n", 10);
        return;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t n = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    respond(c, 200, content_type(full), buf, n);
    free(buf);
}

static const char *header_value(const char *req, const char *name, size_t *len) {
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ')
                v++;
            const char *e = strstr(v, "\r\n");
            *len = e ? (size_t)(e - v) : strlen(v);
            return v;
        }
    }
    return NULL;
}

static void ws_upgrade(struct client *c, const char *req) {
    size_t klen;
    const char *key = header_value(req, "Sec-WebSocket-Key", &klen);
    if (!key || klen > 64) {
        respond(c, 400, "text/plain", "bad websocket request\n", 22);
        return;
    }
    char buf[128];
    memcpy(buf, key, klen);
    strcpy(buf + klen, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    uint8_t dig[20];
    sha1((const uint8_t *)buf, strlen(buf), dig);
    char acc[32];
    base64_encode(dig, 20, acc);
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                     acc);
    out_append(c, resp, (size_t)n, WS_OUT_CAP);
    c->state = C_WS;
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void handle_http(struct httpd *h, struct client *c) {
    c->in[c->in_len < IN_CAP ? c->in_len : IN_CAP - 1] = 0;
    char *req = (char *)c->in;
    char *end = strstr(req, "\r\n\r\n");
    if (!end) {
        if (c->in_len >= IN_CAP - 1)
            respond(c, 413, "text/plain", "too large\n", 10);
        return;
    }
    size_t hdr_len = (size_t)(end - req) + 4;
    size_t clen_len = 0;
    const char *cl = header_value(req, "Content-Length", &clen_len);
    size_t body_len = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (hdr_len + body_len >= IN_CAP) {
        respond(c, 413, "text/plain", "too large\n", 10);
        return;
    }
    if (c->in_len < hdr_len + body_len)
        return; // wait for the body

    char method[8] = {0}, path[512] = {0};
    if (sscanf(req, "%7s %511s", method, path) != 2) {
        respond(c, 400, "text/plain", "bad request\n", 12);
        return;
    }
    c->in_len = 0;
    if (!strcmp(method, "OPTIONS")) {
        respond(c, 204, "text/plain", NULL, 0);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/rx")) {
        ws_upgrade(c, req);
    } else if (!strncmp(path, "/api/", 5) && h->cfg.api) {
        static char resp[8192];
        char *body = req + hdr_len;
        body[body_len] = 0;
        int st = h->cfg.api(h->cfg.ctx, method, path, body, body_len, resp, sizeof(resp));
        respond(c, st, "application/json", resp, strlen(resp));
    } else if (!strcmp(method, "GET")) {
        serve_file(h, c, path);
    } else {
        respond(c, 404, "text/plain", "not found\n", 10);
    }
}

// Client-to-server frames are masked. We only act on close and ping.
static void handle_ws(struct client *c) {
    for (;;) {
        if (c->in_len < 2)
            return;
        uint8_t op = c->in[0] & 0x0F;
        bool masked = c->in[1] & 0x80;
        uint64_t len = c->in[1] & 0x7F;
        size_t off = 2;
        if (len == 126) {
            if (c->in_len < 4)
                return;
            len = (uint64_t)c->in[2] << 8 | c->in[3];
            off = 4;
        } else if (len == 127) {
            client_close(c);
            return; // nothing legitimate from the GUI is that large
        }
        size_t mask_off = off;
        if (masked)
            off += 4;
        if (len > IN_CAP - off) {
            client_close(c);
            return;
        }
        if (c->in_len < off + len)
            return;
        uint8_t *payload = c->in + off;
        if (masked)
            for (uint64_t i = 0; i < len; i++)
                payload[i] ^= c->in[mask_off + (i & 3)];
        if (op == 0x8) {
            uint8_t fr[2] = {0x88, 0};
            out_append(c, fr, 2, WS_OUT_CAP);
            c->close_after_send = true;
            return;
        }
        if (op == 0x9 && len <= 125) {
            uint8_t fr[2] = {0x8A, (uint8_t)len};
            out_append(c, fr, 2, WS_OUT_CAP);
            out_append(c, payload, (size_t)len, WS_OUT_CAP);
        }
        size_t used = off + (size_t)len;
        memmove(c->in, c->in + used, c->in_len - used);
        c->in_len -= used;
    }
}

struct httpd *httpd_open(const struct httpd_cfg *cfg) {
    struct httpd *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->cfg = *cfg;
    for (int i = 0; i < HTTPD_MAX_CLIENTS; i++)
        h->c[i].fd = -1;
    h->lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(h->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(cfg->port)};
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (cfg->bind_addr && inet_pton(AF_INET, cfg->bind_addr, &a.sin_addr) != 1) {
        fprintf(stderr, "httpd: bad bind address %s\n", cfg->bind_addr);
        close(h->lfd);
        free(h);
        return NULL;
    }
    if (h->lfd < 0 || bind(h->lfd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(h->lfd, 8) != 0) {
        perror("httpd: bind/listen");
        if (h->lfd >= 0)
            close(h->lfd);
        free(h);
        return NULL;
    }
    fcntl(h->lfd, F_SETFL, O_NONBLOCK);
    return h;
}

void httpd_close(struct httpd *h) {
    if (!h)
        return;
    for (int i = 0; i < HTTPD_MAX_CLIENTS; i++)
        if (h->c[i].state != C_FREE)
            client_close(&h->c[i]);
    close(h->lfd);
    free(h);
}

int httpd_poll(struct httpd *h, int timeout_ms) {
    struct pollfd pf[HTTPD_MAX_CLIENTS + 1];
    int idx[HTTPD_MAX_CLIENTS + 1];
    int n = 0;
    pf[n] = (struct pollfd){.fd = h->lfd, .events = POLLIN};
    idx[n++] = -1;
    for (int i = 0; i < HTTPD_MAX_CLIENTS; i++) {
        struct client *c = &h->c[i];
        if (c->state == C_FREE)
            continue;
        short ev = POLLIN;
        if (c->out_off < c->out_len)
            ev |= POLLOUT;
        pf[n] = (struct pollfd){.fd = c->fd, .events = ev};
        idx[n++] = i;
    }
    int r = poll(pf, (nfds_t)n, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    for (int k = 0; k < n; k++) {
        if (!pf[k].revents)
            continue;
        if (idx[k] < 0) {
            int fd = accept(h->lfd, NULL, NULL);
            if (fd < 0)
                continue;
            int slot = -1;
            for (int i = 0; i < HTTPD_MAX_CLIENTS && slot < 0; i++)
                if (h->c[i].state == C_FREE)
                    slot = i;
            if (slot < 0) {
                close(fd);
                continue;
            }
            fcntl(fd, F_SETFL, O_NONBLOCK);
            struct client *c = &h->c[slot];
            memset(c, 0, sizeof(*c));
            c->fd = fd;
            c->state = C_HTTP;
            continue;
        }
        struct client *c = &h->c[idx[k]];
        if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            client_close(c);
            continue;
        }
        if (pf[k].revents & POLLIN) {
            ssize_t got = recv(c->fd, c->in + c->in_len, IN_CAP - 1 - c->in_len, 0);
            if (got <= 0) {
                if (got == 0 || (errno != EAGAIN && errno != EINTR))
                    client_close(c);
                continue;
            }
            c->in_len += (size_t)got;
            if (c->state == C_HTTP)
                handle_http(h, c);
            else
                handle_ws(c);
        }
        if (c->state != C_FREE && c->out_off < c->out_len)
            flush_out(c);
    }
    return 0;
}

void httpd_ws_broadcast(struct httpd *h, const uint8_t *msg, size_t len) {
    uint8_t hdr[10];
    size_t hl;
    hdr[0] = 0x82; // FIN + binary
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hl = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hl = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hl = 10;
    }
    for (int i = 0; i < HTTPD_MAX_CLIENTS; i++) {
        struct client *c = &h->c[i];
        if (c->state != C_WS || c->close_after_send)
            continue;
        if (!out_reserve(c, hl + len, WS_OUT_CAP)) {
            h->ws_dropped++;
            continue;
        }
        memcpy(c->out + c->out_len, hdr, hl);
        memcpy(c->out + c->out_len + hl, msg, len);
        c->out_len += hl + len;
        flush_out(c);
    }
}

unsigned httpd_ws_clients(const struct httpd *h) {
    unsigned n = 0;
    for (int i = 0; i < HTTPD_MAX_CLIENTS; i++)
        n += h->c[i].state == C_WS;
    return n;
}

uint32_t httpd_ws_dropped(const struct httpd *h) {
    return h->ws_dropped;
}
