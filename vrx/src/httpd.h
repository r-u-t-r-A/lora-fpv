// SPDX-License-Identifier: GPL-3.0-or-later
// Small single-threaded HTTP/1.1 + WebSocket server for the VRX:
//   GET /rx (Upgrade: websocket)  binary record stream to the GUI
//   GET|POST /api/...             handed to the API callback (JSON)
//   GET anything else             static file from www_root
// poll()-based, no threads, no dynamic allocation per WebSocket message.
// Slow WebSocket clients lose messages instead of stalling the radio.
#ifndef OLV_HTTPD_H
#define OLV_HTTPD_H

#include <stddef.h>
#include <stdint.h>

#define HTTPD_MAX_CLIENTS 16

// Fills resp (JSON) and returns the HTTP status. method is "GET" or "POST".
typedef int (*httpd_api_cb)(void *ctx, const char *method, const char *path, const char *body,
                            size_t body_len, char *resp, size_t resp_cap);

struct httpd_cfg {
    const char *bind_addr; // NULL = any
    uint16_t port;
    const char *www_root; // NULL = no static files
    httpd_api_cb api;
    void *ctx;
};

struct httpd;

struct httpd *httpd_open(const struct httpd_cfg *cfg);
void httpd_close(struct httpd *h);

// Runs one poll() round, waiting at most timeout_ms. Returns -1 on fatal error.
int httpd_poll(struct httpd *h, int timeout_ms);

// Queues one binary WebSocket message to every connected /rx client.
void httpd_ws_broadcast(struct httpd *h, const uint8_t *msg, size_t len);

unsigned httpd_ws_clients(const struct httpd *h);
uint32_t httpd_ws_dropped(const struct httpd *h);

#endif
