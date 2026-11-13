# toronto

A minimal, single-threaded async runtime in C11, built from scratch to learn how runtimes like Tokio work inside.

**Status: design proposal.** This README is the design. M0 (tooling) and M1 (coroutines without I/O) are implemented. M2 to M5 are not.

---

## 🚀 What This Project Is

toronto is a **teaching runtime**. It contains:

- a **poller layer** over the OS readiness API: epoll on Linux, kqueue on macOS and FreeBSD
- a **reactor** that turns readiness into completed I/O operations
- a **timer heap** that drives the loop's wait timeout
- a **scheduler** with a run queue of tasks
- **tasks** written as stackless coroutines, with `AWAIT`-style macros
- **one-shot futures** that carry a result or an error
- **cancellation, deadlines and join**

### Scope

toronto is complete enough to run a real TCP echo server under load in its own tests, and no more. It is not hardened, its API is not stable, and it is not meant for production. "Real" means the end goal below has measurable acceptance criteria. It does not mean production-ready.

### How it differs from Tokio

toronto borrows Tokio's vocabulary, but its I/O model is different, and that difference shapes the rest of the design.

- **Who makes the system call.** In Tokio, a task calls `read` itself each time it is polled, and registers a waker when the call would block. In toronto, the reactor makes the call and hands the task a finished result. That is a completion model, as in libuv or io_uring, built on top of readiness.
- **What is polled.** Tokio polls a tree of futures, and each one gets a waker. toronto polls only tasks. A future here is a slot that receives one result, and its waker is simply the task that started the operation.
- **What it buys.** `EAGAIN` and the retry logic live in one place, and `AWAIT` stays a single line.
- **What it costs.** A buffer must stay valid while its operation is pending, so it lives in the frame. A result can also exist before the task has seen it, which is why [cancellation](#cancellation) never discards a settled future.

---

## 🎯 Goals

- Show how an async runtime works, one layer at a time, with each layer small enough to read in one sitting.
- Build everything from scratch: libc and OS system calls only.
- Make every design decision explicit in this README: why it was chosen, and what it costs.
- Keep each milestone testable on its own, on both poller backends.

## 🚫 Non-goals

- **Multithreading and work stealing.** Supporting them would need atomic task flags, a thread-safe wake path (`eventfd`, `EVFILT_USER`) and per-thread run queues. That is a different design, not an extension of this one.
- **Async file I/O and async DNS.** See [Blocking work](#blocking-work).
- TLS, HTTP, Windows (IOCP), and a stable API or ABI.
- Competing with libuv or Tokio on performance.

## 🏁 End Goal

`examples/echo_server.c` plus a load test, `tests/test_echo_load.c`. The test runs the server and its clients as tasks in one runtime, in one process. The goal is met when all of these hold on **both** backends:

1. 1,000 concurrent client connections each send 100 messages of 1 KiB, and every echo matches.
2. After the load, a client that stays silent is disconnected by its idle deadline of 200 ms.
3. Afterwards, `runtime_run` returns 0, `runtime_stats` reports no tasks, timers or fd table entries, and the process has as many open fds as before the test.
4. The run is clean under AddressSanitizer and UndefinedBehaviorSanitizer, plus LeakSanitizer on Linux.
5. The test prints throughput in messages per second. This is for information only; there is no threshold.

The test raises its own `RLIMIT_NOFILE` soft limit, because macOS defaults to 256 open files.

Two details keep the test from failing for reasons that have nothing to do with the runtime:

- **Connects are throttled.** The clients start at most 64 connects at a time. macOS caps the accept queue at 128 by default (`kern.ipc.somaxconn`), and 1,000 connects started in one pass would overflow it and stall on SYN retransmits.
- **The idle deadline is a server parameter.** It is 5 s during the load and 200 ms for the silent client. With a 200 ms deadline during the load, one slow pass under the sanitizers could disconnect an active client.

---

## 📖 Terms

These words mean exactly this throughout the README and the code.

| Term | Meaning |
|---|---|
| **runtime** | Owns the poller, the timer heap, the run queue and all tasks. One per process; used from one thread. |
| **loop** | `runtime_run`. It repeats three steps: poll queued tasks, wait for events, fire timers and I/O. |
| **task** | The unit of scheduling: a poll function, a frame, a resume point and a final error code. |
| **poll function** | `poll_t fn(task_t *t)`. One call is one *poll*. It runs until the task suspends (`POLL_PENDING`) or finishes (`POLL_READY`). |
| **frame** | Per-task memory, allocated and zeroed at spawn. Holds everything that must survive a suspension. |
| **suspend / resume** | The poll function returns `POLL_PENDING` / the task is polled again and continues from its resume point. |
| **wake** | Put a task on the run queue. Waking a queued or finished task does nothing. Waking a task while it is being polled queues it again if that poll returns `POLL_PENDING`. |
| **future** | A one-shot completion slot: *idle*, then *pending*, then *settled* (*completed* with a value, or *failed* with an error). It belongs to the task that started its operation, the *waiter*, and lives in that task's frame. |
| **operation** | A call that starts async work and later settles a future: `sleep_ms`, `io_read`, `task_join`, … |
| **producer** | The component that settles a future: the timer heap, the reactor, or a task. |
| **drop** | Tell a producer to forget a pending future, because its waiter was cancelled or has finished, or because the future timed out. |
| **poller** | The OS readiness abstraction (`poller.h`), with epoll and kqueue backends. |
| **reactor** | Maps poller events to pending I/O operations, and runs the system calls for them. |
| **deadline** | An absolute time on the monotonic clock. When it passes, the timer heap acts. |
| **cancel** | Ask a task to stop, with a reason code. Cancellation is cooperative. |
| **error code** | An `int`: 0 means success; anything else is a positive `errno` value. |

There are no "promises". The producer side of a future is just `future_begin`, then `future_complete` or `future_fail`.

---

## 🧱 Architecture

```
          task code: ASYNC_BEGIN / AWAIT / YIELD / ASYNC_CLEANUP / ASYNC_END
                                      │
   ┌──────────────┬───────────────────┼─────────────────────┐
   │  scheduler   │      futures      │   timers   │ reactor │
   │  run queue   │ one-shot, 1 waiter│  min-heap  │ fd table│
   └──────────────┴───────────────────┴─────┬──────┴────┬────┘
                                            │ timeout   │ arm / events
                                       ┌────┴───────────┴────┐
                                       │   poller (poller.h) │
                                       ├──────────┬──────────┤
                                       │  epoll   │  kqueue  │
                                       └──────────┴──────────┘
```

### Poller layer

`poller.h` is the only place that knows about the OS readiness API. Everything above it is portable C11.

```c
enum { POLLER_READ = 1u << 0, POLLER_WRITE = 1u << 1 };

typedef struct poller poller_t;
typedef struct { int fd; unsigned ready; } poller_event_t;  /* ready: POLLER_READ | POLLER_WRITE */

poller_t *poller_new(void);
void      poller_free(poller_t *p);
int       poller_arm(poller_t *p, int fd, unsigned interest);     /* set the fd's interest; one-shot */
int       poller_forget(poller_t *p, int fd);                     /* before close() */
int       poller_wait(poller_t *p, poller_event_t *out, int max,
                      int timeout_ms);                            /* -1 = no timeout */
```

**Contract: one-shot, level-triggered.**

- `poller_arm` replaces the fd's interest with `interest`. Passing 0 disarms the fd.
- An armed interest delivers at most one event. After an event for an fd, the caller treats *all* of that fd's interest as disarmed, and arms again whatever it still needs.
- Re-arming while the condition still holds fires again; the readiness is level-based.
- Errors and hang-ups count as readiness for every armed direction, so the retried system call reports the real error.
- Readiness is only a hint. Callers always ask the kernel again, and re-arm on `EAGAIN`. An event for a direction that nobody waits on is ignored.
- **The poller keeps no per-fd state.** The armed mask lives in its caller, which is the reactor's fd table.
- `poller_wait` returns the number of events, or 0 on timeout. It treats `EINTR` as 0 events.

The design is one-shot and level-triggered because that gives no missed wake-ups, no busy loop on a level that stays ready, and no duplicate events, at the cost of one extra system call per arm. Edge-triggered mode is an optimization for later.

| | epoll (`poller_epoll.c`) | kqueue (`poller_kqueue.c`) |
|---|---|---|
| arm | `epoll_ctl` MOD with `EPOLLONESHOT` and the full mask; on `ENOENT`, ADD | one `kevent` call: `EV_ADD \| EV_ONESHOT` for each direction in the mask, `EV_DELETE` for each direction not in it (`EV_RECEIPT`, ignoring `ENOENT`) |
| after an event | `EPOLLONESHOT` has disabled the whole fd | only the filter that fired is gone; the other one stays until the next `poller_arm`, and may deliver an event that the caller ignores |
| error / hang-up | `EPOLLERR`, `EPOLLHUP` → ready for the armed directions | `EV_EOF`, `EV_ERROR` → same |
| forget | `EPOLL_CTL_DEL` | `EV_DELETE` for both filters, ignoring `ENOENT` |

### Run loop

```c
typedef struct { size_t tasks, timers, fds; } runtime_stats_t;

runtime_t *runtime_new(void);                        /* NULL + errno on failure */
int        runtime_run(runtime_t *rt);               /* 0, or EDEADLK */
void       runtime_free(runtime_t *rt);              /* first cancels and finishes any live task */
void       runtime_stats(const runtime_t *rt, runtime_stats_t *out);
uint64_t   runtime_now_ms(const runtime_t *rt);      /* the loop's clock */
void       runtime_set_virtual_time(runtime_t *rt);  /* for tests; see Timers */
```

```
runtime_run(rt):
    while live tasks > 0:
        poll every task that was queued when this pass began (FIFO)
        timeout = run queue not empty ? 0
                : timers pending     ? ms until the earliest deadline, rounded up
                : fds armed          ? -1
                : return EDEADLK          # live tasks, but nothing can wake them
        events = poller_wait(timeout)
        fire due timers  → settle sleep futures, time out operations,
                           cancel tasks past their deadline → wake
        for each event   → reactor retries the pending system call → settle future or re-arm
    return 0
```

- Each pass polls only the tasks that were queued when it began. A task that yields therefore runs again only after I/O has been checked, which prevents starvation.
- Rounding the timeout up avoids a busy loop on a remainder of less than 1 ms.
- **`runtime_stats`** counts the tasks not yet freed, the entries in the timer heap and the entries in the fd table. Every test ends by checking that all three are 0. This is how end-goal criterion 3 is observed, and it also works on macOS, where LeakSanitizer is not available.
- **`runtime_free` is a bounded shutdown.** If tasks are still live, for example after `EDEADLK`, it cancels each one with `ECANCELED` and polls until all of them have finished. This always ends, because a cancelled task cannot start an operation and cleanup cannot suspend. It then frees everything, including finished tasks whose handle was never consumed.

### Tasks: the task contract

```c
typedef enum { POLL_PENDING, POLL_READY } poll_t;
typedef poll_t (*task_fn)(task_t *t);

task_t    *task_spawn(runtime_t *rt, task_fn fn, size_t frame_size); /* NULL + errno on failure */
void      *task_frame(task_t *t);           /* zeroed at spawn, stable for the task's life */
runtime_t *task_runtime(const task_t *t);   /* e.g. to spawn from inside a task */
int        task_error(const task_t *t);     /* final error code once finished */
void       task_detach(task_t *t);          /* nobody will join; free it when it finishes */
void       task_join(task_t *t, task_t *child, future_t *f);  /* value: the child's error code */
void       task_cancel(task_t *t, int reason);
void       task_set_deadline(task_t *t, uint64_t ms_from_now);  /* cancel with ETIMEDOUT */
void       task_clear_deadline(task_t *t);
```

- **Spawning.** A new task is queued and first polled on the next pass. The caller can fill in the frame after `task_spawn` returns.
- **`POLL_PENDING`.** The task has arranged to be woken, either by awaiting a pending future or by yielding. The macros guarantee this. A task that returns `POLL_PENDING` without arranging a wake is never polled again, and if nothing else is live the loop returns `EDEADLK`.
- **`POLL_READY`.** The task is finished. It is never polled again, and its error code is final. Its deadline is cleared, and any future of it that is still pending is dropped.
- **Wake during a poll.** A task can be woken while it is being polled, for example when it cancels itself. The wake is remembered: if the poll returns `POLL_PENDING`, the task is queued again, and if it returns `POLL_READY`, the wake is discarded. The run queue therefore never holds a finished task.
- **Handles.** Every handle returned by `task_spawn` is consumed exactly once: by `task_detach`, or by a join that settles.
  - A consumed handle must not be used again, because the task may already be freed. Only the task itself may go on using its own `t`.
  - A join that is dropped, because the joiner was cancelled or the join timed out, does not consume the handle. The joiner still owns it.
  - `task_cancel(NULL, …)` and `task_detach(NULL)` do nothing, so cleanup code can call them without a check.
  - A task is freed once it has finished *and* its handle has been consumed. A finished task with an unconsumed handle stays in `runtime_stats`, which is how tests catch a leaked handle.
- **Join.** A join never fails. Its value is the child's error code: 0, the child's failure, or the child's cancel reason. `AWAIT` therefore does not turn a child's failure into the parent's; the parent decides:

  ```c
  task_join(t, f->child, &f->joined);
  AWAIT(t, &f->joined);
  f->child = NULL;                    /* the join settled: the handle is consumed */
  if (future_value(&f->joined) != 0)
      ASYNC_FAIL(t, (int)future_value(&f->joined));
  ```
- **Opaque fields.** The fields of `task_t` are visible only because the macros need them. Use the accessors.

Lifecycle: `spawned → queued → polled → (suspended → queued → polled)* → finished → freed`.

### Futures

```c
typedef struct future future_t;          /* embedded in a frame; zeroed = idle */

bool     future_settled(const future_t *f);
int      future_error(const future_t *f);   /* 0 or error code, once settled */
intptr_t future_value(const future_t *f);   /* e.g. bytes read, accepted fd */

/* producer side: how an operation is written */
typedef void (*future_drop_fn)(future_t *f, void *ctx);   /* forget f */

bool future_begin(task_t *t, future_t *f, future_drop_fn drop, void *ctx);
void future_complete(future_t *f, intptr_t value);
void future_fail(future_t *f, int error);   /* error must be non-zero */
```

- A future is one-shot and belongs to one task, its waiter. The waiter is the task that started the operation, and only that task may `AWAIT` the future.
- **Every operation starts with `future_begin`.** It resets `f` to pending, links it to the waiter `t`, and records the producer's drop hook. If `t` is cancelled, it returns `false` and leaves `f` idle, and the operation must then do nothing.
- **Settling** a future unlinks it from its waiter and wakes the waiter.
- **Dropping.** The runtime calls the drop hook of every future that is still pending when its waiter is cancelled or finishes, and when the future times out. The producer must forget `f` and never touch it again. The reactor, the timer heap and join all work this way, and so must an operation written in user code, so a producer never keeps a pointer into a frame that is gone.
- **A producer that goes away first settles its futures first.** A producer task fails the futures it still holds, for example with `ECANCELED`, in its cleanup.
- A future must not be copied, moved or reused while it is pending.
- An operation reports failure **only** through its future, never through a return value. One path, `AWAIT`, therefore handles every async error.

### Async macros

The macros live in `async.h`. Tasks are stackless coroutines: the macros turn the poll function's body into a `switch` on the saved resume point (the protothreads technique, also known as Duff's device).

| Macro | Effect |
|---|---|
| `ASYNC_BEGIN(t)` | Opens the body. A task cancelled before its first poll goes straight to cleanup. |
| `AWAIT(t, f)` | Suspends until `f` settles. If `f` failed, sets the task's error and jumps to cleanup. |
| `AWAIT_TRY(t, f)` | Suspends until `f` settles and does not jump on failure. Inspect `future_error(f)`. |
| `YIELD(t)` | Re-queues the task and suspends. Use it inside long CPU loops. A cancelled task jumps to cleanup instead. |
| `ASYNC_FAIL(t, err)` | Sets the task's error to `err` (non-zero) and jumps to cleanup. |
| `ASYNC_CLEANUP(t)` | Starts the cleanup section. Success, failure and cancellation all end here. |
| `ASYNC_END(t)` | Closes the body and marks the task finished. |

Simplified expansion:

```c
#define ASYNC_BEGIN(t)  switch ((t)->resume) { case 0: \
                            if ((t)->flags & TASK_CANCELLED) goto async_cancelled

#define AWAIT_TRY(t, f)                                              \
    do {                                                             \
        (t)->resume = __LINE__;                                      \
        ASYNC_FALLTHROUGH;                                           \
    case __LINE__:                                                   \
        if (!future_settled(f)) {                                    \
            if ((t)->flags & TASK_CANCELLED) goto async_cancelled;   \
            return POLL_PENDING;    /* settling f wakes t */         \
        }                                                            \
        if (!task_take_budget(t))                                    \
            return POLL_PENDING;    /* budget spent: t is queued */  \
    } while (0)
```

- **The order of the two checks is the cancellation rule.** A settled future passes even when the task has been cancelled. Only a future that has not settled lets the cancel through.
- **`async_cancelled`** is a label inside `ASYNC_CLEANUP`. It stores the cancel reason as the task's error and then falls into the cleanup code.
- **`task_take_budget` is the cooperative budget** (Tokio uses the same idea). Each poll may pass at most 32 `AWAIT`s whose future has already settled. The next one queues the task again and delivers the result on the following pass. Without this budget, a connection that always has data would starve every other task. The budget sits in `AWAIT`, so it covers every kind of future and needs no extra poller call.

`ASYNC_FALLTHROUGH` expands to `__attribute__((fallthrough))` on gcc and clang, and to nothing elsewhere. gcc's `-Wextra` enables `-Wimplicit-fallthrough`, which flags every `case __LINE__:`. Without the attribute, the warning flags turn that into a build error. This is the only compiler-specific construct, and it sits behind a macro. It isn't GNU computed goto, so the code stays ISO C11.

**Rules for task functions.** The macros cannot enforce all of these, so they are part of the contract.

1. **State that must survive a suspension lives in the frame.**
   - Locals declared *before* `ASYNC_BEGIN` are recomputed on every poll, so they are safe. The frame pointer is the usual one.
   - Locals inside the body lose their values at every `AWAIT` and `YIELD`.
2. **No `AWAIT` or `YIELD` inside a `switch` statement**, because the macros are `case` labels. Use `if`/`else`. Loops are fine. A violation compiles, but it is caught at run time: the task aborts when it is resumed.
3. **At most one `AWAIT` or `YIELD` per source line**, because `__LINE__` is the resume ID.
4. **`AWAIT` evaluates its future argument more than once.** Pass a plain pointer, and start the operation on the line before it.
5. **Only the task function can suspend**; helper functions cannot `AWAIT`. To reuse async logic, spawn a child task and join it.
6. **Each task function has exactly one `ASYNC_CLEANUP`.**
   - The code after it must not `AWAIT` or `YIELD`.
   - It must be safe in any frame state, including before the body ran.
   - Frames are zeroed, so a zero fd is a valid fd (stdin). Set fd fields to -1 right after spawning.
7. **One operation per future at a time.** Never start an operation on a pending future.
8. **Take a result out of its future right after the `AWAIT`.** Copy an accepted fd or a spawned handle into a frame field before the next `AWAIT` or `YIELD`. Cleanup looks at frame fields, not at futures.

### Timers

```c
void sleep_ms(task_t *t, future_t *f, uint64_t ms);   /* completes with value 0 */
void future_set_timeout(future_t *f, uint64_t ms);    /* fail a pending f with ETIMEDOUT after ms */
```

- The timer heap is a binary min-heap of deadlines on `CLOCK_MONOTONIC`, in milliseconds. It holds two kinds of entry: future timers (sleeps and operation timeouts) and task deadlines. Each entry is embedded in its future or its task.
- Each entry stores its heap index, so cancelling an entry is O(log n).
- Ties fire in insertion order, so tests are deterministic.
- `sleep_ms(t, f, 0)` completes on the next pass, after I/O has been checked.
- The earliest deadline sets the loop's wait timeout. Timers are part of the core because they are the simplest async source, and because the loop cannot compute its timeout without them.
- **There are two ways to time out**, and neither is a combinator.
  - **A task deadline ends the task.** `task_set_deadline(t, ms)` cancels the task with `ETIMEDOUT` when the deadline passes. For an idle timeout, reset the deadline after each received message. A task's deadline is cleared when the task finishes.
  - **An operation timeout fails one future.** Call `future_set_timeout(f, ms)` right after starting an operation. If `f` is still pending after `ms`, the operation is dropped and `f` fails with `ETIMEDOUT`. With `AWAIT_TRY`, the task can handle that and carry on:

    ```c
    io_read(t, &f->rd, f->fd, f->buf, sizeof f->buf);
    future_set_timeout(&f->rd, 5000);
    AWAIT_TRY(t, &f->rd);
    if (future_error(&f->rd) == ETIMEDOUT) {
        /* nothing arrived for 5 s: send a ping, then read again */
    }
    ```

    It does nothing on a future that is settled or idle. A sleep cannot have a timeout.
- **Virtual time for tests.** After `runtime_set_virtual_time(rt)`, the clock starts at 0 and the loop never sleeps for a timer. When it would wait for a timer, it polls with a zero timeout instead and, if there are no events, moves the clock to the earliest deadline. Timer tests then run instantly and give the same result on a loaded machine. Tokio's paused clock works the same way.

### Reactor and I/O

```c
void io_read     (task_t *t, future_t *f, int fd, void *buf, size_t len);       /* value: bytes, 0 = EOF */
void io_write    (task_t *t, future_t *f, int fd, const void *buf, size_t len); /* value: bytes (>= 1) */
void io_write_all(task_t *t, future_t *f, int fd, const void *buf, size_t len); /* value: len */
void io_accept   (task_t *t, future_t *f, int listen_fd);                      /* value: new fd (non-blocking) */
void io_connect  (task_t *t, future_t *f, int fd, const struct sockaddr *a, socklen_t alen);
int  io_close    (task_t *t, int fd);    /* forgets the fd in the poller and the fd table, then close()s it */
int  fd_set_nonblocking(int fd);
```

- **The reactor runs the system call, not the task.** An operation first tries the call immediately. On `EAGAIN` it arms the poller; on readiness it retries the call; then it settles the future with the result or the error. `EAGAIN` and `EINTR` never reach task code.
- **`io_connect` is not retried.** A second `connect` on the same socket would report `EALREADY` or `EISCONN`. On `EINPROGRESS` or `EINTR` the reactor arms the fd for writing, and on readiness it reads the result with `getsockopt(SO_ERROR)`.
- **Fd table.** The table is indexed by fd. Each entry records the armed mask (the poller keeps none), at most one pending reader and at most one pending writer; a second pending reader or writer is a programming error.
- **Cooperative budget.** An operation that can finish at once does so, but a task cannot use that to run forever. The limit is in `AWAIT`; see [Async macros](#async-macros).
- **Close with `io_close`, never plain `close()`.** Fd numbers are reused, and a stale table entry would wake the wrong task. Do not close an fd that another task is waiting on; cancel that task first.
- **Regular files are rejected.** `io_*` on a regular file fails with `EINVAL` on every backend: the reactor checks with `fstat` when it first sees the fd. epoll would refuse such an fd with `EPERM`, while kqueue would report it as always ready, so the reactor rejects it the same way on both backends.

### Errors

There are four kinds of error, and each has one path:

| Kind | Example | How it is reported |
|---|---|---|
| Operation error | `ECONNRESET` from a read | The future fails, and `AWAIT` propagates the error to cleanup. `AWAIT_TRY` lets the task handle it instead. |
| Task outcome | A task failed or was cancelled | `task_error(t)`, and the joiner's future. |
| Synchronous setup error | `task_spawn` out of memory | Return value: an error code, or `NULL` plus `errno`. |
| Programming error | An operation on a pending future, `AWAIT` on a future with no operation started, `future_fail(f, 0)`, `AWAIT` in cleanup | `assert` and abort. Asserts stay enabled in every build. |

A task's error code is set by the first of these: a failing `AWAIT`, `ASYNC_FAIL`, or a cancellation that takes effect at a resume point. If none of them happens, it stays 0. The error of a detached task is dropped, so log it in the cleanup section if it matters.

### Cancellation

`task_cancel(t, reason)`:

1. **It acts immediately.** Every pending future of the task is dropped: a sleep leaves the heap, an I/O operation leaves the fd table and its interest is disarmed, a join is unlinked from the child. A dropped future stays unsettled.
2. **The rest is deferred.** The task is flagged and woken. At its next resume point it jumps to cleanup, with `reason` as its error code. The resume points are `ASYNC_BEGIN`, `YIELD`, and an `AWAIT` whose future has not settled.

Rules:

- **Cooperative.** A task that never reaches a resume point cannot be cancelled. That is one more reason to `YIELD` in long loops.
- **The first reason wins.** Cancelling a task that has finished or is already cancelled does nothing.
- **A settled future is always delivered.** If an operation completed before the cancel, its `AWAIT` passes and the task runs on to its next resume point. The reactor may already have read the data or accepted the connection, and discarding that result would lose bytes or leak an fd. This is why rule 8 says to take a result out of its future right away.
- **A cancelled task cannot start operations.** An operation started by a cancelled task does nothing and leaves its future idle, so the `AWAIT` that follows jumps to cleanup.
- **A pending operation is all or nothing.** A dropped operation has no effect: the read or accept did not happen. The exception is `io_write_all`, which may already have written a prefix.
- **A late cancel changes nothing.** If the task reaches `ASYNC_CLEANUP` on its own before a resume point sees the flag, its error code is not replaced by the reason.
- **Cleanup cannot be cancelled**, because it cannot suspend.
- **Not structured.** Cancelling a task does not cancel the tasks it spawned. A parent that wants that behavior cancels and detaches its children in its own cleanup. It cannot wait for them there, because cleanup cannot suspend; [Ownership and lifetimes](#ownership-and-lifetimes) says what that rules out.
- **Join sees the reason.** A joiner of a cancelled task gets the cancel reason as the join's value.
- **Shutdown is bounded.** Because cleanup cannot suspend, cancelling every task finishes all of them without waiting for I/O. `runtime_free` relies on this.

A parent's cleanup section for a child it may still own:

```c
ASYNC_CLEANUP(t);
task_cancel(f->child, ECANCELED);   /* both do nothing if f->child is NULL */
task_detach(f->child);
```

### Ownership and lifetimes

| Resource | Owner | Freed when |
|---|---|---|
| Task and frame | Runtime | The task has finished and its handle has been consumed (detached, or a join has settled) |
| Task handle | Whoever called `task_spawn` | Consumed by `task_detach`, or by a join that settles |
| Future | The frame of the task that started its operation | Together with the frame. Producers hold a pointer only while it is pending, and drop it on request. |
| fd | The task that opened or accepted it | `io_close` in that task's cleanup section |
| Timer entry | Its future or its task | When it fires, when its future is dropped, or when its task clears the deadline or finishes |
| fd table entry | Reactor | `io_close` |

- **The runtime may point into a frame.** The frame's address is stable, so futures and I/O buffers in it can be handed to producers. Dropping removes those pointers before the frame goes away.
- **A task must not point into another task's frame.** A parent can finish, and its frame be freed, while tasks it spawned are still running, and it cannot wait for them in cleanup. State shared between tasks therefore lives outside every frame: in static storage, or in heap memory that outlives its users.

### Blocking work

Anything that blocks a task blocks the whole loop. The rules:

- **Fds given to `io_*` must be non-blocking.** `io_accept` returns non-blocking fds. For any other fd, use `fd_set_nonblocking`.
- **File I/O and DNS lookups are out of scope.** Use regular-file reads, `getaddrinfo` and blocking `connect` only before `runtime_run`, or in examples where a stall is acceptable. Examples use numeric addresses.
- **CPU-heavy loops must `YIELD`** every few hundred iterations.
- **Writes to the terminal block too.** `printf` to a terminal is acceptable for examples and tests. Output to a slow pipe is not.
- **Signals.**
  - `runtime_new` sets `SIGPIPE` to `SIG_IGN`, so writing to a closed peer fails with `EPIPE` instead of killing the process.
  - Handling `SIGINT` and other signals is an extension, done with a self-pipe.
- **No thread pool.** The runtime is single-threaded, so there is nowhere to offload blocking work. That is the price of the non-goal above.

---

## ✨ Example

The target for M3:

```c
#include <stdio.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/async.h"
#include "runtime/timer.h"

struct ticker {
    int      i;       /* survives AWAIT: lives in the frame */
    future_t tick;
};

static poll_t ticker(task_t *t)
{
    struct ticker *f = task_frame(t);   /* recomputed on every poll: safe */

    ASYNC_BEGIN(t);
    task_set_deadline(t, 250);          /* cancel with ETIMEDOUT after 250 ms */
    for (f->i = 0; f->i < 5; f->i++) {
        printf("tick %d\n", f->i);
        sleep_ms(t, &f->tick, 100);     /* start the operation ... */
        AWAIT(t, &f->tick);             /* ... then suspend until it settles */
    }
    ASYNC_CLEANUP(t);                   /* success, failure and cancellation end here */
    printf("ticker finished: %s\n", task_error(t) ? strerror(task_error(t)) : "ok");
    ASYNC_END(t);
}

int main(void)
{
    runtime_t *rt = runtime_new();
    if (rt == NULL)
        return 1;
    task_t *t = task_spawn(rt, ticker, sizeof(struct ticker));
    if (t == NULL)
        return 1;
    task_detach(t);
    int err = runtime_run(rt);          /* returns when no task is left */
    runtime_free(rt);
    return err != 0;
}
```

Expected output: `tick 0`, `tick 1` and `tick 2`, then `ticker finished:` followed by the OS's text for `ETIMEDOUT`. The deadline at 250 ms interrupts the third sleep, which would end at 300 ms.

In real time this output depends on the loop running less than 50 ms late in total, so the M3 test runs the same task under virtual time, where the output is exact.

---

## ⚠️ Constraints

- **Language.** ISO C11, `-std=c11 -Wall -Wextra -Wpedantic -Werror`. The only compiler-specific construct is `ASYNC_FALLTHROUGH`, behind a macro.
- **Feature macros.** Portable files define `_POSIX_C_SOURCE 200809L`. The OS APIs (`sys/epoll.h`, `sys/event.h`) are included only by their poller backend.
- **Dependencies.** libc and OS system calls only, for the runtime and for the tests.
- **Compilers.** clang and gcc.
- **Platforms.** Linux (epoll) and macOS (kqueue) are tested. FreeBSD should work with the kqueue backend but is not tested.
- **Threading.** Single-threaded: every runtime call is made from the thread that runs `runtime_run`. Runtime functions are not async-signal-safe.
- **Sanitizers.** Tests run under ASan and UBSan by default. LeakSanitizer runs only on Linux, because Apple clang on Apple Silicon rejects `-fsanitize=leak`. On macOS, leaked tasks, timers and fd table entries are caught by the `runtime_stats` check instead.

---

## 📂 Layout

```
runtime/
  runtime.c  runtime.h   runtime_new / runtime_run / runtime_free, the loop
  task.c     task.h      spawn, wake, detach, join, cancel, deadlines
  future.c   future.h    one-shot futures
  async.h                ASYNC_BEGIN, AWAIT, AWAIT_TRY, YIELD, ASYNC_FAIL, ASYNC_CLEANUP, ASYNC_END
  internal.h             shared by the runtime's own .c files; not part of the API
  timer.c    timer.h     deadline min-heap, sleep_ms
  reactor.c  reactor.h   fd table, io_* operations, io_close
  poller.h               backend interface
  poller_epoll.c         Linux
  poller_kqueue.c        macOS, FreeBSD
examples/
  countdown.c            M1
  ticker.c               M3 (the example above)
  pipe_pingpong.c        M4
  echo_server.c          M5
tests/
  test_*.c               one program per test; exit code 0 means pass
  test.h                 CHECK and the runtime_stats check that ends every test
Makefile
```

---

## 🛠️ Getting Started

Requirements:

- a C11 compiler (clang or gcc)
- `make`
- on macOS, Docker, to build and test the epoll backend

```bash
git clone https://github.com/ofkindness/toronto.git
cd toronto
```

These commands are available from M0:

```bash
make                  # runtime, examples and tests for this OS (epoll on Linux, kqueue on macOS/FreeBSD)
make test             # run all tests under ASan and UBSan
make check-linux      # the same build and tests in Docker (gcc:14) with the epoll backend
make POLLER=kqueue    # force a backend
./build/darwin-kqueue/examples/ticker
```

Build output goes to `build/<os>-<poller>/`, for example `build/darwin-kqueue/` or `build/linux-epoll/`. The macOS build and the Docker build share the source directory, and separate build directories keep their object files apart.

`make check-linux` is a wrapper around:

```bash
docker run --rm -v "$PWD":/src -w /src gcc:14 make test
```

---

## 🧪 Milestones

Each milestone is done only when its tests pass under `make test` **and** `make check-linux`. From M1 on, every test ends by checking that `runtime_stats` reports all zeros.

### M0 — Tooling
- Makefile with backend selection (`POLLER`), the compiler flags above, sanitizers on by default, and one build directory per OS and backend.
- A test runner: one program per test, where exit code 0 means pass.
- A `check-linux` target, and `build/` added to `.gitignore`.
- **Done:** a trivial test passes on both backends.

### M1 — Coroutines without I/O
- `task_t`, `async.h` (`ASYNC_BEGIN`, `YIELD`, `ASYNC_FAIL`, `ASYNC_CLEANUP`, `ASYNC_END`), a FIFO run queue, `task_detach`, `task_runtime` and `runtime_stats`.
- `task_detach` is here, not in M2, because it is the only way to consume a handle before join exists. Without it no test could end with `runtime_stats` at zero.
- No OS calls yet. With no timers and no I/O, the loop returns `EDEADLK` as soon as tasks are live and the run queue is empty. `runtime_free` frees a task that is still live without running its cleanup; M2 replaces that with the shutdown.
- Remove `examples/async_dsl.c`.
- **Done:**
  - two tasks interleave in the expected order, and a task spawned during a pass first runs on the next pass;
  - a test demonstrates rule 1 (locals lose their values, frame fields don't);
  - `ASYNC_FAIL` reaches cleanup with its code;
  - a finished task stays in `runtime_stats` until its handle is detached;
  - a stuck task makes `runtime_run` return `EDEADLK`;
  - a `YIELD` in cleanup, `ASYNC_FAIL(t, 0)` and a `YIELD` inside a `switch` each abort;
  - `examples/countdown.c` prints the expected output;
  - no warnings on gcc or clang.

### M2 — Futures, wake, join, cancellation
- `future_t` with `future_begin` and the drop hook, `AWAIT`, `AWAIT_TRY`, the cooperative budget, the duplicate-wake guard, `task_join`, `task_cancel` and the shutdown in `runtime_free`.
- The only built-in operation so far is join, so the tests define their own operation on top of `future_begin`.
- **Done:**
  - a consumer awaits a future that a producer task completes;
  - a failed future propagates to cleanup;
  - a double wake polls only once, and a wake during a poll queues the task again;
  - cancellation before the first poll and while awaiting both reach cleanup with the reason;
  - a future that settled before the cancel is still delivered;
  - a task that awaits 100 already settled futures in a row is polled four times;
  - cancelling the consumer calls the producer's drop hook;
  - a join completes with the child's error code as its value, and a cancelled join leaves the handle with the joiner;
  - a handle that is never consumed shows up in `runtime_stats`;
  - `runtime_free` finishes tasks that are still live;
  - no leaks.

### M3 — Poller and timers
- `poller.h` with both backends, the timer heap, `sleep_ms`, task deadlines, `future_set_timeout`, virtual time, the loop's timeout computation and `EDEADLK` detection.
- The timer tests run under virtual time.
- **Done:**
  - three sleeps finish in deadline order;
  - a cancelled sleep leaves the heap;
  - a deadline cancels a task with `ETIMEDOUT`;
  - a task that finishes with a deadline still set leaves no timer entry;
  - a join with a timeout fails with `ETIMEDOUT`, and the task then joins the same child again;
  - a pipe test confirms one-shot semantics on both backends;
  - the ticker task prints the expected output under virtual time, and `examples/ticker.c` runs in real time.

### M4 — Reactor and non-blocking I/O
- The fd table, the `io_*` operations, `io_close`, `SIGPIPE` handling and `EINTR` handling.
- **Done:**
  - 10,000 round-trips of pipe ping-pong between two tasks;
  - a reader cancelled while waiting leaves no armed interest;
  - a read that completed before a cancel is delivered, and an accept that was still pending leaves no fd behind;
  - writing to a closed peer gives `EPIPE`;
  - connecting to a port with no listener gives `ECONNREFUSED`;
  - a regular file gives `EINVAL`;
  - one busy connection does not starve another.

### M5 — Echo server
- `examples/echo_server.c` and `tests/test_echo_load.c`.
- **Done:** every criterion under [End Goal](#-end-goal) is met.

---

## 🔮 Possible Extensions (after M5)

- Signal handling with a self-pipe, for example a graceful shutdown on `SIGINT`.
- Combinators over several futures, such as `race` and `all`. These would need futures with more than one waiter, or a waiter that waits on a set.
- Nested coroutines: a child state machine that lives in its parent's frame and is polled by the parent. Today every reusable async helper costs a task (rule 5). Nesting removes that cost, and it is how Rust futures compose.
- A cleanup section that can suspend, with cancellation masked, so that a parent can join its children before it finishes.
- Optimizations, measured before and after:
  - memory pools for tasks and frames;
  - a timer wheel instead of the heap;
  - edge-triggered polling.
- An io_uring backend. It is completion-based, so it does not fit the readiness contract of `poller.h` and would plug in at the reactor level. That contrast is a lesson in itself.

---

## 🧠 What You Will Learn

- How readiness-based I/O works, and how epoll and kqueue differ underneath one interface
- What changes when the reactor, not the task, makes the system call
- Why async/await is "just" a state machine, and where stackless coroutines stop being convenient
- The wake path from a kernel event to a resumed task
- How timers drive the event loop's sleep
- Why cancellation, ownership and lifetimes are the hard parts of async, not the scheduler

---

## 🧩 Philosophy

> Async is not magic — it is controlled execution + scheduling.
