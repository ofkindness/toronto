/*
 * Timers: sleeping, and timing out an operation. See README.md, "Timers".
 *
 * A task deadline is the third kind of timer. It is in task.h, with the
 * other functions that act on a task: task_set_deadline, task_clear_deadline.
 */
#ifndef TORONTO_TIMER_H
#define TORONTO_TIMER_H

#include <stdint.h>

#include "runtime/future.h"
#include "runtime/task.h"

/*
 * An operation: f completes with value 0 after ms milliseconds. A sleep of 0
 * completes on the next pass, after timers and I/O have been checked.
 */
void sleep_ms(task_t *t, future_t *f, uint64_t ms);

/*
 * Gives the operation on f a time limit. Call it right after starting the
 * operation. If f is still pending after ms milliseconds, the operation is
 * dropped and f fails with ETIMEDOUT. With AWAIT_TRY the task can handle
 * that and carry on.
 *
 * Calling it again replaces the earlier limit. It does nothing on a future
 * that is settled or idle. A sleep cannot have a timeout.
 */
void future_set_timeout(future_t *f, uint64_t ms);

#endif
