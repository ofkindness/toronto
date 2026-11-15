/*
 * The macros that turn a poll function into a stackless coroutine.
 *
 * The body of the function becomes one `switch` on the saved resume point,
 * and every suspension point is a `case` label inside it (the protothreads
 * technique). See README.md, "Async macros", for the rules this imposes.
 *
 *     static poll_t reader(task_t *t)
 *     {
 *         struct frame *f = task_frame(t);   // recomputed on every poll
 *
 *         ASYNC_BEGIN(t);
 *         some_operation(t, &f->result);     // start the operation ...
 *         AWAIT(t, &f->result);              // ... then suspend until it settles
 *         use(future_value(&f->result));
 *         ASYNC_CLEANUP(t);                  // success, failure and cancellation end here
 *         ASYNC_END(t);
 *     }
 */
#ifndef TORONTO_ASYNC_H
#define TORONTO_ASYNC_H

#include <stdbool.h>

#include "runtime/future.h"
#include "runtime/task.h"

/*
 * A resume point is a `case` label that the statement before it runs into on
 * purpose. gcc's -Wextra flags that, so the macros say that they mean it.
 * This is the only compiler-specific construct in toronto.
 */
#if defined(__GNUC__)
#define ASYNC_FALLTHROUGH __attribute__((fallthrough))
#else
#define ASYNC_FALLTHROUGH ((void)0)
#endif

/*
 * Opens the body. A first poll starts at `case 0`; a later poll jumps to the
 * `case` of the AWAIT or YIELD that suspended it. A task that was cancelled
 * before its first poll goes straight to cleanup.
 *
 * `default` catches a resume point that this switch does not contain. That
 * happens when an AWAIT or YIELD sits inside a nested `switch` statement: its
 * `case` label then belongs to the inner switch (rule 2).
 */
#define ASYNC_BEGIN(t)                                                      \
    switch ((t)->resume) {                                                  \
    default:                                                                \
        async_bad_resume(t);    /* does not return */                       \
    case 0:                                                                 \
        if ((t)->flags & TASK_CANCELLED) goto async_cancelled

/*
 * Suspends until f settles, and does not jump if f failed: inspect
 * future_error(f) afterwards. Start the operation on the line before.
 *
 * The order of the two checks is the cancellation rule. A settled future
 * passes even when the task has been cancelled, so a result that already
 * exists is never lost. Only a future that has not settled lets the cancel
 * through.
 *
 * task_take_budget is the cooperative budget: a poll may pass only so many
 * settled futures before the task has to let the others run.
 */
#define AWAIT_TRY(t, f)                                                     \
    do {                                                                    \
        (t)->resume = __LINE__;                                             \
        ASYNC_FALLTHROUGH;                                                  \
    case __LINE__:                                                          \
        async_await_check((t), (f));                                        \
        if (!future_settled(f)) {                                           \
            if ((t)->flags & TASK_CANCELLED) goto async_cancelled;          \
            return POLL_PENDING;    /* settling f wakes t */                \
        }                                                                   \
        if (!task_take_budget(t))                                           \
            return POLL_PENDING;    /* budget spent: t is queued again */   \
    } while (0)

/*
 * Suspends until f settles. If f failed, its error becomes the task's error
 * and the task jumps to cleanup.
 */
#define AWAIT(t, f)                                                         \
    do {                                                                    \
        AWAIT_TRY((t), (f));                                                \
        if (future_error(f) != 0) {                                         \
            async_fail((t), future_error(f));                               \
            goto async_cleanup;                                             \
        }                                                                   \
    } while (0)

/*
 * Re-queues the task and suspends. The task continues after this line on a
 * later pass, once every other queued task has had its turn. A cancelled
 * task jumps to cleanup instead, before or after the suspension.
 */
#define YIELD(t)                                                            \
    do {                                                                    \
        async_yield(t);                                                     \
        if ((t)->flags & TASK_CANCELLED) goto async_cancelled;              \
        (t)->resume = __LINE__;                                             \
        return POLL_PENDING;                                                \
    case __LINE__:                                                          \
        if ((t)->flags & TASK_CANCELLED) goto async_cancelled;              \
    } while (0)

/* Sets the task's error to err, which must not be 0, and jumps to cleanup. */
#define ASYNC_FAIL(t, err)                                                  \
    do {                                                                    \
        async_fail((t), (err));                                             \
        goto async_cleanup;                                                 \
    } while (0)

/*
 * Starts the cleanup section. Success, failure and cancellation all end
 * here. The code after it must not AWAIT or YIELD.
 *
 * A task that reaches this line on its own jumps over `async_cancelled`. A
 * cancellation that takes effect at a resume point lands on it, and its
 * reason becomes the task's error.
 */
#define ASYNC_CLEANUP(t)                                                    \
        goto async_cleanup;                                                 \
    async_cancelled:                                                        \
        async_take_cancel(t);                                               \
    async_cleanup:                                                          \
        async_enter_cleanup(t)

/* Closes the body and finishes the task. */
#define ASYNC_END(t)                                                        \
    }                                                                       \
    return POLL_READY

/* Helpers for the macros above. Do not call them directly. */
void async_yield(task_t *t);
void async_fail(task_t *t, int err);
void async_await_check(const task_t *t, const future_t *f);
void async_take_cancel(task_t *t);
void async_enter_cleanup(task_t *t);
bool task_take_budget(task_t *t);
_Noreturn void async_bad_resume(const task_t *t);

#endif
