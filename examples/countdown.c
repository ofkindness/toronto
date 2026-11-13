/*
 * M1: two countdowns that take turns.
 *
 * Each task prints one number, then yields so that the other one can run.
 * Nothing here waits for time or I/O yet: the tasks only pass control back
 * and forth through the run queue.
 *
 * Expected output:
 *
 *     slow 3
 *     fast 2
 *     slow 2
 *     fast 1
 *     slow 1
 *     fast done
 *     slow done
 */
#include <stdio.h>

#include "runtime/async.h"
#include "runtime/runtime.h"
#include "runtime/task.h"

struct countdown {
    const char *name;
    int         n;      /* survives YIELD: it lives in the frame */
};

static poll_t countdown(task_t *t)
{
    struct countdown *f = task_frame(t);    /* recomputed on every poll: safe */

    ASYNC_BEGIN(t);
    for (; f->n > 0; f->n--) {
        printf("%s %d\n", f->name, f->n);
        YIELD(t);
    }
    ASYNC_CLEANUP(t);                       /* success and failure end here */
    printf("%s done\n", f->name);
    ASYNC_END(t);
}

static int spawn_countdown(runtime_t *rt, const char *name, int from)
{
    task_t *t = task_spawn(rt, countdown, sizeof(struct countdown));
    struct countdown *f;

    if (t == NULL)
        return -1;
    f = task_frame(t);
    f->name = name;
    f->n = from;
    task_detach(t);     /* nobody waits for the result */
    return 0;
}

int main(void)
{
    runtime_t *rt = runtime_new();
    int err;

    if (rt == NULL)
        return 1;
    if (spawn_countdown(rt, "slow", 3) != 0 || spawn_countdown(rt, "fast", 2) != 0) {
        runtime_free(rt);
        return 1;
    }
    err = runtime_run(rt);                  /* returns when no task is left */
    runtime_free(rt);
    return err != 0;
}
