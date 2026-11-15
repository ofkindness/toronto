/*
 * M3: sleeping, under virtual time.
 *
 *   - sleeps finish in deadline order, and ties in the order they were armed;
 *   - a sleep of 0 completes on the next pass, without moving the clock;
 *   - a cancelled sleep leaves the heap;
 *   - many timers, some of them cancelled, still fire at the right times;
 *   - in real time, a sleep lasts at least as long as it was asked to.
 */
#include <errno.h>
#include <string.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "runtime/timer.h"
#include "tests/test.h"

static char   trace[16];
static size_t trace_len;

static void mark(char c)
{
    CHECK(trace_len < sizeof trace - 1);
    trace[trace_len++] = c;
}

static void trace_reset(void)
{
    memset(trace, 0, sizeof trace);
    trace_len = 0;
}

struct sleeper {
    future_t nap;
    char     name;
    uint64_t ms;
    int      polls;
    int      woke;          /* did the sleep complete? */
    uint64_t woke_at;       /* the clock when it did */
    intptr_t value;
    int      error_in_cleanup;
};

static poll_t sleeper(task_t *t)
{
    struct sleeper *f = task_frame(t);

    f->polls++;
    ASYNC_BEGIN(t);
    sleep_ms(t, &f->nap, f->ms);
    AWAIT(t, &f->nap);
    f->woke = 1;
    f->woke_at = runtime_now_ms(task_runtime(t));
    f->value = future_value(&f->nap);
    if (f->name != 0)
        mark(f->name);
    ASYNC_CLEANUP(t);
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

static task_t *spawn_sleeper(runtime_t *rt, char name, uint64_t ms)
{
    task_t *t = task_spawn(rt, sleeper, sizeof(struct sleeper));
    struct sleeper *f;

    CHECK(t != NULL);
    f = task_frame(t);
    f->name = name;
    f->ms = ms;
    return t;
}

static runtime_t *virtual_runtime(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    runtime_set_virtual_time(rt);
    CHECK(runtime_now_ms(rt) == 0);
    return rt;
}

static void sleeps_finish_in_deadline_order(void)
{
    runtime_t *rt = virtual_runtime();
    task_t *a, *b, *c;

    trace_reset();
    a = spawn_sleeper(rt, 'A', 30);
    b = spawn_sleeper(rt, 'B', 10);
    c = spawn_sleeper(rt, 'C', 20);

    CHECK(runtime_run(rt) == 0);
    CHECK(strcmp(trace, "BCA") == 0);
    CHECK(((struct sleeper *)task_frame(a))->woke_at == 30);
    CHECK(((struct sleeper *)task_frame(b))->woke_at == 10);
    CHECK(((struct sleeper *)task_frame(c))->woke_at == 20);
    CHECK(((struct sleeper *)task_frame(a))->value == 0);
    CHECK(runtime_now_ms(rt) == 30);    /* the clock moved, the test did not wait */

    task_detach(a);
    task_detach(b);
    task_detach(c);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

static void ties_fire_in_insertion_order(void)
{
    runtime_t *rt = virtual_runtime();

    trace_reset();
    task_detach(spawn_sleeper(rt, 'A', 10));
    task_detach(spawn_sleeper(rt, 'B', 10));
    task_detach(spawn_sleeper(rt, 'C', 10));
    task_detach(spawn_sleeper(rt, 'D', 5));

    CHECK(runtime_run(rt) == 0);
    CHECK(strcmp(trace, "DABC") == 0);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

static void sleep_of_zero_takes_one_pass(void)
{
    runtime_t *rt = virtual_runtime();
    task_t *t = spawn_sleeper(rt, 0, 0);
    struct sleeper *f = task_frame(t);

    CHECK(runtime_run(rt) == 0);
    CHECK(f->woke == 1);
    CHECK(f->polls == 2);               /* it did suspend, once */
    CHECK(f->woke_at == 0);             /* and the clock did not move */

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/* Sleeps a little, then cancels the victim and looks at the heap. */
struct canceller {
    future_t nap;
    task_t  *victim;        /* borrowed: main owns the handle */
    size_t   timers_before;
    size_t   timers_after;
};

static poll_t canceller(task_t *t)
{
    struct canceller *f = task_frame(t);
    runtime_stats_t stats;

    ASYNC_BEGIN(t);
    sleep_ms(t, &f->nap, 10);
    AWAIT(t, &f->nap);
    runtime_stats(task_runtime(t), &stats);
    f->timers_before = stats.timers;
    task_cancel(f->victim, ECANCELED);
    runtime_stats(task_runtime(t), &stats);
    f->timers_after = stats.timers;
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void cancelled_sleep_leaves_the_heap(void)
{
    runtime_t *rt = virtual_runtime();
    task_t *victim = spawn_sleeper(rt, 0, 1000);
    task_t *c = task_spawn(rt, canceller, sizeof(struct canceller));
    struct sleeper *vf = task_frame(victim);
    struct canceller *cf;

    CHECK(c != NULL);
    cf = task_frame(c);
    cf->victim = victim;

    CHECK(runtime_run(rt) == 0);
    CHECK(cf->timers_before == 1);      /* the victim's sleep */
    CHECK(cf->timers_after == 0);       /* gone at the cancel, not later */
    CHECK(vf->woke == 0 && vf->error_in_cleanup == ECANCELED);
    CHECK(runtime_now_ms(rt) == 10);    /* nobody waited for the 1000 ms */

    task_detach(victim);
    task_detach(c);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/*
 * Many timers at once, to exercise the heap: 200 sleeps with scattered
 * durations. After 500 ms a third of the longer ones are cancelled, which
 * takes entries out of the middle of the heap.
 */
enum { MANY = 200 };

static task_t *many[MANY];
static uint64_t last_wake;

struct reaper {
    future_t nap;
    int      cancelled;
};

static poll_t reaper(task_t *t)
{
    struct reaper *f = task_frame(t);

    ASYNC_BEGIN(t);
    sleep_ms(t, &f->nap, 500);
    AWAIT(t, &f->nap);
    for (int i = 0; i < MANY; i += 3) {
        struct sleeper *s = task_frame(many[i]);

        if (s->ms > 500) {
            task_cancel(many[i], ECANCELED);
            f->cancelled++;
        }
    }
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void many_timers(void)
{
    runtime_t *rt = virtual_runtime();
    task_t *r = task_spawn(rt, reaper, sizeof(struct reaper));
    uint32_t seed = 12345;
    int woke = 0, cancelled = 0;

    CHECK(r != NULL);
    for (int i = 0; i < MANY; i++) {
        seed = seed * 1103515245u + 12345u;
        many[i] = spawn_sleeper(rt, 0, (seed >> 8) % 1000);
    }

    CHECK(runtime_run(rt) == 0);

    last_wake = 0;
    for (int i = 0; i < MANY; i++) {
        struct sleeper *s = task_frame(many[i]);

        if (s->woke) {
            CHECK(s->woke_at == s->ms);         /* exactly on time */
            CHECK(s->error_in_cleanup == 0);
            if (s->woke_at > last_wake)
                last_wake = s->woke_at;
            woke++;
        } else {
            CHECK(i % 3 == 0 && s->ms > 500);   /* only the ones the reaper chose */
            CHECK(s->error_in_cleanup == ECANCELED);
            cancelled++;
        }
        task_detach(many[i]);
    }
    CHECK(cancelled == ((struct reaper *)task_frame(r))->cancelled);
    CHECK(cancelled > 0 && woke + cancelled == MANY);
    CHECK(runtime_now_ms(rt) == last_wake);

    task_detach(r);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/* The one test here that really waits: 20 ms on the real clock. */
static void real_sleep_is_not_short(void)
{
    runtime_t *rt = runtime_new();
    uint64_t start, elapsed;
    task_t *t;

    CHECK(rt != NULL);
    t = spawn_sleeper(rt, 0, 20);
    start = runtime_now_ms(rt);

    CHECK(runtime_run(rt) == 0);

    elapsed = runtime_now_ms(rt) - start;
    CHECK(((struct sleeper *)task_frame(t))->woke == 1);
    CHECK(elapsed >= 20);
    CHECK(elapsed < 5000);

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    sleeps_finish_in_deadline_order();
    ties_fire_in_insertion_order();
    sleep_of_zero_takes_one_pass();
    cancelled_sleep_leaves_the_heap();
    many_timers();
    real_sleep_is_not_short();
    return 0;
}
