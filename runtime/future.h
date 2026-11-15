/*
 * Futures: one-shot completion slots. See README.md, "Futures".
 *
 * A future lives in the frame of the task that waits for it. An operation
 * makes it pending, and a producer settles it later with a value or an
 * error.
 */
#ifndef TORONTO_FUTURE_H
#define TORONTO_FUTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/task.h"

typedef struct future future_t;     /* embedded in a frame; zeroed = idle */

/* True once the future is completed or failed. */
bool future_settled(const future_t *f);

/* 0 if the future completed, its error code if it failed. Only once settled. */
int future_error(const future_t *f);

/* The value of a completed future, 0 for a failed one. Only once settled. */
intptr_t future_value(const future_t *f);

/*
 * The producer side: how an operation is written.
 *
 *     void my_operation(task_t *t, future_t *f, struct thing *thing)
 *     {
 *         if (!future_begin(t, f, forget_it, thing))
 *             return;                 // t is cancelled: do nothing
 *         thing->waiting = f;         // settle it later
 *     }
 */

/*
 * The drop hook. The runtime calls it when the waiter is cancelled or
 * finishes while f is still pending, and when f times out. The producer must
 * forget f and never touch it again. The hook must not call back into the
 * runtime.
 */
typedef void (*future_drop_fn)(future_t *f, void *ctx);

/*
 * Starts an operation on f for the task t, which becomes the waiter. Must be
 * called by t itself, from inside its own poll. f must not be pending.
 *
 * Returns true with f pending and linked to t. Returns false, with f idle,
 * if t is cancelled: the operation must then do nothing.
 *
 * drop may be NULL if the producer keeps no pointer to f, which is the case
 * when it settles f before it returns.
 */
bool future_begin(task_t *t, future_t *f, future_drop_fn drop, void *ctx);

/* Settles a pending future and wakes its waiter. */
void future_complete(future_t *f, intptr_t value);
void future_fail(future_t *f, int error);       /* error must not be 0 */

/* Visible only so that a future can be embedded in a frame. Use the accessors. */

enum {                      /* future->state */
    FUTURE_IDLE,            /* no operation: never started, or dropped */
    FUTURE_PENDING,
    FUTURE_COMPLETED,
    FUTURE_FAILED
};

struct future {
    unsigned        state;
    int             error;
    intptr_t        value;
    task_t         *waiter;
    future_drop_fn  drop;
    void           *ctx;
    future_t       *prev;   /* the waiter's list of pending futures */
    future_t       *next;
    timer_entry_t   timer;  /* a sleep, or the timeout of another operation */
};

#endif
