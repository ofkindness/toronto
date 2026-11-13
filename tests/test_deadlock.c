/*
 * M1: a task that returns POLL_PENDING without arranging a wake is never
 * polled again. When nothing else is live, runtime_run returns EDEADLK
 * instead of spinning.
 *
 * This test cannot end with CHECK_NOTHING_LEFT: in M1 the stuck task can only
 * be freed by runtime_free. The sanitizers check that nothing leaks.
 */
#include <errno.h>

#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/test.h"

static int polls;

/* Written without the macros, so nothing wakes it. */
static poll_t stuck(task_t *t)
{
    (void)t;
    polls++;
    return POLL_PENDING;
}

int main(void)
{
    runtime_t *rt = runtime_new();
    runtime_stats_t stats;
    task_t *t;

    CHECK(rt != NULL);

    /* With no task at all, the loop has nothing to do. */
    CHECK(runtime_run(rt) == 0);

    t = task_spawn(rt, stuck, 0);
    CHECK(t != NULL);
    task_detach(t);

    CHECK(runtime_run(rt) == EDEADLK);
    CHECK(polls == 1);

    /* Running again changes nothing: the task is still not queued. */
    CHECK(runtime_run(rt) == EDEADLK);
    CHECK(polls == 1);

    runtime_stats(rt, &stats);
    CHECK(stats.tasks == 1);

    runtime_free(rt);
    return 0;
}
