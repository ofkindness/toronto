/*
 * M2: cancellation.
 *
 *   - before the first poll, and while awaiting, it reaches cleanup with the
 *     reason, and the producer's drop hook runs;
 *   - a future that settled before the cancel is still delivered;
 *   - a cancelled task cannot start operations;
 *   - a late cancel changes nothing;
 *   - the first reason wins, and cancelling a finished task does nothing;
 *   - YIELD is a resume point, and a task can cancel itself.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/slot.h"
#include "tests/test.h"

static struct slot slot;
static int drops_right_after_cancel;    /* slot.drops as the canceller saw it */

static void slot_reset(void)
{
    slot.waiting = NULL;
    slot.drops = 0;
    drops_right_after_cancel = -1;
}

struct waiter {
    future_t fut;
    int      polls;
    int      stage;             /* how far the body got */
    intptr_t value;
    int      second_started;    /* did the second operation reach the slot? */
    int      cleanups;
    int      error_in_cleanup;
};

/* Waits on the slot twice. */
static poll_t waiter(task_t *t)
{
    struct waiter *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    f->stage = 1;
    slot_wait(t, &f->fut, &slot);
    AWAIT(t, &f->fut);
    f->stage = 2;
    f->value = future_value(&f->fut);
    slot_wait(t, &f->fut, &slot);
    f->second_started = (slot.waiting == &f->fut);
    AWAIT(t, &f->fut);
    f->stage = 3;
    ASYNC_CLEANUP(t);
    f->cleanups++;
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/* Waits on the slot once, then finishes without another resume point. */
static poll_t finisher(task_t *t)
{
    struct waiter *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    f->stage = 1;
    slot_wait(t, &f->fut, &slot);
    AWAIT(t, &f->fut);
    f->stage = 2;
    f->value = future_value(&f->fut);
    ASYNC_CLEANUP(t);
    f->cleanups++;
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/* Yields forever. Only a cancel ends it. */
static poll_t spinner(task_t *t)
{
    struct waiter *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    for (;;) {
        f->stage++;
        YIELD(t);
    }
    ASYNC_CLEANUP(t);
    f->cleanups++;
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/* Cancels itself, then runs on to its next resume point. */
static poll_t quitter(task_t *t)
{
    struct waiter *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    f->stage = 1;
    task_cancel(t, EINTR);
    f->stage = 2;               /* a cancel is not a jump: this line still runs */
    YIELD(t);
    f->stage = 3;
    ASYNC_CLEANUP(t);
    f->cleanups++;
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

struct canceller {
    task_t *victim;     /* borrowed: main owns the handle and outlives this task */
    int     passes;     /* yield this often first */
    int     put_first;  /* fill the slot in the same poll, before the cancel */
    int     reason;
    int     i;
};

static poll_t canceller(task_t *t)
{
    struct canceller *f = task_frame(t);

    ASYNC_BEGIN(t);
    for (f->i = 0; f->i < f->passes; f->i++)
        YIELD(t);
    if (f->put_first)
        CHECK(slot_put(&slot, 7));
    task_cancel(f->victim, f->reason);
    drops_right_after_cancel = slot.drops;
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void spawn_canceller(runtime_t *rt, task_t *victim, int passes, int put_first,
                            int reason)
{
    task_t *t = task_spawn(rt, canceller, sizeof(struct canceller));
    struct canceller *f;

    CHECK(t != NULL);
    f = task_frame(t);
    f->victim = victim;
    f->passes = passes;
    f->put_first = put_first;
    f->reason = reason;
    task_detach(t);
}

static void cancel_before_the_first_poll(void)
{
    runtime_t *rt = runtime_new();
    struct waiter *f;
    task_t *w;

    CHECK(rt != NULL);
    slot_reset();
    w = task_spawn(rt, waiter, sizeof(struct waiter));
    CHECK(w != NULL);
    f = task_frame(w);

    task_cancel(NULL, ECANCELED);       /* does nothing */
    task_cancel(w, ECANCELED);
    task_cancel(w, EINTR);              /* the first reason wins */

    CHECK(runtime_run(rt) == 0);
    CHECK(f->polls == 1);
    CHECK(f->stage == 0);               /* the body never started */
    CHECK(f->cleanups == 1 && f->error_in_cleanup == ECANCELED);
    CHECK(task_error(w) == ECANCELED);

    task_cancel(w, EIO);                /* finished: does nothing */
    CHECK(task_error(w) == ECANCELED);

    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

static void cancel_while_awaiting(void)
{
    runtime_t *rt = runtime_new();
    struct waiter *f;
    task_t *w;

    CHECK(rt != NULL);
    slot_reset();
    w = task_spawn(rt, waiter, sizeof(struct waiter));
    CHECK(w != NULL);
    f = task_frame(w);
    spawn_canceller(rt, w, 1, 0, EINTR);

    CHECK(runtime_run(rt) == 0);
    CHECK(f->polls == 2);
    CHECK(f->stage == 1);               /* it never got past the first AWAIT */
    CHECK(f->cleanups == 1 && f->error_in_cleanup == EINTR);
    CHECK(task_error(w) == EINTR);

    /*
     * The producer was told to forget the future, which stayed unsettled. It
     * was told by task_cancel itself, not later when the task finished.
     */
    CHECK(drops_right_after_cancel == 1);
    CHECK(slot.drops == 1 && slot.waiting == NULL);
    CHECK(!future_settled(&f->fut));

    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/*
 * The slot is filled and the task cancelled in the same poll of another
 * task. The value exists, so the task still receives it. Its next operation
 * does nothing, and the AWAIT after that takes the cancel.
 */
static void settled_future_is_delivered(void)
{
    runtime_t *rt = runtime_new();
    struct waiter *f;
    task_t *w;

    CHECK(rt != NULL);
    slot_reset();
    w = task_spawn(rt, waiter, sizeof(struct waiter));
    CHECK(w != NULL);
    f = task_frame(w);
    spawn_canceller(rt, w, 1, 1, EINTR);

    CHECK(runtime_run(rt) == 0);
    CHECK(f->polls == 2);
    CHECK(f->stage == 2 && f->value == 7);      /* delivered */
    CHECK(f->second_started == 0);              /* a cancelled task starts nothing */
    CHECK(slot.waiting == NULL && slot.drops == 0);
    CHECK(f->cleanups == 1 && f->error_in_cleanup == EINTR);
    CHECK(task_error(w) == EINTR);

    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/* The same, but the task has no resume point left: the cancel comes too late. */
static void late_cancel_changes_nothing(void)
{
    runtime_t *rt = runtime_new();
    struct waiter *f;
    task_t *w;

    CHECK(rt != NULL);
    slot_reset();
    w = task_spawn(rt, finisher, sizeof(struct waiter));
    CHECK(w != NULL);
    f = task_frame(w);
    spawn_canceller(rt, w, 1, 1, EINTR);

    CHECK(runtime_run(rt) == 0);
    CHECK(f->stage == 2 && f->value == 7);
    CHECK(f->cleanups == 1 && f->error_in_cleanup == 0);
    CHECK(task_error(w) == 0);

    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

static void yield_is_a_resume_point(void)
{
    runtime_t *rt = runtime_new();
    struct waiter *f;
    task_t *w;

    CHECK(rt != NULL);
    w = task_spawn(rt, spinner, sizeof(struct waiter));
    CHECK(w != NULL);
    f = task_frame(w);
    spawn_canceller(rt, w, 2, 0, ECANCELED);

    /* Three rounds, then the cancel arrives while the spinner is queued. */
    CHECK(runtime_run(rt) == 0);
    CHECK(f->stage == 3 && f->polls == 4);
    CHECK(f->cleanups == 1 && f->error_in_cleanup == ECANCELED);

    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

static void a_task_can_cancel_itself(void)
{
    runtime_t *rt = runtime_new();
    struct waiter *f;
    task_t *w;

    CHECK(rt != NULL);
    w = task_spawn(rt, quitter, sizeof(struct waiter));
    CHECK(w != NULL);
    f = task_frame(w);

    CHECK(runtime_run(rt) == 0);
    CHECK(f->polls == 1);               /* the YIELD did not suspend */
    CHECK(f->stage == 2);
    CHECK(f->cleanups == 1 && f->error_in_cleanup == EINTR);

    task_detach(w);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    cancel_before_the_first_poll();
    cancel_while_awaiting();
    settled_future_is_delivered();
    late_cancel_changes_nothing();
    yield_is_a_resume_point();
    a_task_can_cancel_itself();
    return 0;
}
