#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stddef.h>

#include "runtime/future.h"
#include "runtime/internal.h"

bool future_settled(const future_t *f)
{
    return f->state == FUTURE_COMPLETED || f->state == FUTURE_FAILED;
}

int future_error(const future_t *f)
{
    assert(future_settled(f) && "future_error on a future that has not settled");
    return f->error;
}

intptr_t future_value(const future_t *f)
{
    assert(future_settled(f) && "future_value on a future that has not settled");
    return f->value;
}

/* Removes a pending future from its waiter's list. */
static void unlink_pending(future_t *f)
{
    task_t *t = f->waiter;

    if (f->prev != NULL)
        f->prev->next = f->next;
    else
        t->pending = f->next;
    if (f->next != NULL)
        f->next->prev = f->prev;
    f->prev = NULL;
    f->next = NULL;
}

bool future_begin(task_t *t, future_t *f, future_drop_fn drop, void *ctx)
{
    assert(t->state == TASK_RUNNING &&
           "an operation is started by its waiter, from inside its own poll");
    assert(f->state != FUTURE_PENDING && "operation started on a pending future");

    f->state = FUTURE_IDLE;
    f->error = 0;
    f->value = 0;
    f->waiter = t;
    f->drop = NULL;
    f->ctx = NULL;
    f->prev = NULL;
    f->next = NULL;

    if (t->flags & TASK_CANCELLED)
        return false;

    f->state = FUTURE_PENDING;
    f->drop = drop;
    f->ctx = ctx;
    f->next = t->pending;
    if (t->pending != NULL)
        t->pending->prev = f;
    t->pending = f;
    return true;
}

static void settle(future_t *f, unsigned state, intptr_t value, int error)
{
    assert(f->state == FUTURE_PENDING && "settling a future that is not pending");
    unlink_pending(f);
    f->state = state;
    f->value = value;
    f->error = error;
    f->drop = NULL;
    f->ctx = NULL;
    task_wake(f->waiter);
}

void future_complete(future_t *f, intptr_t value)
{
    settle(f, FUTURE_COMPLETED, value, 0);
}

void future_fail(future_t *f, int error)
{
    assert(error != 0 && "future_fail needs a non-zero error code");
    settle(f, FUTURE_FAILED, 0, error);
}

void future_drop_all(task_t *t)
{
    while (t->pending != NULL) {
        future_t *f = t->pending;
        future_drop_fn drop = f->drop;
        void *ctx = f->ctx;

        /* Idle before the hook runs, so a hook that settles f is caught. */
        unlink_pending(f);
        f->state = FUTURE_IDLE;
        f->drop = NULL;
        f->ctx = NULL;
        if (drop != NULL)
            drop(f, ctx);
    }
}
