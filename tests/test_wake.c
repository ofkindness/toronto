/*
 * M2: the wake rules.
 *
 *   - waking a task that is already queued does nothing: two wakes, one poll;
 *   - a wake during a poll is remembered: the task is queued again if that
 *     poll returns POLL_PENDING;
 *   - a wake during the poll that finishes the task is discarded.
 */
#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/slot.h"
#include "tests/test.h"

static struct slot slot_a, slot_b;
static int polls;           /* polls of the task under test */

/* Starts two operations, then awaits them one after the other. */
struct two {
    future_t a, b;
};

static poll_t waits_for_two(task_t *t)
{
    struct two *f = task_frame(t);

    polls++;
    ASYNC_BEGIN(t);
    slot_wait(t, &f->a, &slot_a);
    slot_wait(t, &f->b, &slot_b);
    AWAIT(t, &f->a);
    AWAIT(t, &f->b);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/* Yields `passes` times, then fills slot A if asked, then slot B. */
struct filler {
    int i;
    int passes;
    int fill_a;
};

static poll_t filler(task_t *t)
{
    struct filler *f = task_frame(t);

    ASYNC_BEGIN(t);
    for (f->i = 0; f->i < f->passes; f->i++)
        YIELD(t);
    if (f->fill_a)
        CHECK(slot_put(&slot_a, 1));
    CHECK(slot_put(&slot_b, 2));
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void spawn_filler(runtime_t *rt, int passes, int fill_a)
{
    task_t *t = task_spawn(rt, filler, sizeof(struct filler));
    struct filler *f;

    CHECK(t != NULL);
    f = task_frame(t);
    f->passes = passes;
    f->fill_a = fill_a;
    task_detach(t);
}

static void spawn_detached(runtime_t *rt, task_fn fn, size_t frame_size)
{
    task_t *t = task_spawn(rt, fn, frame_size);

    CHECK(t != NULL);
    task_detach(t);
}

/*
 * The filler settles both futures in one poll, so the waiter is woken twice
 * while it sits on the run queue. It is polled once for that: twice in all.
 */
static void double_wake_polls_once(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    polls = 0;
    spawn_detached(rt, waits_for_two, sizeof(struct two));
    spawn_filler(rt, 1, 1);

    CHECK(runtime_run(rt) == 0);
    CHECK(polls == 2);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/* Settles its own first future during its poll, then waits for the second. */
static poll_t wakes_itself(task_t *t)
{
    struct two *f = task_frame(t);

    polls++;
    ASYNC_BEGIN(t);
    slot_wait(t, &f->a, &slot_a);
    CHECK(slot_put(&slot_a, 1));        /* wakes this task while it is being polled */
    slot_wait(t, &f->b, &slot_b);
    AWAIT(t, &f->a);                    /* already settled: passes */
    AWAIT(t, &f->b);                    /* pending: suspends */
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/*
 * Poll 1 ends in POLL_PENDING with a wake remembered, so the task is queued
 * again. Poll 2 finds B still pending and suspends for real. Poll 3 comes
 * after the filler settles B, several passes later.
 */
static void wake_during_poll_queues_again(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    polls = 0;
    spawn_detached(rt, wakes_itself, sizeof(struct two));
    spawn_filler(rt, 3, 0);

    CHECK(runtime_run(rt) == 0);
    CHECK(polls == 3);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/* Wakes itself and finishes in the same poll. */
static poll_t wakes_itself_and_finishes(task_t *t)
{
    struct two *f = task_frame(t);

    polls++;
    ASYNC_BEGIN(t);
    slot_wait(t, &f->a, &slot_a);
    CHECK(slot_put(&slot_a, 1));
    AWAIT(t, &f->a);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void wake_is_discarded_when_the_task_finishes(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    polls = 0;
    spawn_detached(rt, wakes_itself_and_finishes, sizeof(struct two));

    CHECK(runtime_run(rt) == 0);
    CHECK(polls == 1);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    double_wake_polls_once();
    wake_during_poll_queues_again();
    wake_is_discarded_when_the_task_finishes();
    return 0;
}
