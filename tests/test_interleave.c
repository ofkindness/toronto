/*
 * M1: tasks that yield take turns, in FIFO order, and a task spawned during a
 * pass is first polled on the next pass.
 */
#include <string.h>

#include "runtime/async.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/test.h"

/* Shared between tasks, so it lives outside every frame. */
static char   trace[32];
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

/* Marks its upper-case name three times, yielding after each mark. */
struct worker {
    char name;
    int  i;
};

static poll_t worker(task_t *t)
{
    struct worker *f = task_frame(t);

    ASYNC_BEGIN(t);
    for (f->i = 0; f->i < 3; f->i++) {
        mark(f->name);
        YIELD(t);
    }
    ASYNC_CLEANUP(t);
    mark((char)(f->name - 'A' + 'a'));      /* lower case: finished */
    ASYNC_END(t);
}

static task_t *spawn_worker(runtime_t *rt, char name)
{
    task_t *t = task_spawn(rt, worker, sizeof(struct worker));

    CHECK(t != NULL);
    ((struct worker *)task_frame(t))->name = name;
    task_detach(t);
    return t;
}

static void two_tasks_take_turns(void)
{
    runtime_t *rt = runtime_new();

    CHECK(rt != NULL);
    trace_reset();
    spawn_worker(rt, 'A');
    spawn_worker(rt, 'B');

    CHECK(runtime_run(rt) == 0);
    CHECK(strcmp(trace, "ABABABab") == 0);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

/* Spawns a child from inside a task, then yields once. */
static poll_t parent(task_t *t)
{
    ASYNC_BEGIN(t);
    mark('P');
    spawn_worker(task_runtime(t), 'C');
    YIELD(t);
    mark('p');
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

static void spawned_task_waits_for_the_next_pass(void)
{
    runtime_t *rt = runtime_new();
    task_t *t;

    CHECK(rt != NULL);
    trace_reset();
    t = task_spawn(rt, parent, 0);
    CHECK(t != NULL);
    CHECK(task_runtime(t) == rt);
    task_detach(t);

    /*
     * Pass 1 polls the parent only: the child is spawned during that pass.
     * The child is queued before the parent yields, so it runs first in
     * pass 2 and then keeps taking turns on its own.
     */
    CHECK(runtime_run(rt) == 0);
    CHECK(strcmp(trace, "PCpCCc") == 0);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
}

int main(void)
{
    two_tasks_take_turns();
    spawned_task_waits_for_the_next_pass();
    return 0;
}
