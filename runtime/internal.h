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
#include "runtime/poller.h"
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

    poller_t *poller;

    timer_entry_t **heap;   /* binary min-heap, ordered by (deadline, seq) */
    size_t   heap_len;
    size_t   heap_cap;      /* always >= tasks + future_timers: see timer_reserve */
    size_t   future_timers; /* armed entries that belong to a future */
    uint64_t timer_seq;

    bool     virtual_time;  /* the clock is virtual_now, and the loop never sleeps */
    uint64_t virtual_now;
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

/* Drops the operation on a pending future, then fails the future with error. */
void future_drop_and_fail(future_t *f, int error);

/* The timer heap. */

enum {                      /* timer_entry->kind */
    TIMER_SLEEP = 1,        /* owner is a future: complete it with 0 */
    TIMER_TIMEOUT,          /* owner is a future: drop its operation, fail with ETIMEDOUT */
    TIMER_DEADLINE          /* owner is a task: cancel it with ETIMEDOUT */
};

/*
 * Makes room for `slots` entries in total. Returns 0 or ENOMEM.
 *
 * The heap always has room for one deadline per task and for every armed
 * future timer. A task reserves its slot when it is spawned and a future
 * timer when it is armed, so task_set_deadline can never fail.
 */
int timer_reserve(runtime_t *rt, size_t slots);

/* Arms an entry, or re-arms it with a new deadline. The slot must be reserved. */
void timer_arm(runtime_t *rt, timer_entry_t *e, unsigned kind, void *owner,
               uint64_t deadline);

/* Takes an entry out of the heap. Does nothing if it is not armed. */
void timer_disarm(runtime_t *rt, timer_entry_t *e);

/* The earliest deadline, if any entry is armed. */
bool timer_next(const runtime_t *rt, uint64_t *deadline);

/* Fires every entry whose deadline is not after now, earliest first. */
void timer_fire_due(runtime_t *rt, uint64_t now);

#endif
