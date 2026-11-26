/*
 * M3: task deadlines.
 *
 *   - a deadline cancels the task with ETIMEDOUT;
 *   - a task that finishes with its deadline still set leaves no timer entry;
 *   - setting the deadline again replaces it, which makes an idle timeout;
 *   - a cleared deadline does not fire;
 *   - a deadline also ends a task that never sleeps, at its next YIELD.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "runtime/timer.h"
#include "tests/test.h"

struct worker {
    future_t nap;
    uint64_t deadline;      /* 0: none */
    int      clear_it;      /* set the deadline, then clear it again */
    uint64_t sleep;         /* each round sleeps this long */
    int      rounds;
    int      reset_each_round;

    int      i;
    int      rounds_done;
    size_t   timers_seen;   /* heap entries right after setting the deadline */
    uint64_t ended_at;
    int      error_in_cleanup;
};

static poll_t worker(task_t *t)
{
    struct worker *f = task_frame(t);
    runtime_stats_t stats;

    ASYNC_BEGIN(t);
    if (f->deadline != 0)
        task_set_deadline(t, f->deadline);
    runtime_stats(task_runtime(t), &stats);
    f->timers_seen = stats.timers;
    if (f->clear_it)
        task_clear_deadline(t);

    for (f->i = 0; f->i < f->rounds; f->i++) {
        if (f->reset_each_round)
            task_set_deadline(t, f->deadline);
        sleep_ms(t, &f->nap, f->sleep);
        AWAIT(t, &f->nap);
        f->rounds_done++;
    }
    ASYNC_CLEANUP(t);
    f->ended_at = runtime_now_ms(task_runtime(t));
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/* Runs one worker under virtual time and returns its frame. */
static struct worker run_worker(struct worker setup, uint64_t outside_deadline)
{
    runtime_t *rt = runtime_new();
    runtime_stats_t stats;
    struct worker result;
    task_t *t;

    CHECK(rt != NULL);
    runtime_set_virtual_time(rt);
    t = task_spawn(rt, worker, sizeof(struct worker));
    CHECK(t != NULL);
    *(struct worker *)task_frame(t) = setup;

    if (outside_deadline != 0)
        task_set_deadline(t, outside_deadline);     /* by whoever holds the handle */

    CHECK(runtime_run(rt) == 0);

    result = *(struct worker *)task_frame(t);
    CHECK(task_error(t) == result.error_in_cleanup);

    /* The deadline was cleared when the task finished, not when it is freed. */
    runtime_stats(rt, &stats);
    CHECK(stats.timers == 0);

    /* On a finished task both do nothing. */
    task_set_deadline(t, 5);
    task_clear_deadline(t);

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);         /* no timer entry is left behind */
    runtime_free(rt);
    return result;
}

struct spinner {
    int spins;
    int error_in_cleanup;
};

/* Never sleeps and never waits: only its deadline can end it. */
static poll_t spinner(task_t *t)
{
    struct spinner *f = task_frame(t);

    ASYNC_BEGIN(t);
    task_set_deadline(t, 20);
    for (;;) {
        f->spins++;
        YIELD(t);
    }
    ASYNC_CLEANUP(t);
    f->error_in_cleanup = task_error(t);
    ASYNC_END(t);
}

/*
 * In real time, because virtual time only moves when every task is
 * suspended, and this one never is.
 */
static void deadline_ends_a_task_that_only_yields(void)
{
    runtime_t *rt = runtime_new();
    struct spinner *f;
    uint64_t start;
    task_t *t;

    CHECK(rt != NULL);
    t = task_spawn(rt, spinner, sizeof(struct spinner));
    CHECK(t != NULL);
    f = task_frame(t);
    start = runtime_now_ms(rt);

    CHECK(runtime_run(rt) == 0);
    CHECK(runtime_now_ms(rt) - start >= 20);
    CHECK(f->spins > 0);
    CHECK(f->error_in_cleanup == ETIMEDOUT);

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    struct worker w;

    /* The deadline passes during the sleep. */
    w = run_worker((struct worker){ .deadline = 50, .sleep = 100, .rounds = 1 }, 0);
    CHECK(w.timers_seen == 1);
    CHECK(w.rounds_done == 0);
    CHECK(w.error_in_cleanup == ETIMEDOUT && w.ended_at == 50);

    /* The task finishes long before its deadline, which must not linger. */
    w = run_worker((struct worker){ .deadline = 1000, .sleep = 10, .rounds = 1 }, 0);
    CHECK(w.rounds_done == 1);
    CHECK(w.error_in_cleanup == 0 && w.ended_at == 10);

    /* An idle timeout: 50 ms, set again before each 30 ms sleep. */
    w = run_worker((struct worker){ .deadline = 50, .sleep = 30, .rounds = 5,
                                    .reset_each_round = 1 }, 0);
    CHECK(w.rounds_done == 5);
    CHECK(w.error_in_cleanup == 0 && w.ended_at == 150);

    /* The same without the reset: the second sleep runs into the deadline. */
    w = run_worker((struct worker){ .deadline = 50, .sleep = 30, .rounds = 5 }, 0);
    CHECK(w.rounds_done == 1);
    CHECK(w.error_in_cleanup == ETIMEDOUT && w.ended_at == 50);

    /* A cleared deadline does not fire. */
    w = run_worker((struct worker){ .deadline = 20, .clear_it = 1, .sleep = 100,
                                    .rounds = 1 }, 0);
    CHECK(w.timers_seen == 1);
    CHECK(w.rounds_done == 1);
    CHECK(w.error_in_cleanup == 0 && w.ended_at == 100);

    /* A deadline set from outside, through the handle. */
    w = run_worker((struct worker){ .sleep = 100, .rounds = 1 }, 40);
    CHECK(w.rounds_done == 0);
    CHECK(w.error_in_cleanup == ETIMEDOUT && w.ended_at == 40);

    task_set_deadline(NULL, 5);     /* both do nothing */
    task_clear_deadline(NULL);

    deadline_ends_a_task_that_only_yields();
    return 0;
}
