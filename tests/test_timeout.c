/*
 * M3: operation timeouts, under virtual time.
 *
 *   - a join with a timeout fails with ETIMEDOUT, and the task can then join
 *     the same child again;
 *   - a timed-out operation is dropped: its producer is told to forget it;
 *   - with AWAIT_TRY the task carries on, with AWAIT it fails;
 *   - a timeout that is not needed is disarmed when the future settles;
 *   - setting it again replaces it, and it does nothing on a future that is
 *     settled or idle.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "runtime/timer.h"
#include "tests/slot.h"
#include "tests/test.h"

static struct slot slot;

static runtime_t *virtual_runtime(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    runtime_set_virtual_time(rt);
    slot.waiting = NULL;
    slot.drops = 0;
    return rt;
}

static size_t timers_now(task_t *t)
{
    runtime_stats_t stats;

    runtime_stats(task_runtime(t), &stats);
    return stats.timers;
}

/* A child that sleeps for 100 ms. */
struct napper {
    future_t nap;
};

static poll_t napper(task_t *t)
{
    struct napper *f = task_frame(t);

    ASYNC_BEGIN(t);
    sleep_ms(t, &f->nap, 100);
    AWAIT(t, &f->nap);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

struct parent {
    task_t  *child;
    future_t joined;
    int      first_error;
    uint64_t first_at;
    int      second_done;
    intptr_t second_value;
    uint64_t second_at;
};

/* Joins its child with a 30 ms limit, then joins it again without one. */
static poll_t parent(task_t *t)
{
    struct parent *f = task_frame(t);

    ASYNC_BEGIN(t);
    f->child = task_spawn(task_runtime(t), napper, sizeof(struct napper));
    if (f->child == NULL)
        ASYNC_FAIL(t, ENOMEM);

    task_join(t, f->child, &f->joined);
    future_set_timeout(&f->joined, 30);
    AWAIT_TRY(t, &f->joined);
    f->first_error = future_error(&f->joined);
    f->first_at = runtime_now_ms(task_runtime(t));
    if (f->first_error == 0)
        f->child = NULL;

    /* The join timed out, so it did not consume the handle: join again. */
    task_join(t, f->child, &f->joined);
    AWAIT(t, &f->joined);
    f->child = NULL;
    f->second_done = 1;
    f->second_value = future_value(&f->joined);
    f->second_at = runtime_now_ms(task_runtime(t));

    ASYNC_CLEANUP(t);
    task_cancel(f->child, ECANCELED);
    task_detach(f->child);
    ASYNC_END(t);
}

static void join_with_a_timeout(void)
{
    runtime_t *rt = virtual_runtime();
    task_t *p = task_spawn(rt, parent, sizeof(struct parent));
    struct parent *f;

    CHECK(p != NULL);
    f = task_frame(p);

    CHECK(runtime_run(rt) == 0);
    CHECK(f->first_error == ETIMEDOUT && f->first_at == 30);
    CHECK(f->second_done == 1 && f->second_value == 0 && f->second_at == 100);
    CHECK(task_error(p) == 0);

    task_detach(p);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

struct waiter {
    future_t fut;
    future_t nap;
    int      use_await;         /* AWAIT instead of AWAIT_TRY */
    int      set_twice;         /* a 10 ms limit, replaced by the 50 ms one */

    int      seen_error;
    intptr_t value;
    uint64_t settled_at;
    size_t   timers_while_waiting;
    size_t   timers_after;
    int      carried_on;
    int      error_in_cleanup;
};

/* Waits on the slot for at most 50 ms, then does one more operation. */
static poll_t waiter(task_t *t)
{
    struct waiter *f = task_frame(t);

    ASYNC_BEGIN(t);
    slot_wait(t, &f->fut, &slot);
    if (f->set_twice)
        future_set_timeout(&f->fut, 10);
    future_set_timeout(&f->fut, 50);
    f->timers_while_waiting = timers_now(t);

    if (f->use_await) {
        AWAIT(t, &f->fut);
    } else {
        AWAIT_TRY(t, &f->fut);
    }
    f->seen_error = future_error(&f->fut);
    f->value = future_value(&f->fut);
    f->settled_at = runtime_now_ms(task_runtime(t));
    f->timers_after = timers_now(t);

    /* The task is not cancelled, so it can go on to something else. */
    sleep_ms(t, &f->nap, 5);
    AWAIT(t, &f->nap);
    f->carried_on = 1;

    ASYNC_CLEANUP(t);
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/* Fills the slot after 10 ms. */
struct filler {
    future_t nap;
};

static poll_t filler(task_t *t)
{
    struct filler *f = task_frame(t);

    ASYNC_BEGIN(t);
    sleep_ms(t, &f->nap, 10);
    AWAIT(t, &f->nap);
    CHECK(slot_put(&slot, 7));
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static struct waiter run_waiter(struct waiter setup, int with_filler, uint64_t *ended_at)
{
    runtime_t *rt = virtual_runtime();
    task_t *w = task_spawn(rt, waiter, sizeof(struct waiter));
    struct waiter result;

    CHECK(w != NULL);
    *(struct waiter *)task_frame(w) = setup;
    if (with_filler) {
        task_t *f = task_spawn(rt, filler, sizeof(struct filler));

        CHECK(f != NULL);
        task_detach(f);
    }

    CHECK(runtime_run(rt) == 0);

    result = *(struct waiter *)task_frame(w);
    CHECK(task_error(w) == result.error_in_cleanup);
    *ended_at = runtime_now_ms(rt);
    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
    return result;
}

/* Does nothing on a future that is settled or idle. */
struct idler {
    future_t settled;
    future_t idle;
    size_t   timers;
};

static poll_t idler(task_t *t)
{
    struct idler *f = task_frame(t);

    ASYNC_BEGIN(t);
    ready(t, &f->settled, 1);
    future_set_timeout(&f->settled, 10);
    future_set_timeout(&f->idle, 10);
    f->timers = timers_now(t);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void nothing_to_time_out(void)
{
    runtime_t *rt = virtual_runtime();
    task_t *t = task_spawn(rt, idler, sizeof(struct idler));

    CHECK(t != NULL);
    CHECK(runtime_run(rt) == 0);
    CHECK(((struct idler *)task_frame(t))->timers == 0);
    CHECK(runtime_now_ms(rt) == 0);

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    struct waiter w;
    uint64_t ended_at;

    join_with_a_timeout();

    /* Nobody fills the slot: the operation times out and is dropped. */
    w = run_waiter((struct waiter){ 0 }, 0, &ended_at);
    CHECK(w.timers_while_waiting == 1);
    CHECK(w.seen_error == ETIMEDOUT && w.settled_at == 50);
    CHECK(slot.drops == 1 && slot.waiting == NULL);     /* the producer was told */
    CHECK(w.timers_after == 0);
    CHECK(w.carried_on == 1 && w.error_in_cleanup == 0);
    CHECK(ended_at == 55);

    /* The same with AWAIT: the timeout becomes the task's error. */
    w = run_waiter((struct waiter){ .use_await = 1 }, 0, &ended_at);
    CHECK(w.carried_on == 0 && w.error_in_cleanup == ETIMEDOUT);
    CHECK(ended_at == 50);

    /* The slot is filled in time: the timeout is disarmed and never fires. */
    w = run_waiter((struct waiter){ 0 }, 1, &ended_at);
    CHECK(w.seen_error == 0 && w.value == 7 && w.settled_at == 10);
    CHECK(slot.drops == 0);
    CHECK(w.timers_after == 0);
    CHECK(w.carried_on == 1 && w.error_in_cleanup == 0);
    CHECK(ended_at == 15);

    /* Setting it again replaces it: one entry, and the later limit counts. */
    w = run_waiter((struct waiter){ .set_twice = 1 }, 0, &ended_at);
    CHECK(w.timers_while_waiting == 1);
    CHECK(w.seen_error == ETIMEDOUT && w.settled_at == 50);

    nothing_to_time_out();
    return 0;
}
