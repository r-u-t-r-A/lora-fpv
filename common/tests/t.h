// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal test helpers: CHECK keeps going, REQUIRE stops the test.
#ifndef OLV_T_H
#define OLV_T_H

#include <stdio.h>
#include <stdlib.h>

static int t_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            t_failures++;                                                                          \
        }                                                                                          \
    } while (0)

#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond);             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define T_RUN(fn)                                                                                  \
    do {                                                                                           \
        int before = t_failures;                                                                   \
        fn();                                                                                      \
        printf("%-40s %s\n", #fn, t_failures == before ? "ok" : "FAIL");                           \
    } while (0)

#define T_DONE() return t_failures ? 1 : 0

#endif
