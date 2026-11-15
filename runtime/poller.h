/*
 * The poller: the only place that knows about the OS readiness API.
 * See README.md, "Poller layer".
 *
 * There are two backends, poller_epoll.c and poller_kqueue.c. The Makefile
 * compiles exactly one of them.
 *
 * The contract is one-shot and level-triggered:
 *
 *   - poller_arm replaces the fd's interest. Passing 0 disarms the fd.
 *   - An armed interest delivers at most one event. After an event for an
 *     fd, the caller treats all of that fd's interest as disarmed, and arms
 *     again whatever it still needs.
 *   - Arming while the condition still holds fires again.
 *   - An error or a hang-up counts as readiness, so that the caller's next
 *     system call reports what happened.
 *   - Readiness is only a hint. An event may name a direction that the
 *     caller does not wait on, and the caller ignores it. One call of
 *     poller_wait may also report the same fd twice, once per direction.
 *   - The poller keeps no per-fd state. The armed mask lives in its caller.
 */
#ifndef TORONTO_POLLER_H
#define TORONTO_POLLER_H

enum { POLLER_READ = 1u << 0, POLLER_WRITE = 1u << 1 };

typedef struct poller poller_t;

typedef struct {
    int      fd;
    unsigned ready;     /* POLLER_READ | POLLER_WRITE */
} poller_event_t;

/* Returns NULL and sets errno on failure. */
poller_t *poller_new(void);
void      poller_free(poller_t *p);

/* Sets the fd's interest, one-shot. Returns 0 or an error code. */
int poller_arm(poller_t *p, int fd, unsigned interest);

/*
 * Removes the fd from the poller. Call it before close(). Returns 0 or an
 * error code; forgetting an fd that was never armed is not an error.
 */
int poller_forget(poller_t *p, int fd);

/*
 * Waits up to timeout_ms for events and stores at most max of them in out.
 * A timeout of -1 waits without limit, and 0 does not wait at all.
 *
 * Returns the number of events, or 0 on a timeout or an interrupting
 * signal. Returns -1 and sets errno on failure.
 */
int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms);

#endif
