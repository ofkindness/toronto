/*
 * The runtime: it owns the run queue and every task, and runs the loop.
 *
 * One runtime is used from one thread. See README.md, "Run loop".
 */
#ifndef TORONTO_RUNTIME_H
#define TORONTO_RUNTIME_H

#include <stddef.h>

typedef struct runtime runtime_t;

typedef struct {
    size_t tasks;   /* tasks not yet freed */
    size_t timers;  /* entries in the timer heap (always 0 before M3) */
    size_t fds;     /* entries in the fd table (always 0 before M4) */
} runtime_stats_t;

/* Returns NULL and sets errno on failure. */
runtime_t *runtime_new(void);

/*
 * Runs the loop until no task is live. Returns 0, or EDEADLK if tasks are
 * still live but nothing can wake them. Must not be called from a task.
 */
int runtime_run(runtime_t *rt);

/*
 * Frees the runtime and every task it still holds, including finished tasks
 * whose handle was never consumed.
 *
 * If tasks are still live, this is first a bounded shutdown: each one is
 * cancelled with ECANCELED and polled until it has run its cleanup section
 * and finished. A task spawned during the shutdown starts cancelled.
 */
void runtime_free(runtime_t *rt);

void runtime_stats(const runtime_t *rt, runtime_stats_t *out);

#endif
