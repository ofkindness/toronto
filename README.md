# toronto

A hands-on project to implement a minimal async runtime in pure C — inspired by Tokio.

---

## 🚀 What This Project Is

This project is a **learning-focused implementation** of an async runtime:

- event loop (epoll)
- task scheduler
- futures/promises
- await-like macros (stackless coroutines)

The goal is not to build a production-ready system, but to **understand how async runtimes actually work**.

---

## 🎯 Goals

- Learn how async systems work internally
- Build everything from scratch in C
- Keep it small and understandable
- Avoid unnecessary abstractions

---

## 🧱 What You Will Build

### 1. Event Loop
- Based on `epoll`
- Waits for I/O readiness

### 2. Scheduler
- Queue of runnable tasks
- Cooperative multitasking

### 3. Task System
- Resumable functions
- Manual state handling

### 4. Futures / Promises
- Represent async results
- Wake tasks when ready

### 5. Await Macros
- Simulate async/await
- Implemented via state machines

---

## 📂 Suggested Structure

```
/runtime
  loop.c / loop.h
  sched.c / sched.h
  task.c / task.h
  future.c / future.h
  await.h

/examples
  basic.c

main.c
```

---

## 🛠️ Getting Started

### Step 1 — Setup

```bash
mkdir async-c-runtime
cd async-c-runtime
git init
```

### Step 2 — Create basic structure

```bash
mkdir runtime examples
touch main.c
```

---

## 🧪 First Milestone

Implement a minimal event loop:

- initialize epoll
- wait for events
- print when something happens

---

## 🧪 Second Milestone

Implement a basic task system:

- task struct
- scheduler queue
- simple execution loop

---

## 🧪 Third Milestone

Add futures:

- store result
- allow task to wait
- wake task when ready

---

## 🧪 Fourth Milestone

Add await macros:

- save execution state
- resume from same point

---

## ✨ Example Goal

```c
void my_task(task_t *task) {
    ASYNC_BEGIN;

    printf("Start\n");

    AWAIT(some_future);

    printf("End\n");

    ASYNC_END;
}
```

---

## ⚠️ Constraints

- Single-threaded (for now)
- No external libraries
- Linux only (epoll)

---

## 🧠 What You Will Learn

- How event loops work
- How async/await is implemented internally
- How schedulers manage tasks
- Why async is just state machines

---

## 🔮 Possible Extensions

- timers
- TCP server
- multithreading
- cancellation
- memory pools

---

## 🧩 Philosophy

> Async is not magic — it is controlled execution + scheduling.

---

## 🏁 End Goal

Build something real:

- async TCP server
