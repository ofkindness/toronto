/*
 * Tasks: the unit of scheduling. See README.md, "Tasks: the task contract".
 */
#ifndef TORONTO_TASK_H
#define TORONTO_TASK_H

#include <stddef.h>

#include "runtime/runtime.h"

typedef struct task task_t;

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
 * The returned handle must be consumed exactly once. In M1 the only way is
 * task_detach.
 */
task_t *task_spawn(runtime_t *rt, task_fn fn, size_t frame_size);

/* The frame: zeroed at spawn, stable for the task's life, aligned for any type. */
void *task_frame(task_t *t);

/* The runtime the task belongs to, e.g. to spawn from inside a task. */
runtime_t *task_runtime(const task_t *t);

/* The task's error code: 0, or the code given to ASYNC_FAIL. Final once finished. */
int task_error(const task_t *t);

/*
 * Consumes the handle: nobody will ask for the task's result. The task is
 * freed when it finishes, or right now if it already has. After this call
 * only the task itself may use its own handle. Does nothing if t is NULL.
 */
void task_detach(task_t *t);

/*
 * Everything below is visible only because the macros in async.h need the
 * resume field. Use the accessors above.
 */

enum {                      /* task->state */
    TASK_IDLE,              /* suspended and not queued: waiting for a wake */
    TASK_QUEUED,            /* on the run queue */
    TASK_RUNNING,           /* being polled right now */
    TASK_FINISHED           /* returned POLL_READY; never polled again */
};

enum {                      /* task->flags */
    TASK_NOTIFIED   = 1u << 0,  /* woken while it was being polled */
    TASK_DETACHED   = 1u << 1,  /* the handle has been consumed */
    TASK_IN_CLEANUP = 1u << 2   /* past ASYNC_CLEANUP: may not suspend */
};

struct task {
    int        resume;      /* resume point: 0 = not started, else a source line */
    int        error;
    unsigned   state;
    unsigned   flags;
    task_fn    fn;
    runtime_t *rt;
    void      *frame;
    task_t    *next;        /* run queue link */
    task_t    *all_prev;    /* the runtime's list of every task not yet freed */
    task_t    *all_next;
};

#endif
