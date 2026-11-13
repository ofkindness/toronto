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
    t->flags |= TASK_DETACHED;
    if (t->state == TASK_FINISHED)
        task_free(t);
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
    task_wake(t);
}

void async_fail(task_t *t, int err)
{
    assert(err != 0 && "ASYNC_FAIL needs a non-zero error code");
    assert(!(t->flags & TASK_IN_CLEANUP) && "ASYNC_FAIL after ASYNC_CLEANUP");
    t->error = err;
}

void async_enter_cleanup(task_t *t)
{
    t->flags |= TASK_IN_CLEANUP;
}

_Noreturn void async_bad_resume(const task_t *t)
{
    fprintf(stderr,
            "toronto: a task was resumed at line %d, which is not a resume point of its\n"
            "poll function. Is a YIELD inside a nested switch statement?\n",
            t->resume);
    abort();
}
