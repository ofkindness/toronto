/*
 * M1: rule 1. State that must survive a suspension lives in the frame.
 *
 * A local variable does not survive, because every poll is a new call of the
 * poll function. This test uses a local that is declared before ASYNC_BEGIN
 * and initialized there: it is set again on every poll, so what was added to
 * it before a YIELD is gone afterwards. A local declared inside the body is
 * worse, because the jump to the resume point skips its initialization and
 * leaves its value indeterminate. Reading that would be undefined behavior,
 * so the test does not do it.
 */
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/async.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/test.h"

struct counter {
    int i;
    int kept;           /* counts in the frame */
    int local_at_end;   /* what the local held when the loop was over */
    int first_poll_saw; /* a value the spawner wrote before the first poll */
    int given;
};

static poll_t counter(task_t *t)
{
    struct counter *f = task_frame(t);
    int local = 0;      /* set to 0 again on every poll */

    ASYNC_BEGIN(t);
    f->first_poll_saw = f->given;
    for (f->i = 0; f->i < 3; f->i++) {
        local++;
        f->kept++;
        YIELD(t);
    }
    f->local_at_end = local;
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

int main(void)
{
    runtime_t *rt = runtime_new();
    struct counter *f;
    task_t *t;

    CHECK(rt != NULL);
    t = task_spawn(rt, counter, sizeof(struct counter));
    CHECK(t != NULL);
    f = task_frame(t);

    /* The frame is zeroed, and it is aligned for any type. */
    CHECK(f->i == 0 && f->kept == 0 && f->local_at_end == 0 && f->given == 0);
    CHECK((uintptr_t)f % alignof(max_align_t) == 0);

    /* The spawner can fill the frame in before the first poll. */
    f->given = 42;

    CHECK(runtime_run(rt) == 0);

    /* The frame's address is stable, and the task is not freed until detached. */
    CHECK(task_frame(t) == f);
    CHECK(f->first_poll_saw == 42);
    CHECK(f->kept == 3);            /* the frame field counted all three rounds */
    CHECK(f->local_at_end == 0);    /* the local lost its count at every YIELD */

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
    return 0;
}
