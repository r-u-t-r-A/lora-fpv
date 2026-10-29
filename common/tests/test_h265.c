// SPDX-License-Identifier: GPL-3.0-or-later
#include "h265.h"
#include "nal_fixture.h"
#include "t.h"

static uint8_t stream[8192];

struct au_list {
    size_t off[16], len[16];
    int n;
    const uint8_t *base;
};

static void collect(void *ctx, const uint8_t *au, size_t len) {
    struct au_list *l = ctx;
    l->off[l->n] = (size_t)(au - l->base);
    l->len[l->n] = len;
    l->n++;
}

static void test_iterate_and_split(void) {
    uint32_t rng = 1;
    size_t p = 0;
    // AU0: AUD VPS SPS PPS SEI IDR(2 slices). AU1: AUD TRAIL. AU2: TRAIL (no AUD).
    p = fx_nal(stream, p, H265_NAL_AUD, 1, false, &rng);
    p = fx_nal(stream, p, H265_NAL_VPS, 20, false, &rng);
    p = fx_nal(stream, p, H265_NAL_SPS, 30, false, &rng);
    p = fx_nal(stream, p, H265_NAL_PPS, 6, false, &rng);
    p = fx_nal(stream, p, H265_NAL_SEI_PREFIX, 40, false, &rng);
    p = fx_nal(stream, p, H265_NAL_IDR_W_RADL, 500, true, &rng);
    p = fx_nal(stream, p, H265_NAL_IDR_W_RADL, 300, false, &rng);
    size_t au1 = p;
    p = fx_nal(stream, p, H265_NAL_AUD, 1, false, &rng);
    p = fx_nal(stream, p, 1, 200, true, &rng);
    size_t au2 = p;
    p = fx_nal(stream, p, 1, 150, true, &rng);

    size_t pos = 0;
    struct h265_nal nal;
    uint8_t types[16];
    int n = 0;
    while (h265_next_nal(stream, p, &pos, &nal))
        types[n++] = nal.type;
    CHECK(n == 10);
    CHECK(types[1] == H265_NAL_VPS && types[5] == H265_NAL_IDR_W_RADL && types[9] == 1);

    struct au_list l = {.base = stream};
    h265_split_aus(stream, p, collect, &l);
    CHECK(l.n == 3);
    CHECK(l.off[0] == 0 && l.len[0] == au1);
    CHECK(l.off[1] == au1 && l.len[1] == au2 - au1);
    CHECK(l.off[2] == au2 && l.len[2] == p - au2);
}

static void test_prepare_au(void) {
    uint32_t rng = 2;
    uint8_t au0[2048], au1[2048], out[4096];
    size_t n0 = 0, n1 = 0;
    n0 = fx_nal(au0, n0, H265_NAL_AUD, 1, false, &rng);
    n0 = fx_nal(au0, n0, H265_NAL_VPS, 20, false, &rng);
    n0 = fx_nal(au0, n0, H265_NAL_SPS, 30, false, &rng);
    n0 = fx_nal(au0, n0, H265_NAL_PPS, 6, false, &rng);
    n0 = fx_nal(au0, n0, H265_NAL_SEI_PREFIX, 40, false, &rng);
    n0 = fx_nal(au0, n0, H265_NAL_IDR_N_LP, 300, true, &rng);
    n1 = fx_nal(au1, n1, H265_NAL_AUD, 1, false, &rng);
    n1 = fx_nal(au1, n1, 1, 100, true, &rng);

    struct h265_ps_cache ps = {0};
    bool has_ps;
    size_t o = h265_prepare_au(&ps, au0, n0, true, out, sizeof(out), &has_ps);
    CHECK(has_ps);
    CHECK(o == n0 - (4 + 3) - (4 + 42)); // AUD and SEI removed
    CHECK(ps.vps_len == 22 && ps.sps_len == 32 && ps.pps_len == 8);

    o = h265_prepare_au(&ps, au1, n1, false, out, sizeof(out), &has_ps);
    CHECK(!has_ps && o == n1 - 7);
    o = h265_prepare_au(&ps, au1, n1, true, out, sizeof(out), &has_ps);
    CHECK(has_ps && o == (n1 - 7) + 4 * 3 + 22 + 32 + 8);
    size_t pos = 0;
    struct h265_nal nal;
    CHECK(h265_next_nal(out, o, &pos, &nal) && nal.type == H265_NAL_VPS);
    CHECK(h265_next_nal(out, o, &pos, &nal) && nal.type == H265_NAL_SPS);
    CHECK(h265_next_nal(out, o, &pos, &nal) && nal.type == H265_NAL_PPS);
    CHECK(h265_next_nal(out, o, &pos, &nal) && nal.type == 1);
    CHECK(h265_prepare_au(&ps, au1, n1, true, out, 20, &has_ps) == 0);
}

int main(void) {
    T_RUN(test_iterate_and_split);
    T_RUN(test_prepare_au);
    T_DONE();
}
