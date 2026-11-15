/*
 * M2: join.
 *
 *   - a join completes with the child's error code as its value: 0, the
 *     child's failure, or the child's cancel reason;
 *   - a join never fails, so the child's failure is not the parent's;
 *   - joining a child that has already finished completes at once;
 *   - a join that is cancelled leaves the handle with the joiner, which then
 *     cancels and detaches the child in its cleanup section.
 */
#include <errno.h>

#include "runtime/async.h"
#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/test.h"

static int children_finished;
static int last_child_error;

struct child {
    int yields;         /* yield this often, then finish */
    int forever;        /* or: yield until cancelled */
    int fail_with;      /* finish with ASYNC_FAIL */
    int cancel_self;    /* finish by cancelling itself with this reason */
    int i;
};

static poll_t child(task_t *t)
{
    struct child *f = task_frame(t);

    ASYNC_BEGIN(t);
    for (f->i = 0; f->forever || f->i < f->yields; f->i++)
        YIELD(t);
    if (f->cancel_self != 0) {
        task_cancel(t, f->cancel_self);
        YIELD(t);
    }
    if (f->fail_with != 0)
        ASYNC_FAIL(t, f->fail_with);
    ASYNC_CLEANUP(t);
    children_finished++;
    last_child_error = task_error(t);
    ASYNC_END(t);
}

struct parent {
    struct child setup;         /* how the child should behave */
    int          wait_first;    /* yield this often before joining */

    task_t  *child;             /* owned until the join settles */
    future_t joined;
    int      i;
    int      got_result;
    intptr_t result;
    int      cleanup_saw_child;
};

/* Spawns a child and joins it. This is the pattern from the README. */
static poll_t parent(task_t *t)
{
    struct parent *f = task_frame(t);

    ASYNC_BEGIN(t);
    f->child = task_spawn(task_runtime(t), child, sizeof(struct child));
    if (f->child == NULL)
        ASYNC_FAIL(t, ENOMEM);
    *(struct child *)task_frame(f->child) = f->setup;

    for (f->i = 0; f->i < f->wait_first; f->i++)
        YIELD(t);

    task_join(t, f->child, &f->joined);
    AWAIT(t, &f->joined);
    f->child = NULL;                    /* the join settled: the handle is consumed */
    f->result = future_value(&f->joined);
    f->got_result = 1;

    ASYNC_CLEANUP(t);
    f->cleanup_saw_child = (f->child != NULL);
    task_cancel(f->child, ECANCELED);   /* both do nothing if f->child is NULL */
    task_detach(f->child);
    ASYNC_END(t);
}

struct canceller {
    task_t *victim;     /* borrowed: main owns the handle */
    int     passes;
    int     i;
};

static poll_t canceller(task_t *t)
{
    struct canceller *f = task_frame(t);

    ASYNC_BEGIN(t);
    for (f->i = 0; f->i < f->passes; f->i++)
        YIELD(t);
    task_cancel(f->victim, ECANCELED);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/*
 * Runs one parent with one child. cancel_after < 0 means the parent is left
 * alone; otherwise another task cancels it after that many passes.
 */
static struct parent run_parent(struct child setup, int wait_first, int cancel_after,
                                int *parent_error)
{
    runtime_t *rt = runtime_new();
    struct parent result;
    struct parent *pf;
    task_t *p;

    CHECK(rt != NULL);
    children_finished = 0;
    last_child_error = -1;

    p = task_spawn(rt, parent, sizeof(struct parent));
    CHECK(p != NULL);
    pf = task_frame(p);
    pf->setup = setup;
    pf->wait_first = wait_first;

    if (cancel_after >= 0) {
        task_t *c = task_spawn(rt, canceller, sizeof(struct canceller));
        struct canceller *cf;

        CHECK(c != NULL);
        cf = task_frame(c);
        cf->victim = p;
        cf->passes = cancel_after;
        task_detach(c);
    }

    CHECK(runtime_run(rt) == 0);

    result = *pf;
    *parent_error = task_error(p);
    task_detach(p);
    CHECK_NOTHING_LEFT(rt);             /* the child is gone too */
    runtime_free(rt);
    return result;
}

int main(void)
{
    struct parent p;
    int error;

    /* The child succeeds. */
    p = run_parent((struct child){ .yields = 2 }, 0, -1, &error);
    CHECK(p.got_result == 1 && p.result == 0);
    CHECK(p.cleanup_saw_child == 0 && error == 0);
    CHECK(children_finished == 1);

    /* The child fails. The join reports that as a value; the parent succeeds. */
    p = run_parent((struct child){ .yields = 2, .fail_with = EIO }, 0, -1, &error);
    CHECK(p.got_result == 1 && p.result == EIO);
    CHECK(error == 0);

    /* The child is cancelled: the join sees the reason. */
    p = run_parent((struct child){ .yields = 1, .cancel_self = EINTR }, 0, -1, &error);
    CHECK(p.got_result == 1 && p.result == EINTR);
    CHECK(last_child_error == EINTR && error == 0);

    /* The child finished long before the join: the join completes at once. */
    p = run_parent((struct child){ .fail_with = EIO }, 3, -1, &error);
    CHECK(p.got_result == 1 && p.result == EIO);
    CHECK(children_finished == 1 && error == 0);

    /*
     * The parent is cancelled while it waits for a child that never ends.
     * The join is dropped and the handle stays with the parent, whose
     * cleanup cancels and detaches the child.
     */
    p = run_parent((struct child){ .forever = 1 }, 0, 2, &error);
    CHECK(p.got_result == 0);
    CHECK(p.cleanup_saw_child == 1);
    CHECK(error == ECANCELED);
    CHECK(children_finished == 1 && last_child_error == ECANCELED);

    return 0;
}
