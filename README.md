# toronto

A minimal, single-threaded async runtime in C11, built from scratch to learn how runtimes like Tokio work inside.

**Status: design proposal.** This README is the design. No milestone is implemented yet. `examples/async_dsl.c` is an early sketch of the error-handling macros. It is superseded by the design below and will be removed in M1.

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
2. A client that stays silent is disconnected by its idle deadline. The test uses a 200 ms deadline.
3. Afterwards, `runtime_run` returns 0, and no tasks, fds or timers are left.
4. The run is clean under AddressSanitizer and UndefinedBehaviorSanitizer, plus LeakSanitizer on Linux.
5. The test prints throughput in messages per second. This is for information only; there is no threshold.

The test raises its own `RLIMIT_NOFILE` soft limit, because macOS defaults to 256 open files.

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
| **wake** | Put a task on the run queue. Waking a queued or finished task does nothing. |
| **future** | A one-shot completion slot: *pending*, then *completed* with a value or *failed* with an error. It has exactly one waiter and lives in the waiter's frame. |
| **operation** | A call that starts async work and later settles a future: `sleep_ms`, `io_read`, `task_join`, … |
| **producer** | The component that settles a future: the timer heap, the reactor, or a task. |
| **poller** | The OS readiness abstraction (`poller.h`), with epoll and kqueue backends. |
| **reactor** | Maps poller events to pending I/O operations, and runs the system calls for them. |
| **deadline** | An absolute time on the monotonic clock. When it passes, the timer heap acts. |
| **cancel** | Ask a task to stop, with a reason code. Cancellation is cooperative. |
| **error code** | An `int`: 0 means success; anything else is a positive `errno` value. |

There are no "promises". The producer side of a future is just `future_complete` / `future_fail`.

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
int       poller_arm(poller_t *p, int fd, unsigned interest);     /* one-shot, see below */
int       poller_forget(poller_t *p, int fd);                     /* before close() */
int       poller_wait(poller_t *p, poller_event_t *out, int max,
                      int timeout_ms);                            /* -1 = no timeout */
```

**Contract: one-shot, level-triggered.**

- An armed interest delivers at most one event and is then disarmed.
- Re-arming while the condition still holds fires again; the readiness is level-based.
- Errors and hang-ups count as readiness for every armed direction, so the retried system call reports the real error.
- Readiness is only a hint. Callers always retry the system call, and re-arm on `EAGAIN`.
- `poller_wait` returns the number of events, or 0 on timeout. It treats `EINTR` as 0 events.

The design is one-shot and level-triggered because that gives no missed wake-ups, no busy loop on a level that stays ready, and no duplicate events, at the cost of one extra system call per arm. Edge-triggered mode is an optimization for later.

| | epoll (`poller_epoll.c`) | kqueue (`poller_kqueue.c`) |
|---|---|---|
| arm | `epoll_ctl` ADD/MOD with `EPOLLONESHOT` and the fd's full armed mask | `kevent` with `EV_ADD \| EV_ONESHOT` per filter (`EVFILT_READ`, `EVFILT_WRITE`) |
| one-shot gotcha | `EPOLLONESHOT` disables the whole fd, so the backend re-arms the direction that did not fire | filters are independent; nothing to do |
| error / hang-up | `EPOLLERR`, `EPOLLHUP` → ready for the armed directions | `EV_EOF`, `EV_ERROR` → same |
| forget | `EPOLL_CTL_DEL` | `EV_DELETE` for armed filters, ignoring `ENOENT` |

### Run loop

```
runtime_run(rt):
    while live tasks > 0:
        poll every task that was queued when this pass began (FIFO)
        timeout = run queue not empty ? 0
                : timers pending     ? ms until the earliest deadline, rounded up
                : fds armed          ? -1
                : return EDEADLK          # live tasks, but nothing can wake them
        events = poller_wait(timeout)
        fire due timers  → settle sleep futures, cancel tasks past their deadline → wake
        for each event   → reactor retries the pending system call → settle future or re-arm
    return 0
```

- Each pass polls only the tasks that were queued when it began. A task that yields therefore runs again only after I/O has been checked, which prevents starvation.
- Rounding the timeout up avoids a busy loop on a remainder of less than 1 ms.

### Tasks: the task contract

```c
typedef enum { POLL_PENDING, POLL_READY } poll_t;
typedef poll_t (*task_fn)(task_t *t);

