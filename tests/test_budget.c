/*
 * M2: the cooperative budget. A poll may pass at most 32 AWAITs whose future
 * has already settled; the next one queues the task again.
 *
 * A task that awaits 100 settled futures in a row is therefore polled four
 * times (32 + 32 + 32 + 4), and another task gets a turn after each of those
 * polls instead of waiting for all 100.
 */
#include <string.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/slot.h"
#include "tests/test.h"

static char   trace[32];
static size_t trace_len;

static void mark(char c)
{
    CHECK(trace_len < sizeof trace - 1);
    trace[trace_len++] = c;
}

struct greedy {
    future_t fut;
    int      i;
    intptr_t sum;
};

/* Never has to wait: every operation completes at once. */
static poll_t greedy(task_t *t)
{
    struct greedy *f = task_frame(t);

    mark('G');                          /* once per poll */
    ASYNC_BEGIN(t);
    for (f->i = 1; f->i <= 100; f->i++) {
        ready(t, &f->fut, f->i);
        AWAIT(t, &f->fut);
        f->sum += future_value(&f->fut);
    }
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

struct other {
    int i;
};

static poll_t other(task_t *t)
{
    struct other *f = task_frame(t);

    mark('o');                          /* once per poll */
    ASYNC_BEGIN(t);
    for (f->i = 0; f->i < 3; f->i++)
        YIELD(t);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

int main(void)
{
    runtime_t *rt = runtime_new();
    task_t *g, *o;

    CHECK(rt != NULL);
    g = task_spawn(rt, greedy, sizeof(struct greedy));
    o = task_spawn(rt, other, sizeof(struct other));
    CHECK(g != NULL && o != NULL);
    task_detach(o);

    CHECK(runtime_run(rt) == 0);

    CHECK(strcmp(trace, "GoGoGoGo") == 0);
    CHECK(((struct greedy *)task_frame(g))->sum == 5050);   /* no result was lost */
    CHECK(task_error(g) == 0);

    task_detach(g);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
    return 0;
}
