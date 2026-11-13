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

#include "runtime/runtime.h"
#include "runtime/task.h"

struct runtime {
    task_t *queue_head;     /* run queue: FIFO, linked through task->next */
    task_t *queue_tail;
    task_t *all;            /* every task not yet freed */
    size_t  tasks;          /* length of `all` */
    size_t  live;           /* tasks that have not finished */
    bool    running;        /* inside runtime_run */
};

/* Appends an idle or running task to the run queue. */
void runtime_enqueue(runtime_t *rt, task_t *t);

/*
 * Puts a task on the run queue. Waking a queued or finished task does
 * nothing. Waking a task that is being polled is remembered, and the task is
 * queued again if that poll returns POLL_PENDING.
 */
void task_wake(task_t *t);

/* Unlinks a task from its runtime and frees it together with its frame. */
void task_free(task_t *t);

#endif
