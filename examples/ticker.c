/*
 * M3: a ticker with a deadline.
 *
 * The task prints a tick every 100 ms and wants to do so five times. Its
 * deadline of 250 ms cancels it with ETIMEDOUT during the third sleep, which
 * would have ended at 300 ms.
 *
 * Expected output (the last line carries the OS's text for ETIMEDOUT):
 *
 *     tick 0
 *     tick 1
 *     tick 2
 *     ticker finished: Operation timed out
 *
 * In real time this depends on the loop running less than 50 ms late in
 * total. tests/test_ticker.c runs the same task under virtual time, where
 * the output is exact.
 */
#include <stdio.h>
#include <string.h>

#include "runtime/async.h"
#include "runtime/runtime.h"
#include "runtime/timer.h"

struct ticker {
    int      i;       /* survives AWAIT: lives in the frame */
    future_t tick;
};

static poll_t ticker(task_t *t)
{
    struct ticker *f = task_frame(t);   /* recomputed on every poll: safe */

    ASYNC_BEGIN(t);
    task_set_deadline(t, 250);          /* cancel with ETIMEDOUT after 250 ms */
    for (f->i = 0; f->i < 5; f->i++) {
        printf("tick %d\n", f->i);
        sleep_ms(t, &f->tick, 100);     /* start the operation ... */
        AWAIT(t, &f->tick);             /* ... then suspend until it settles */
    }
    ASYNC_CLEANUP(t);                   /* success, failure and cancellation end here */
    printf("ticker finished: %s\n", task_error(t) ? strerror(task_error(t)) : "ok");
    ASYNC_END(t);
}

int main(void)
{
    runtime_t *rt = runtime_new();
    task_t *t;
    int err;

    if (rt == NULL)
        return 1;
    t = task_spawn(rt, ticker, sizeof(struct ticker));
    if (t == NULL) {
        runtime_free(rt);
        return 1;
    }
    task_detach(t);
    err = runtime_run(rt);              /* returns when no task is left */
    runtime_free(rt);
    return err != 0;
}
