/*
 * The macros that turn a poll function into a stackless coroutine.
 *
 * The body of the function becomes one `switch` on the saved resume point,
 * and every suspension point is a `case` label inside it (the protothreads
 * technique). See README.md, "Async macros", for the rules this imposes.
 *
 *     static poll_t countdown(task_t *t)
 *     {
 *         struct frame *f = task_frame(t);   // recomputed on every poll
 *
 *         ASYNC_BEGIN(t);
 *         for (f->i = 3; f->i > 0; f->i--) {
 *             printf("%d\n", f->i);
 *             YIELD(t);
 *         }
 *         ASYNC_CLEANUP(t);                  // success and failure end here
 *         ASYNC_END(t);
 *     }
 *
 * M1 has ASYNC_BEGIN, YIELD, ASYNC_FAIL, ASYNC_CLEANUP and ASYNC_END.
 * AWAIT and AWAIT_TRY arrive with futures in M2.
 */
#ifndef TORONTO_ASYNC_H
#define TORONTO_ASYNC_H

#include "runtime/task.h"

/*
 * Opens the body. A first poll starts at `case 0`; a later poll jumps to the
 * `case` of the YIELD that suspended it.
 *
 * `default` catches a resume point that this switch does not contain. That
 * happens when a YIELD sits inside a nested `switch` statement: its `case`
 * label then belongs to the inner switch (rule 2).
 */
#define ASYNC_BEGIN(t)                                                      \
    switch ((t)->resume) {                                                  \
    default:                                                                \
        async_bad_resume(t);    /* does not return */                       \
    case 0:

/*
 * Re-queues the task and suspends. The task continues after this line on a
 * later pass, once every other queued task has had its turn.
 */
#define YIELD(t)                                                            \
    do {                                                                    \
        async_yield(t);                                                     \
        (t)->resume = __LINE__;                                             \
        return POLL_PENDING;                                                \
    case __LINE__:;                                                         \
    } while (0)

/* Sets the task's error to err, which must not be 0, and jumps to cleanup. */
#define ASYNC_FAIL(t, err)                                                  \
    do {                                                                    \
        async_fail((t), (err));                                             \
        goto async_cleanup;                                                 \
    } while (0)

/*
 * Starts the cleanup section. Success and failure both end here. The code
 * after it must not YIELD.
 *
 * The `goto` in front of its own label looks redundant. It keeps the label
 * referenced in a task that never fails, and in M2 it is what jumps over the
 * code that handles a cancellation.
 */
#define ASYNC_CLEANUP(t)                                                    \
        goto async_cleanup;                                                 \
    async_cleanup:                                                          \
        async_enter_cleanup(t)

/* Closes the body and finishes the task. */
#define ASYNC_END(t)                                                        \
    }                                                                       \
    return POLL_READY

/* Helpers for the macros above. Do not call them directly. */
void async_yield(task_t *t);
void async_fail(task_t *t, int err);
void async_enter_cleanup(task_t *t);
_Noreturn void async_bad_resume(const task_t *t);

#endif
