#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "runtime/internal.h"
#include "runtime/timer.h"

/*
 * The heap is an array of pointers to entries. The entries themselves are
 * embedded in futures and tasks. Each entry knows its own position, so it
 * can be taken out of the middle of the heap in O(log n).
 */

/* The order of the heap: earlier deadline first, then earlier insertion. */
static bool before(const timer_entry_t *a, const timer_entry_t *b)
{
    if (a->deadline != b->deadline)
        return a->deadline < b->deadline;
    return a->seq < b->seq;
}

static void place(runtime_t *rt, size_t i, timer_entry_t *e)
{
    rt->heap[i] = e;
    e->pos = i + 1;
}

static void sift_up(runtime_t *rt, size_t i)
{
    timer_entry_t *e = rt->heap[i];

    while (i > 0) {
        size_t parent = (i - 1) / 2;

        if (!before(e, rt->heap[parent]))
            break;
        place(rt, i, rt->heap[parent]);
        i = parent;
    }
    place(rt, i, e);
}

static void sift_down(runtime_t *rt, size_t i)
{
    timer_entry_t *e = rt->heap[i];

    for (;;) {
        size_t child = 2 * i + 1;

        if (child >= rt->heap_len)
            break;
        if (child + 1 < rt->heap_len && before(rt->heap[child + 1], rt->heap[child]))
            child++;
        if (!before(rt->heap[child], e))
            break;
        place(rt, i, rt->heap[child]);
        i = child;
    }
    place(rt, i, e);
}

int timer_reserve(runtime_t *rt, size_t slots)
{
    timer_entry_t **heap;
    size_t cap = rt->heap_cap != 0 ? rt->heap_cap : 16;

    if (slots <= rt->heap_cap)
        return 0;
    while (cap < slots) {
        if (cap > SIZE_MAX / 2)
            return ENOMEM;
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof *heap)
        return ENOMEM;
    heap = realloc(rt->heap, cap * sizeof *heap);
    if (heap == NULL)
        return ENOMEM;
    rt->heap = heap;
    rt->heap_cap = cap;
    return 0;
}

void timer_arm(runtime_t *rt, timer_entry_t *e, unsigned kind, void *owner,
               uint64_t deadline)
{
    timer_disarm(rt, e);        /* re-arming replaces the old deadline */

    assert(rt->heap_len < rt->heap_cap && "timer slot was not reserved");
    e->deadline = deadline;
    e->seq = rt->timer_seq++;
    e->kind = kind;
    e->owner = owner;
    if (kind != TIMER_DEADLINE)
        rt->future_timers++;

    rt->heap[rt->heap_len] = e;
    e->pos = rt->heap_len + 1;
    rt->heap_len++;
    sift_up(rt, rt->heap_len - 1);
}

void timer_disarm(runtime_t *rt, timer_entry_t *e)
{
    timer_entry_t *last;
    size_t i;

    if (e->pos == 0)
        return;
    i = e->pos - 1;
    e->pos = 0;
    if (e->kind != TIMER_DEADLINE)
        rt->future_timers--;

    /* Move the last entry into the hole, then restore the order around it. */
    rt->heap_len--;
    last = rt->heap[rt->heap_len];
    if (last == e)
        return;
    place(rt, i, last);
    sift_up(rt, i);
    sift_down(rt, last->pos - 1);
}

bool timer_next(const runtime_t *rt, uint64_t *deadline)
{
    if (rt->heap_len == 0)
        return false;
    *deadline = rt->heap[0]->deadline;
    return true;
}

void timer_fire_due(runtime_t *rt, uint64_t now)
{
    while (rt->heap_len > 0 && rt->heap[0]->deadline <= now) {
        timer_entry_t *e = rt->heap[0];
        unsigned kind = e->kind;
        void *owner = e->owner;

        timer_disarm(rt, e);
        switch (kind) {
        case TIMER_SLEEP:
            future_complete(owner, 0);
            break;
        case TIMER_TIMEOUT:
            future_drop_and_fail(owner, ETIMEDOUT);
            break;
        case TIMER_DEADLINE:
            task_cancel(owner, ETIMEDOUT);
            break;
        }
    }
}

/* now + ms on the loop's clock, without wrapping around. */
static uint64_t deadline_after(const runtime_t *rt, uint64_t ms)
{
    uint64_t now = runtime_now_ms(rt);

    return ms > UINT64_MAX - now ? UINT64_MAX : now + ms;
}

void sleep_ms(task_t *t, future_t *f, uint64_t ms)
{
    runtime_t *rt = t->rt;

    /* Nothing holds a pointer to f except the heap, so no drop hook is needed. */
    if (!future_begin(t, f, NULL, NULL))
        return;
    if (timer_reserve(rt, rt->tasks + rt->future_timers + 1) != 0) {
        future_fail(f, ENOMEM);
        return;
    }
    timer_arm(rt, &f->timer, TIMER_SLEEP, f, deadline_after(rt, ms));
}

void future_set_timeout(future_t *f, uint64_t ms)
{
    runtime_t *rt;

    if (f->state != FUTURE_PENDING)
        return;
    rt = f->waiter->rt;
    assert(!(f->timer.pos != 0 && f->timer.kind == TIMER_SLEEP) &&
           "a sleep cannot have a timeout");

    if (f->timer.pos == 0 &&
        timer_reserve(rt, rt->tasks + rt->future_timers + 1) != 0) {
        future_drop_and_fail(f, ENOMEM);    /* the limit cannot be kept */
        return;
    }
    timer_arm(rt, &f->timer, TIMER_TIMEOUT, f, deadline_after(rt, ms));
}