task_t *task_spawn(runtime_t *rt, task_fn fn, size_t frame_size); /* NULL + errno on failure */
void   *task_frame(task_t *t);           /* zeroed at spawn, stable for the task's life */
int     task_error(const task_t *t);     /* final error code once finished */
void    task_detach(task_t *t);          /* nobody will join; free it when it finishes */
void    task_join(task_t *child, future_t *f);  /* f settles with the child's error code */
void    task_cancel(task_t *t, int reason);
void    task_set_deadline(task_t *t, uint64_t ms_from_now);  /* cancel with ETIMEDOUT */
void    task_clear_deadline(task_t *t);
```

- **Spawning.** A new task is queued and first polled on the next pass. The caller can fill in the frame after `task_spawn` returns.
- **`POLL_PENDING`.** The task has arranged to be woken, either by awaiting a future or by yielding. The macros guarantee this. A task that returns `POLL_PENDING` without arranging a wake is never polled again, and if nothing else is live the loop returns `EDEADLK`.
- **`POLL_READY`.** The task is finished. It is never polled again, and its error code is final.
- **Handles.** Every handle returned by `task_spawn` is either detached or joined, exactly once; LeakSanitizer catches the rest. A task is freed once it has finished *and* been detached, or once its join has settled.
- **Opaque fields.** The fields of `task_t` are visible only because the macros need them. Use the accessors.

Lifecycle: `spawned → queued → polled → (suspended → queued → polled)* → finished → freed`.

### Futures

```c
typedef struct future future_t;          /* embedded in a frame; zeroed = idle */

int      future_error(const future_t *f);   /* 0 or error code, once settled */
intptr_t future_value(const future_t *f);   /* e.g. bytes read, accepted fd */

/* producer side */
void future_complete(future_t *f, intptr_t value);
void future_fail(future_t *f, int error);   /* error must be non-zero */
```

- A future is one-shot and has a single waiter. Starting an operation resets the future it is given.
- A future must not be copied, moved or reused while it is pending.
- An operation reports failure **only** through its future, never through a return value. One path, `AWAIT`, therefore handles every async error.

### Async macros

The macros live in `async.h`. Tasks are stackless coroutines: the macros turn the poll function's body into a `switch` on the saved resume point (the protothreads technique, also known as Duff's device).

| Macro | Effect |
|---|---|
| `ASYNC_BEGIN(t)` | Opens the body. A task cancelled before its first poll goes straight to cleanup. |
| `AWAIT(t, f)` | Suspends until `f` settles. If `f` failed, sets the task's error and jumps to cleanup. |
| `AWAIT_TRY(t, f)` | Suspends until `f` settles and does not jump on failure. Inspect `future_error(f)`. |
| `YIELD(t)` | Re-queues the task and suspends. Use it inside long CPU loops. |
| `ASYNC_FAIL(t, err)` | Sets the task's error to `err` (non-zero) and jumps to cleanup. |
| `ASYNC_CLEANUP(t)` | Starts the cleanup section. Success, failure and cancellation all end here. |
| `ASYNC_END(t)` | Closes the body and marks the task finished. |

Simplified expansion:

```c
#define ASYNC_BEGIN(t)  switch ((t)->resume) { case 0: \
                            if ((t)->flags & TASK_CANCELLED) goto async_cleanup

#define AWAIT_TRY(t, f)                                          \
    do {                                                         \
        (t)->resume = __LINE__;                                  \
        ASYNC_FALLTHROUGH;                                       \
    case __LINE__:                                               \
        if ((t)->flags & TASK_CANCELLED) goto async_cleanup;     \
        if (!future_settled(f)) {                                \
            future_set_waiter((f), (t));                         \
            return POLL_PENDING;                                 \
        }                                                        \
    } while (0)
