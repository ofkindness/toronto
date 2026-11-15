#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#include "runtime/internal.h"

enum { MAX_EVENTS = 64 };

runtime_t *runtime_new(void)
{
    runtime_t *rt = calloc(1, sizeof *rt);

    if (rt == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    rt->poller = poller_new();
    if (rt->poller == NULL) {
        int err = errno;

        free(rt);
        errno = err;
        return NULL;
    }
    return rt;
}

void runtime_free(runtime_t *rt)
{
    if (rt == NULL)
        return;
    assert(!rt->running && "runtime_free called from inside runtime_run");

    /*
     * The bounded shutdown. Cancel every task that is still live, then poll
     * until all of them have finished. This ends without waiting for
     * anything, because a cancelled task cannot start an operation and a
     * cleanup section cannot suspend.
     */
    if (rt->live > 0) {
        rt->shutting_down = true;
        for (task_t *t = rt->all; t != NULL; t = t->all_next)
            task_cancel(t, ECANCELED);
        (void)runtime_run(rt);
    }

    /*
     * What is left: finished tasks whose handle was never consumed, and any
     * task that broke the contract by suspending without arranging a wake.
     * Such a task is freed without running its cleanup section.
     */
    while (rt->all != NULL)
        task_free(rt->all);

    poller_free(rt->poller);
    free(rt->heap);
    free(rt);
}

void runtime_stats(const runtime_t *rt, runtime_stats_t *out)
{
    out->tasks = rt->tasks;
    out->timers = rt->heap_len;
    out->fds = 0;
}

uint64_t runtime_now_ms(const runtime_t *rt)
{
    struct timespec ts;

    if (rt->virtual_time)
        return rt->virtual_now;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        abort();                /* cannot happen with a valid clock id */
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void runtime_set_virtual_time(runtime_t *rt)
{
    assert(rt->heap_len == 0 && "set virtual time before any timer is armed");
    rt->virtual_time = true;
    rt->virtual_now = 0;
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
    t->budget = TASK_BUDGET;

    if (t->fn(t) == POLL_READY) {
        task_finish(t);
        return;
    }

    if (t->flags & TASK_NOTIFIED) {     /* woken during the poll, e.g. by YIELD */
        t->flags &= ~TASK_NOTIFIED;
        runtime_enqueue(rt, t);
    } else {
        t->state = TASK_IDLE;
    }
}

/*
 * One pass polls only the tasks that were queued when it began. A task that
 * is queued during the pass, by a spawn, a wake or a YIELD, waits for the
 * next pass, so timers and I/O are checked in between.
 */
static void poll_queued_tasks(runtime_t *rt)
{
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

/*
 * How long the loop may wait for events, in milliseconds: not at all while
 * tasks are queued, otherwise until the earliest timer. Returns false if
 * nothing can ever wake the tasks that are still live.
 */
static bool wait_timeout(const runtime_t *rt, int *timeout_ms)
{
    uint64_t deadline, now;

    if (rt->queue_head != NULL) {
        *timeout_ms = 0;
        return true;
    }
    /* The shutdown never waits: every task it can finish is on the run queue. */
    if (rt->shutting_down)
        return false;
    if (!timer_next(rt, &deadline))
        return false;           /* M4 adds: wait without limit while fds are armed */

    /*
     * The clock counts whole milliseconds, so this wait never ends before
     * the deadline, and the loop does not spin on a fraction of one.
     */
    now = runtime_now_ms(rt);
    if (deadline <= now)
        *timeout_ms = 0;
    else if (deadline - now > (uint64_t)INT_MAX)
        *timeout_ms = INT_MAX;
    else
        *timeout_ms = (int)(deadline - now);
    return true;
}

/*
 * Waits for events. Under virtual time the loop never sleeps: when it would
 * wait for a timer and nothing else has happened, the clock jumps to that
 * timer's deadline.
 */
static int wait_for_events(runtime_t *rt, poller_event_t *events, int timeout_ms)
{
    uint64_t deadline;
    int n;

    if (!rt->virtual_time || timeout_ms == 0)
        return poller_wait(rt->poller, events, MAX_EVENTS, timeout_ms);

    n = poller_wait(rt->poller, events, MAX_EVENTS, 0);
    if (n == 0 && timer_next(rt, &deadline) && deadline > rt->virtual_now)
        rt->virtual_now = deadline;
    return n;
}

int runtime_run(runtime_t *rt)
{
    poller_event_t events[MAX_EVENTS];
    int result = 0;

    assert(!rt->running && "runtime_run called from inside a task");
    rt->running = true;

    while (rt->live > 0) {
        int timeout_ms, n;

        poll_queued_tasks(rt);
        if (rt->live == 0)
            break;

        if (!wait_timeout(rt, &timeout_ms)) {
            result = EDEADLK;   /* live tasks, but nothing can wake them */
            break;
        }
        n = wait_for_events(rt, events, timeout_ms);
        if (n < 0) {
            result = errno;
            break;
        }

        timer_fire_due(rt, runtime_now_ms(rt));

        /* M4: the reactor handles the n events here. No fd is armed before that. */
    }

    rt->running = false;
    return result;
}
