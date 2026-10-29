// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-1, only for the WebSocket handshake (RFC 6455), not for security.
#ifndef OLV_SHA1_H
#define OLV_SHA1_H

#include <stddef.h>
#include <stdint.h>

void sha1(const uint8_t *data, size_t len, uint8_t out[20]);

// Base64 of len bytes into out (NUL-terminated); out needs 4*ceil(len/3)+1.
void base64_encode(const uint8_t *in, size_t len, char *out);

#endif
