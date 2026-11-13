#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime/async.h"
#include "runtime/internal.h"

/*
 * A task and its frame are one allocation: the task, padding, then the frame.
 * The padding makes the frame suitably aligned for any type.
 */
static size_t frame_offset(void)
{
    size_t align = alignof(max_align_t);

    return (sizeof(task_t) + align - 1) / align * align;
}

task_t *task_spawn(runtime_t *rt, task_fn fn, size_t frame_size)
{
    size_t offset = frame_offset();
    task_t *t;

    assert(rt != NULL && fn != NULL);

    if (frame_size > SIZE_MAX - offset) {
        errno = ENOMEM;
        return NULL;
    }
    t = calloc(1, offset + frame_size);
    if (t == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    t->fn = fn;
    t->rt = rt;
    t->frame = (char *)t + offset;
    t->state = TASK_IDLE;

    /* The shutdown must end, so a task spawned during it only runs its cleanup. */
    if (rt->shutting_down) {
        t->flags |= TASK_CANCELLED;
        t->cancel_reason = ECANCELED;
    }

    t->all_next = rt->all;
    if (rt->all != NULL)
        rt->all->all_prev = t;
    rt->all = t;
    rt->tasks++;
    rt->live++;

    runtime_enqueue(rt, t);
    return t;
}

void task_free(task_t *t)
{
    runtime_t *rt = t->rt;

    if (t->all_prev != NULL)
        t->all_prev->all_next = t->all_next;
    else
        rt->all = t->all_next;
    if (t->all_next != NULL)
        t->all_next->all_prev = t->all_prev;
    rt->tasks--;
    free(t);
}

void task_finish(task_t *t)
{
    t->state = TASK_FINISHED;
    t->flags &= ~TASK_NOTIFIED;     /* a wake during the last poll is discarded */
    t->rt->live--;

    /* The frame may go away now, so no producer may keep a pointer into it. */
    future_drop_all(t);

    if (t->joiner != NULL) {
        future_t *join = t->joiner;

        t->joiner = NULL;
        future_complete(join, t->error);
        task_free(t);               /* the join settled: the handle is consumed */
    } else if (t->flags & TASK_DETACHED) {
        task_free(t);
    }
}

void *task_frame(task_t *t)
{
    return t->frame;
}

runtime_t *task_runtime(const task_t *t)
{
    return t->rt;
}

int task_error(const task_t *t)
{
    return t->error;
}

void task_detach(task_t *t)
{
    if (t == NULL)
        return;
    assert(!(t->flags & TASK_DETACHED) && "task handle consumed twice");
    assert(t->joiner == NULL && "task detached while a join on it is pending");
    t->flags |= TASK_DETACHED;
    if (t->state == TASK_FINISHED)
        task_free(t);
}

/* The joiner was cancelled or finished: the child forgets the join. */
static void join_drop(future_t *f, void *ctx)
{
    task_t *child = ctx;

    assert(child->joiner == f);
    child->joiner = NULL;
}

void task_join(task_t *t, task_t *child, future_t *f)
{
    assert(child != NULL && child != t && "a task cannot join itself");
    assert(!(child->flags & TASK_DETACHED) && "join on a consumed handle");
    assert(child->joiner == NULL && "a task can have only one pending join");

    if (!future_begin(t, f, join_drop, child))
        return;                     /* t is cancelled: it keeps the handle */

    if (child->state == TASK_FINISHED) {
        future_complete(f, child->error);
        task_free(child);           /* the join settled: the handle is consumed */
        return;
    }
    child->joiner = f;
}

void task_cancel(task_t *t, int reason)
{
    if (t == NULL)
        return;
    assert(reason != 0 && "task_cancel needs a non-zero reason");

    /* The first reason wins, and cleanup cannot be cancelled. */
    if (t->state == TASK_FINISHED || (t->flags & (TASK_CANCELLED | TASK_IN_CLEANUP)))
        return;

    t->flags |= TASK_CANCELLED;
    t->cancel_reason = reason;
    future_drop_all(t);             /* this part acts immediately */
    task_wake(t);                   /* the rest happens at its next resume point */
}

void task_wake(task_t *t)
{
    switch (t->state) {
    case TASK_IDLE:
        runtime_enqueue(t->rt, t);
        break;
    case TASK_RUNNING:
        t->flags |= TASK_NOTIFIED;
        break;
    case TASK_QUEUED:
    case TASK_FINISHED:
        break;
    }
}

/* Helpers for the macros in async.h. */

void async_yield(task_t *t)
{
    assert(t->state == TASK_RUNNING && "YIELD outside the task's own poll");
    assert(!(t->flags & TASK_IN_CLEANUP) && "YIELD after ASYNC_CLEANUP");
    if (!(t->flags & TASK_CANCELLED))
        task_wake(t);
}

void async_fail(task_t *t, int err)
{
    assert(err != 0 && "ASYNC_FAIL needs a non-zero error code");
    assert(!(t->flags & TASK_IN_CLEANUP) && "ASYNC_FAIL after ASYNC_CLEANUP");
    t->error = err;
}

void async_await_check(const task_t *t, const future_t *f)
{
    assert(t->state == TASK_RUNNING && "AWAIT outside the task's own poll");
    assert(!(t->flags & TASK_IN_CLEANUP) && "AWAIT after ASYNC_CLEANUP");
    if (f->state == FUTURE_IDLE) {
        /* Only a cancellation leaves a future idle at an AWAIT. */
        assert((t->flags & TASK_CANCELLED) &&
               "AWAIT on a future with no operation started");
    } else {
        assert(f->waiter == t && "AWAIT by a task that did not start the operation");
    }
}

bool task_take_budget(task_t *t)
{
    if (t->budget > 0) {
        t->budget--;
        return true;
    }
    task_wake(t);                   /* remembered: queued again after this poll */
    return false;
}

void async_take_cancel(task_t *t)
{
    /* A resume point is never reached after a failure, so no error is set yet. */
    assert(t->error == 0);
    t->error = t->cancel_reason;
}

void async_enter_cleanup(task_t *t)
{
    t->flags |= TASK_IN_CLEANUP;
}

_Noreturn void async_bad_resume(const task_t *t)
{
    fprintf(stderr,
            "toronto: a task was resumed at line %d, which is not a resume point of its\n"
            "poll function. Is an AWAIT or YIELD inside a nested switch statement?\n",
            t->resume);
    abort();
}
