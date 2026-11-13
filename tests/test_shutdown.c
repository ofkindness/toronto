/*
 * M2: runtime_free is a bounded shutdown. It cancels every task that is still
 * live with ECANCELED and polls until all of them have run their cleanup
 * sections.
 *
 * The frames are gone when runtime_free returns, so the tasks report through
 * static counters. The sanitizers check that nothing leaks.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/slot.h"
#include "tests/test.h"

static struct slot slot;
static int bodies_started;
static int cleanups;
static int cancelled_cleanups;  /* cleanups that saw ECANCELED */

static void reset(void)
{
    slot.waiting = NULL;
    slot.drops = 0;
    bodies_started = 0;
    cleanups = 0;
    cancelled_cleanups = 0;
}

static void count_cleanup(const task_t *t)
{
    cleanups++;
    if (task_error(t) == ECANCELED)
        cancelled_cleanups++;
}

struct blocked {
    future_t fut;
};

/* Waits on a slot that nobody fills. */
static poll_t blocked(task_t *t)
{
    struct blocked *f = task_frame(t);

    ASYNC_BEGIN(t);
    bodies_started++;
    slot_wait(t, &f->fut, &slot);
    AWAIT(t, &f->fut);
    ASYNC_CLEANUP(t);
    count_cleanup(t);
    ASYNC_END(t);
}

/* Starts its body, yields once, and finishes. */
static poll_t brief(task_t *t)
{
    ASYNC_BEGIN(t);
    bodies_started++;
    YIELD(t);
    ASYNC_CLEANUP(t);
    count_cleanup(t);
    ASYNC_END(t);
}

/* Like blocked, but its cleanup section spawns another task. */
static poll_t spawns_in_cleanup(task_t *t)
{
    struct blocked *f = task_frame(t);
    task_t *late;

    ASYNC_BEGIN(t);
    bodies_started++;
    slot_wait(t, &f->fut, &slot);
    AWAIT(t, &f->fut);
    ASYNC_CLEANUP(t);
    count_cleanup(t);
    late = task_spawn(task_runtime(t), brief, 0);
    CHECK(late != NULL);
    task_detach(late);
    ASYNC_END(t);
}

static task_t *spawn_detached(runtime_t *rt, task_fn fn, size_t frame_size)
{
    task_t *t = task_spawn(rt, fn, frame_size);

    CHECK(t != NULL);
    task_detach(t);
    return t;
}

/* Tasks that were never polled only run their cleanup sections. */
static void tasks_that_never_ran(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    reset();
    spawn_detached(rt, brief, 0);
    spawn_detached(rt, brief, 0);

    runtime_free(rt);
    CHECK(bodies_started == 0);
    CHECK(cleanups == 2 && cancelled_cleanups == 2);
}

/* A task that waits for something that never happens is cancelled out of it. */
static void a_task_that_is_stuck(void)
{
    runtime_t *rt = runtime_new();
    runtime_stats_t stats;

    CHECK(rt != NULL);
    reset();
    spawn_detached(rt, blocked, sizeof(struct blocked));

    CHECK(runtime_run(rt) == EDEADLK);
    CHECK(bodies_started == 1 && cleanups == 0);
    runtime_stats(rt, &stats);
    CHECK(stats.tasks == 1);

    runtime_free(rt);
    CHECK(slot.drops == 1 && slot.waiting == NULL);     /* the producer was told */
    CHECK(cleanups == 1 && cancelled_cleanups == 1);
}

/*
 * A task spawned during the shutdown starts cancelled, so its body never
 * runs and the shutdown still ends.
 */
static void a_task_spawned_during_the_shutdown(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    reset();
    spawn_detached(rt, spawns_in_cleanup, sizeof(struct blocked));

    CHECK(runtime_run(rt) == EDEADLK);
    CHECK(bodies_started == 1);

    runtime_free(rt);
    CHECK(bodies_started == 1);                         /* brief's body did not start */
    CHECK(cleanups == 2 && cancelled_cleanups == 2);
}

/* A finished task whose handle was never consumed is freed as well. */
static void a_handle_that_was_never_consumed(void)
{
    runtime_t *rt = runtime_new();
    runtime_stats_t stats;
    task_t *t;

    CHECK(rt != NULL);
    reset();
    t = task_spawn(rt, brief, 0);
    CHECK(t != NULL);

    CHECK(runtime_run(rt) == 0);
    CHECK(task_error(t) == 0);
    runtime_stats(rt, &stats);
    CHECK(stats.tasks == 1);                            /* the leak is visible */

    runtime_free(rt);
    CHECK(cleanups == 1 && cancelled_cleanups == 0);    /* it was not run again */
}

int main(void)
{
    tasks_that_never_ran();
    a_task_that_is_stuck();
    a_task_spawned_during_the_shutdown();
    a_handle_that_was_never_consumed();
    return 0;
}
