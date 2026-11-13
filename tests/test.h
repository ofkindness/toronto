/*
 * Shared by the tests. Each test is one program, and exit code 0 means pass.
 */
#ifndef TORONTO_TEST_H
#define TORONTO_TEST_H

#include <stdio.h>
#include <stdlib.h>

#include "runtime/runtime.h"

/* Fails the test, with the place and the condition, if cond is false. */
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #cond);                             \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

/* Every test ends with this: the runtime holds no task, timer or fd entry. */
#define CHECK_NOTHING_LEFT(rt)                                              \
    do {                                                                    \
        runtime_stats_t left_;                                              \
        runtime_stats((rt), &left_);                                        \
        CHECK(left_.tasks == 0);                                            \
        CHECK(left_.timers == 0);                                           \
        CHECK(left_.fds == 0);                                              \
    } while (0)

#endif
