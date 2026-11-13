/*
 * M2: a consumer awaits a future that a producer task settles.
 *
 *   - a completed future delivers its value;
 *   - a failed future propagates through AWAIT to the cleanup section;
 *   - AWAIT_TRY lets the task handle the failure and carry on;
 *   - a settled future can be reused for the next operation;
 *   - a task that finishes with an operation still pending drops it.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/slot.h"
#include "tests/test.h"

static struct slot slot;    /* shared between the two tasks: outside every frame */

struct consumer {
    future_t got;
    int      polls;
    int      seen_error;        /* AWAIT_TRY only: what future_error reported */
    intptr_t value;
    int      reached_end;
    int      error_in_cleanup;
};

/* Waits with AWAIT: a failure jumps to cleanup. */
static poll_t consumer(task_t *t)
{
    struct consumer *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    slot_wait(t, &f->got, &slot);
    AWAIT(t, &f->got);
    f->value = future_value(&f->got);
    f->reached_end = 1;
    ASYNC_CLEANUP(t);
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/* Waits with AWAIT_TRY: it looks at the failure, then starts a new operation. */
static poll_t patient(task_t *t)
{
    struct consumer *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    slot_wait(t, &f->got, &slot);
    AWAIT_TRY(t, &f->got);
    f->seen_error = future_error(&f->got);
    ready(t, &f->got, 7);       /* the same future, a new operation */
    AWAIT(t, &f->got);
    f->value = future_value(&f->got);
    f->reached_end = 1;
    ASYNC_CLEANUP(t);
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

struct producer {
    int      i;
    int      fail_with;         /* 0: complete with value */
    intptr_t value;
};

/* Lets the consumer suspend first, then settles its future. */
static poll_t producer(task_t *t)
{
    struct producer *f = task_frame(t);

    ASYNC_BEGIN(t);
    for (f->i = 0; f->i < 2; f->i++)
        YIELD(t);
    if (f->fail_with != 0)
        CHECK(slot_break(&slot, f->fail_with));
    else
        CHECK(slot_put(&slot, f->value));
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/* Runs one consumer and one producer, and returns the consumer's frame and error. */
static struct consumer run_pair(task_fn consumer_fn, intptr_t value, int fail_with,
                                int *consumer_error)
{
    runtime_t *rt = runtime_new();
    struct consumer result;
    struct producer *pf;
    task_t *c, *p;

    CHECK(rt != NULL);
    c = task_spawn(rt, consumer_fn, sizeof(struct consumer));
    p = task_spawn(rt, producer, sizeof(struct producer));
    CHECK(c != NULL && p != NULL);
    pf = task_frame(p);
    pf->value = value;
    pf->fail_with = fail_with;
    task_detach(p);

    CHECK(runtime_run(rt) == 0);
    CHECK(slot.waiting == NULL && slot.drops == 0);

    result = *(struct consumer *)task_frame(c);
    *consumer_error = task_error(c);
    task_detach(c);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
    return result;
}

/* Starts an operation and finishes without waiting for it. */
static poll_t impatient(task_t *t)
{
    struct consumer *f = task_frame(t);

    ASYNC_BEGIN(t);
    slot_wait(t, &f->got, &slot);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/*
 * The frame is freed when the task finishes, so the producer must be told to
 * forget the future first. Otherwise it would keep a pointer into freed
 * memory.
 */
static void finishing_drops_a_pending_future(void)
{
    runtime_t *rt = runtime_new();
    task_t *t;

    CHECK(rt != NULL);
    t = task_spawn(rt, impatient, sizeof(struct consumer));
    CHECK(t != NULL);
    task_detach(t);

    CHECK(runtime_run(rt) == 0);
    CHECK(slot.drops == 1 && slot.waiting == NULL);
    slot.drops = 0;

    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    struct consumer c;
    int error;

    /* Completed: the value arrives, on the consumer's second poll. */
    c = run_pair(consumer, 42, 0, &error);
    CHECK(c.polls == 2);
    CHECK(c.reached_end == 1 && c.value == 42);
    CHECK(c.error_in_cleanup == 0 && error == 0);

    /* Failed: AWAIT jumps to cleanup, and the error becomes the task's error. */
    c = run_pair(consumer, 0, EIO, &error);
    CHECK(c.polls == 2);
    CHECK(c.reached_end == 0);
    CHECK(c.error_in_cleanup == EIO && error == EIO);

    /* Failed, with AWAIT_TRY: the task sees the error and is not failed by it. */
    c = run_pair(patient, 0, EIO, &error);
    CHECK(c.polls == 2);
    CHECK(c.seen_error == EIO);
    CHECK(c.reached_end == 1 && c.value == 7);
    CHECK(c.error_in_cleanup == 0 && error == 0);

    finishing_drops_a_pending_future();
    return 0;
}
