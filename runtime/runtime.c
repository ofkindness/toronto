#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include "runtime/internal.h"

runtime_t *runtime_new(void)
{
    runtime_t *rt = calloc(1, sizeof *rt);

    if (rt == NULL)
        errno = ENOMEM;
    return rt;
}

void runtime_free(runtime_t *rt)
{
    if (rt == NULL)
        return;
    assert(!rt->running && "runtime_free called from inside runtime_run");
    while (rt->all != NULL)
        task_free(rt->all);
    free(rt);
}

void runtime_stats(const runtime_t *rt, runtime_stats_t *out)
{
    out->tasks = rt->tasks;
    out->timers = 0;
    out->fds = 0;
}

void runtime_enqueue(runtime_t *rt, task_t *t)
{
    assert(t->state == TASK_IDLE || t->state == TASK_RUNNING);
    t->state = TASK_QUEUED;
    t->next = NULL;
    if (rt->queue_tail != NULL)
        rt->queue_tail->next = t;
    else
        rt->queue_head = t;
    rt->queue_tail = t;
}

/* One poll of one task, and the state change that its result asks for. */
static void poll_task(runtime_t *rt, task_t *t)
{
    t->state = TASK_RUNNING;

    if (t->fn(t) == POLL_READY) {
        t->state = TASK_FINISHED;
        t->flags &= ~TASK_NOTIFIED;     /* a wake during the last poll is discarded */
        rt->live--;
        if (t->flags & TASK_DETACHED)
            task_free(t);
        return;
    }

    if (t->flags & TASK_NOTIFIED) {     /* woken during the poll, e.g. by YIELD */
        t->flags &= ~TASK_NOTIFIED;
        t->state = TASK_IDLE;
        runtime_enqueue(rt, t);
    } else {
        t->state = TASK_IDLE;
    }
}

int runtime_run(runtime_t *rt)
{
    int result = 0;

    assert(!rt->running && "runtime_run called from inside a task");
    rt->running = true;

    while (rt->live > 0) {
        /*
         * M1 has no timers and no I/O, so an empty run queue means that
         * nothing can ever wake the tasks that are still live.
         */
        if (rt->queue_head == NULL) {
            result = EDEADLK;
            break;
        }

        /*
         * One pass polls only the tasks that were queued when it began. A
         * task that is queued during the pass, by a spawn or by a YIELD,
         * waits for the next pass.
         */
        task_t *batch = rt->queue_head;
        rt->queue_head = NULL;
        rt->queue_tail = NULL;

        while (batch != NULL) {
            task_t *t = batch;

            batch = t->next;
            t->next = NULL;
            poll_task(rt, t);
        }
    }

    rt->running = false;
    return result;
}
