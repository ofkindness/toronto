/*
 * M1: ASYNC_FAIL reaches the cleanup section with its code, and so does a
 * task that succeeds. A finished task stays counted until its handle is
 * consumed.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/test.h"

struct job {
    int fail_with;          /* 0: succeed */
    int steps;              /* how far the body got */
    int cleanups;           /* how often the cleanup section ran */
    int error_in_cleanup;   /* task_error() as seen from the cleanup section */
};

static poll_t job(task_t *t)
{
    struct job *f = task_frame(t);

    ASYNC_BEGIN(t);
    f->steps = 1;
    YIELD(t);
    f->steps = 2;
    if (f->fail_with != 0)
        ASYNC_FAIL(t, f->fail_with);
    f->steps = 3;
    YIELD(t);
    f->steps = 4;
    ASYNC_CLEANUP(t);
    f->cleanups++;
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

static task_t *spawn_job(runtime_t *rt, int fail_with)
{
    task_t *t = task_spawn(rt, job, sizeof(struct job));

    CHECK(t != NULL);
    ((struct job *)task_frame(t))->fail_with = fail_with;
    return t;
}

int main(void)
{
    runtime_t *rt = runtime_new();
    runtime_stats_t stats;
    struct job *ok_frame, *bad_frame;
    task_t *ok, *bad;

    CHECK(rt != NULL);
    ok = spawn_job(rt, 0);
    bad = spawn_job(rt, EIO);
    ok_frame = task_frame(ok);
    bad_frame = task_frame(bad);

    CHECK(runtime_run(rt) == 0);

    /* Success: the whole body ran, then cleanup, and the error is 0. */
    CHECK(ok_frame->steps == 4);
    CHECK(ok_frame->cleanups == 1);
    CHECK(ok_frame->error_in_cleanup == 0);
    CHECK(task_error(ok) == 0);

    /* Failure: the body stopped at ASYNC_FAIL, and cleanup saw the code. */
    CHECK(bad_frame->steps == 2);
    CHECK(bad_frame->cleanups == 1);
    CHECK(bad_frame->error_in_cleanup == EIO);
    CHECK(task_error(bad) == EIO);

    /* Both have finished, but their handles are not consumed yet. */
    runtime_stats(rt, &stats);
    CHECK(stats.tasks == 2);
    task_detach(ok);
    runtime_stats(rt, &stats);
    CHECK(stats.tasks == 1);
    task_detach(bad);
    task_detach(NULL);      /* does nothing */

    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
    return 0;
}