```

`ASYNC_FALLTHROUGH` expands to `__attribute__((fallthrough))` on gcc and clang, and to nothing elsewhere. gcc's `-Wextra` enables `-Wimplicit-fallthrough`, which flags every `case __LINE__:`. Without the attribute, the warning flags turn that into a build error. This is the only compiler-specific construct, and it sits behind a macro. It isn't GNU computed goto, so the code stays ISO C11.

**Rules for task functions.** The macros cannot enforce all of these, so they are part of the contract.

1. **State that must survive a suspension lives in the frame.**
   - Locals declared *before* `ASYNC_BEGIN` are recomputed on every poll, so they are safe. The frame pointer is the usual one.
   - Locals inside the body lose their values at every `AWAIT` and `YIELD`.
2. **No `AWAIT` or `YIELD` inside a `switch` statement**, because the macros are `case` labels. Use `if`/`else`. Loops are fine.
3. **At most one `AWAIT` or `YIELD` per source line**, because `__LINE__` is the resume ID.
4. **`AWAIT` evaluates its future argument more than once.** Pass a plain pointer, and start the operation on the line before it.
5. **Only the task function can suspend**; helper functions cannot `AWAIT`. To reuse async logic, spawn a child task and join it.
6. **Each task function has exactly one `ASYNC_CLEANUP`.**
   - The code after it must not `AWAIT` or `YIELD`.
   - It must be safe in any frame state, including before the body ran.
   - Frames are zeroed, so a zero fd is a valid fd (stdin). Set fd fields to -1 right after spawning.
7. **One operation per future at a time.** Never start an operation on a pending future.

### Timers

```c
void sleep_ms(task_t *t, future_t *f, uint64_t ms);   /* completes with value 0 */
```

- The timer heap is a binary min-heap of deadlines on `CLOCK_MONOTONIC`, in milliseconds. It holds two kinds of entry: sleep futures and task deadlines.
- Each entry stores its heap index, so cancelling an entry is O(log n).
- Ties fire in insertion order, so tests are deterministic.
- `sleep_ms(t, f, 0)` completes on the next pass, after I/O has been checked.
- The earliest deadline sets the loop's wait timeout. Timers are part of the core because they are the simplest async source, and because the loop cannot compute its timeout without them.
- **Timeouts are built from deadlines and cancellation.** There is no separate timeout combinator.
  - `task_set_deadline(t, ms)` cancels the task with `ETIMEDOUT` when the deadline passes.
  - For an idle timeout, reset the deadline after each received message.

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
- **Fd table.** The table is indexed by fd. Each entry records the armed mask, at most one pending reader and at most one pending writer; a second pending reader or writer is a programming error.
- **Cooperative budget** (Tokio uses the same idea). Each poll may complete at most 32 operations immediately. After that, operations complete through the loop. Without this budget, a connection that always has data would starve every other task.
- **Close with `io_close`, never plain `close()`.** Fd numbers are reused, and a stale table entry would wake the wrong task. Do not close an fd that another task is waiting on; cancel that task first.
- **Regular files are rejected.** `io_*` on a regular file fails with `EINVAL` on every backend: the reactor checks with `fstat` when it first sees the fd. epoll would refuse such an fd with `EPERM`, while kqueue would report it as always ready, so the reactor rejects it the same way on both backends.

### Errors

There are four kinds of error, and each has one path:

| Kind | Example | How it is reported |
|---|---|---|
| Operation error | `ECONNRESET` from a read | The future fails, and `AWAIT` propagates the error to cleanup. `AWAIT_TRY` lets the task handle it instead. |
| Task outcome | A task failed or was cancelled | `task_error(t)`, and the joiner's future. |
| Synchronous setup error | `task_spawn` out of memory | Return value: an error code, or `NULL` plus `errno`. |
| Programming error | Two waiters on one future, `future_fail(f, 0)`, `AWAIT` in cleanup | `assert` and abort. Asserts stay enabled in every build. |

A task's error code is set by the first of these: a failing `AWAIT`, `ASYNC_FAIL`, or a cancel reason. If none of them happens, it stays 0. The error of a detached task is dropped, so log it in the cleanup section if it matters.

### Cancellation

`task_cancel(t, reason)`:

1. **It acts immediately.** The future the task awaits is detached from its producer: a sleep is removed from the heap, an I/O operation is dropped and its interest is disarmed.
2. **The rest is deferred.** The task is flagged and woken. At its next resume point (`ASYNC_BEGIN`, `AWAIT` or `YIELD`) it jumps to cleanup, with `reason` as its error code.

Rules:

- **Cooperative.** A task that never reaches a resume point cannot be cancelled. That is one more reason to `YIELD` in long loops.
- **The first reason wins.** Cancelling a task that has finished or is already cancelled does nothing.
- **Cleanup cannot be cancelled**, because it cannot suspend.
- **All or nothing.** A cancelled operation has no effect: the read or accept did not happen. The exception is `io_write_all`, which may already have written a prefix.
- **Not structured.** Cancelling a task does not cancel the tasks it spawned. A parent that wants that behavior cancels and joins its children in its own cleanup.
- **Join sees the reason.** A joiner of a cancelled task gets the cancel reason as the child's error.

### Ownership and lifetimes

| Resource | Owner | Freed when |
|---|---|---|
| Task and frame | Runtime | The task has finished and been detached, or its join has settled |
| Future | The frame of the task that awaits it | Together with the frame. Producers hold a pointer only while it is pending. |
| fd | The task that opened or accepted it | `io_close` in that task's cleanup section |
| Timer entry | Timer heap | When it fires, or when its future or task is cancelled |
| fd table entry | Reactor | `io_close` |

The frame's address is stable, so pointers into it can be handed to producers: futures, and I/O buffers.

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

---

## ⚠️ Constraints

- **Language.** ISO C11, `-std=c11 -Wall -Wextra -Wpedantic -Werror`. The only compiler-specific construct is `ASYNC_FALLTHROUGH`, behind a macro.
- **Feature macros.** Portable files define `_POSIX_C_SOURCE 200809L`. The OS APIs (`sys/epoll.h`, `sys/event.h`) are included only by their poller backend.
- **Dependencies.** libc and OS system calls only, for the runtime and for the tests.
- **Compilers.** clang and gcc.
- **Platforms.** Linux (epoll) and macOS (kqueue) are tested. FreeBSD should work with the kqueue backend but is not tested.
- **Threading.** Single-threaded: every runtime call is made from the thread that runs `runtime_run`. Runtime functions are not async-signal-safe.
- **Sanitizers.** Tests run under ASan and UBSan by default.

---

## 📂 Layout

```
runtime/
  runtime.c  runtime.h   runtime_new / runtime_run / runtime_free, the loop
  task.c     task.h      spawn, wake, detach, join, cancel, deadlines
  future.c   future.h    one-shot futures
  async.h                ASYNC_BEGIN, AWAIT, AWAIT_TRY, YIELD, ASYNC_FAIL, ASYNC_CLEANUP, ASYNC_END
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
./build/examples/ticker
```

`make check-linux` is a wrapper around:

```bash
docker run --rm -v "$PWD":/src -w /src gcc:14 make test
```

---

## 🧪 Milestones

Each milestone is done only when its tests pass under `make test` **and** `make check-linux`.

### M0 — Tooling
- Makefile with backend selection (`POLLER`), the compiler flags above, and sanitizers on by default.
- A test runner: one program per test, where exit code 0 means pass.
- A `check-linux` target, and `build/` added to `.gitignore`.
- **Done:** a trivial test passes on both backends.

### M1 — Coroutines without I/O
- `task_t`, `async.h` (`ASYNC_BEGIN`, `YIELD`, `ASYNC_FAIL`, `ASYNC_CLEANUP`, `ASYNC_END`) and a FIFO run queue.
- No OS calls yet.
- Remove `examples/async_dsl.c`.
- **Done:**
  - two tasks interleave in the expected order;
  - a test demonstrates rule 1 (locals lose their values, frame fields don't);
  - `ASYNC_FAIL` reaches cleanup with its code;
  - no warnings on gcc or clang.

### M2 — Futures, wake, join, cancellation
- `future_t`, `AWAIT`, `AWAIT_TRY`, the duplicate-wake guard, `task_detach`, `task_join` and `task_cancel`.
- **Done:**
  - a consumer awaits a future that a producer task completes;
  - a failed future propagates to cleanup;
  - a double wake polls only once;
  - cancellation before the first poll and while awaiting both reach cleanup with the reason;
  - join returns the child's error;
  - no leaks.

### M3 — Poller and timers
- `poller.h` with both backends, the timer heap, `sleep_ms`, task deadlines, the loop's timeout computation and `EDEADLK` detection.
- **Done:**
  - three sleeps finish in deadline order;
  - a cancelled sleep leaves the heap;
  - a deadline cancels a task with `ETIMEDOUT`;
  - a pipe test confirms one-shot semantics on both backends;
  - `examples/ticker.c` prints the expected output.

### M4 — Reactor and non-blocking I/O
- The fd table, the `io_*` operations, `io_close`, the cooperative budget, `SIGPIPE` handling and `EINTR` handling.
- **Done:**
  - 10,000 round-trips of pipe ping-pong between two tasks;
  - a reader cancelled while waiting leaves no armed interest;
  - writing to a closed peer gives `EPIPE`;
  - a regular file gives `EINVAL`;
  - one busy connection does not starve another.

### M5 — Echo server
- `examples/echo_server.c` and `tests/test_echo_load.c`.
- **Done:** every criterion under [End Goal](#-end-goal) is met.

---

## 🔮 Possible Extensions (after M5)

- Signal handling with a self-pipe, for example a graceful shutdown on `SIGINT`.
- Combinators over several futures, such as `race` and `all`. These would need futures with more than one waiter, or a waiter that waits on a set.
- Optimizations, measured before and after:
  - memory pools for tasks and frames;
  - a timer wheel instead of the heap;
  - edge-triggered polling.
- An io_uring backend. It is completion-based, so it does not fit the readiness contract of `poller.h` and would plug in at the reactor level. That contrast is a lesson in itself.

---

## 🧠 What You Will Learn

- How readiness-based I/O works, and how epoll and kqueue differ underneath one interface
- Why async/await is "just" a state machine, and where stackless coroutines stop being convenient
- The wake path from a kernel event to a resumed task
- How timers drive the event loop's sleep
- Why cancellation, ownership and lifetimes are the hard parts of async, not the scheduler

---

## 🧩 Philosophy

> Async is not magic — it is controlled execution + scheduling.
