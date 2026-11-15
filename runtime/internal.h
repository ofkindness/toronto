/*
 * Shared by the runtime's own .c files. Not part of the API.
 */
#ifndef TORONTO_INTERNAL_H
#define TORONTO_INTERNAL_H

#ifdef NDEBUG
#error "toronto keeps asserts enabled in every build: do not define NDEBUG"
#endif

#include <stdbool.h>
#include <stddef.h>

#include "runtime/future.h"
#include "runtime/runtime.h"
#include "runtime/task.h"

/* Settled AWAITs that one poll may pass before the task must let others run. */
enum { TASK_BUDGET = 32 };

struct runtime {
    task_t *queue_head;     /* run queue: FIFO, linked through task->next */
    task_t *queue_tail;
    task_t *all;            /* every task not yet freed */
    size_t  tasks;          /* length of `all` */
    size_t  live;           /* tasks that have not finished */
    bool    running;        /* inside runtime_run */
    bool    shutting_down;  /* inside runtime_free: new tasks start cancelled */
};

/* Appends an idle or running task to the run queue. */
void runtime_enqueue(runtime_t *rt, task_t *t);

/*
 * Puts a task on the run queue. Waking a queued or finished task does
 * nothing. Waking a task that is being polled is remembered, and the task is
 * queued again if that poll returns POLL_PENDING.
 */
void task_wake(task_t *t);

/*
 * Called when a task's poll function returns POLL_READY. Drops its pending
 * futures, settles a pending join on it, and frees it if its handle has been
 * consumed.
 */
void task_finish(task_t *t);

/* Unlinks a task from its runtime and frees it together with its frame. */
void task_free(task_t *t);

/*
 * Drops every pending future of a task: each becomes idle, and its producer's
 * drop hook is called.
 */
void future_drop_all(task_t *t);

#endif
