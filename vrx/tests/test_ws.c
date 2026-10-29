// SPDX-License-Identifier: GPL-3.0-or-later
#include "sha1.h"
#include "t.h"

#include <string.h>

static void test_sha1(void) {
    uint8_t d[20];
    char hex[41];
    sha1((const uint8_t *)"abc", 3, d);
    for (int i = 0; i < 20; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    // 56 bytes: padding spills into a second block.
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1((const uint8_t *)m, strlen(m), d);
    for (int i = 0; i < 20; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK(!strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

static void test_handshake_example(void) {
    // RFC 6455 section 1.3.
    const char *in = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t d[20];
    char b64[32];
    sha1((const uint8_t *)in, strlen(in), d);
    base64_encode(d, 20, b64);
    CHECK(!strcmp(b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    base64_encode((const uint8_t *)"ab", 2, b64);
    CHECK(!strcmp(b64, "YWI="));
}

int main(void) {
    T_RUN(test_sha1);
    T_RUN(test_handshake_example);
    T_DONE();
}
