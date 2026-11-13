/*
 * The tests' own operations, written on top of future_begin.
 *
 * Until M3 the only built-in operation is join, so the tests bring their own:
 * a slot that one task waits on and another task fills, and an operation that
 * completes before it returns.
 */
#ifndef TORONTO_TEST_SLOT_H
#define TORONTO_TEST_SLOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/future.h"
#include "runtime/task.h"
#include "tests/test.h"

struct slot {
    future_t *waiting;  /* the future of the task that waits, or NULL */
    int       drops;    /* how often the drop hook ran */
};

/* The waiter was cancelled or has finished: forget its future. */
static inline void slot_drop(future_t *f, void *ctx)
{
    struct slot *s = ctx;

    CHECK(s->waiting == f);
    s->waiting = NULL;
    s->drops++;
}

/* The operation: f settles when another task fills or breaks the slot. */
static inline void slot_wait(task_t *t, future_t *f, struct slot *s)
{
    if (!future_begin(t, f, slot_drop, s))
        return;
    CHECK(s->waiting == NULL);
    s->waiting = f;
}

/* Completes the waiting future with value. Returns false if nobody waits. */
static inline bool slot_put(struct slot *s, intptr_t value)
{
    future_t *f = s->waiting;

    if (f == NULL)
        return false;
    s->waiting = NULL;
    future_complete(f, value);
    return true;
}

/* Fails the waiting future with error. Returns false if nobody waits. */
static inline bool slot_break(struct slot *s, int error)
{
    future_t *f = s->waiting;

    if (f == NULL)
        return false;
    s->waiting = NULL;
    future_fail(f, error);
    return true;
}

/* An operation that completes before it returns. */
static inline void ready(task_t *t, future_t *f, intptr_t value)
{
    if (future_begin(t, f, NULL, NULL))
        future_complete(f, value);
}

#endif
