/*
 * Tasks: the unit of scheduling. See README.md, "Tasks: the task contract".
 */
#ifndef TORONTO_TASK_H
#define TORONTO_TASK_H

#include <stddef.h>
#include <stdint.h>

#include "runtime/runtime.h"

typedef struct task task_t;
typedef struct future future_t;     /* runtime/future.h */

typedef enum { POLL_PENDING, POLL_READY } poll_t;

/*
 * A poll function. One call is one poll: it runs until the task suspends
 * (POLL_PENDING) or finishes (POLL_READY). Write it with the macros in
 * async.h.
 */
typedef poll_t (*task_fn)(task_t *t);

/*
 * Creates a task with a zeroed frame of frame_size bytes and queues it. It is
 * first polled on the next pass, so the caller can fill in the frame after
 * this returns. Returns NULL and sets errno on failure.
 *
 * The returned handle must be consumed exactly once: by task_detach, or by a
 * join that settles. A consumed handle must not be used again.
 */
task_t *task_spawn(runtime_t *rt, task_fn fn, size_t frame_size);

/* The frame: zeroed at spawn, stable for the task's life, aligned for any type. */
void *task_frame(task_t *t);

/* The runtime the task belongs to, e.g. to spawn from inside a task. */
runtime_t *task_runtime(const task_t *t);

/*
 * The task's error code: 0, the code of a failed AWAIT or of ASYNC_FAIL, or
 * the reason of a cancellation that took effect. Final once finished.
 */
int task_error(const task_t *t);

/*
 * Consumes the handle: nobody will ask for the task's result. The task is
 * freed when it finishes, or right now if it already has. After this call
 * only the task itself may use its own handle. Does nothing if t is NULL.
 */
void task_detach(task_t *t);

/*
 * An operation: f completes when child finishes, with the child's error code
 * as its value. A join never fails. t is the joiner and must own the child's
 * handle.
 *
 * When the join settles, the handle is consumed and the child is freed. A
 * join that is dropped, because the joiner was cancelled, does not consume
 * the handle: the joiner still owns it.
 */
void task_join(task_t *t, task_t *child, future_t *f);

/*
 * Asks a task to stop. Its pending futures are dropped at once. The task is
 * woken, and at its next resume point it jumps to its cleanup section with
 * reason, which must not be 0, as its error code.
 *
 * Does nothing if t is NULL, has finished, is already cancelled, or is in its
 * cleanup section. See README.md, "Cancellation".
 */
void task_cancel(task_t *t, int reason);

/*
 * Cancels the task with ETIMEDOUT once ms_from_now milliseconds have passed.
 * Setting it again replaces the earlier deadline: for an idle timeout, set
 * it after every message. The deadline is cleared when the task finishes.
 * Both do nothing if t is NULL or has finished.
 */
void task_set_deadline(task_t *t, uint64_t ms_from_now);
void task_clear_deadline(task_t *t);

/*
 * Everything below is visible only because the macros in async.h need the
 * resume and flags fields. Use the accessors above.
 */

/*
 * An entry in the runtime's timer heap. It is embedded in the future or the
 * task it belongs to, so arming a timer allocates nothing.
 */
typedef struct timer_entry {
    uint64_t deadline;      /* ms on the loop's clock */
    uint64_t seq;           /* insertion order: breaks ties between equal deadlines */
    size_t   pos;           /* index in the heap plus 1; 0 = not armed */
    unsigned kind;          /* what to do when it fires */
    void    *owner;         /* the future or the task */
} timer_entry_t;

enum {                      /* task->state */
    TASK_IDLE,              /* suspended and not queued: waiting for a wake */
    TASK_QUEUED,            /* on the run queue */
    TASK_RUNNING,           /* being polled right now */
    TASK_FINISHED           /* returned POLL_READY; never polled again */
};

enum {                      /* task->flags */
    TASK_NOTIFIED   = 1u << 0,  /* woken while it was being polled */
    TASK_DETACHED   = 1u << 1,  /* the handle has been consumed by task_detach */
    TASK_IN_CLEANUP = 1u << 2,  /* past ASYNC_CLEANUP: may not suspend */
    TASK_CANCELLED  = 1u << 3   /* task_cancel was called; see cancel_reason */
};

struct task {
    int        resume;      /* resume point: 0 = not started, else a source line */
    int        error;
    unsigned   state;
    unsigned   flags;
    int        cancel_reason;
    int        budget;      /* settled AWAITs this poll may still pass */
    task_fn    fn;
    runtime_t *rt;
    void      *frame;
    future_t  *pending;     /* the futures this task has started and not seen settle */
    future_t  *joiner;      /* the pending join on this task, if any */
    timer_entry_t deadline; /* armed by task_set_deadline */
    task_t    *next;        /* run queue link */
    task_t    *all_prev;    /* the runtime's list of every task not yet freed */
    task_t    *all_next;
};

#endif
