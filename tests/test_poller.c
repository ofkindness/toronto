/*
 * M3: the poller contract, checked against the backend this build uses.
 * The test talks to the poller directly, without a runtime.
 *
 *   - an armed interest delivers at most one event (one-shot);
 *   - arming again while the condition holds fires again (level-triggered);
 *   - arming with 0 disarms, and forgetting removes the fd;
 *   - a hang-up counts as readiness;
 *   - after an event, arming with a new mask drops the direction left over.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "runtime/poller.h"
#include "tests/test.h"

/*
 * Waits once and returns the directions reported for fd. An fd may be
 * reported twice in one call, once per direction, so the bits are collected.
 * No other fd may be reported.
 */
static unsigned ready_on(poller_t *p, int fd, int timeout_ms)
{
    poller_event_t events[8];
    unsigned ready = 0;
    int n = poller_wait(p, events, 8, timeout_ms);

    CHECK(n >= 0);
    for (int i = 0; i < n; i++) {
        CHECK(events[i].fd == fd);
        ready |= events[i].ready;
    }
    return ready;
}

static long now_ms(void)
{
    struct timespec ts;

    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void put_byte(int fd)
{
    CHECK(write(fd, "x", 1) == 1);
}

static void take_byte(int fd)
{
    char c;

    CHECK(read(fd, &c, 1) == 1);
}

static void one_shot_and_level_triggered(poller_t *p)
{
    int fds[2];
    char c;

    CHECK(pipe(fds) == 0);

    /* Nothing armed, then armed with nothing to read: no event. */
    CHECK(ready_on(p, fds[0], 0) == 0);
    CHECK(poller_arm(p, fds[0], POLLER_READ) == 0);
    CHECK(ready_on(p, fds[0], 0) == 0);

    /* Data arrives: exactly one event. */
    put_byte(fds[1]);
    CHECK(ready_on(p, fds[0], 100) == POLLER_READ);

    /* One-shot: the byte is still unread, but the interest is used up. */
    CHECK(ready_on(p, fds[0], 0) == 0);

    /* Level-triggered: arming again while the byte is there fires again. */
    CHECK(poller_arm(p, fds[0], POLLER_READ) == 0);
    CHECK(ready_on(p, fds[0], 0) == POLLER_READ);

    /* With the byte gone, an armed interest waits. */
    take_byte(fds[0]);
    CHECK(poller_arm(p, fds[0], POLLER_READ) == 0);
    CHECK(ready_on(p, fds[0], 0) == 0);

    /* Arming with 0 disarms it, so new data is not reported. */
    CHECK(poller_arm(p, fds[0], 0) == 0);
    put_byte(fds[1]);
    CHECK(ready_on(p, fds[0], 0) == 0);
    take_byte(fds[0]);

    /* An empty pipe is writable at once. */
    CHECK(poller_arm(p, fds[1], POLLER_WRITE) == 0);
    CHECK(ready_on(p, fds[1], 100) == POLLER_WRITE);

    /* A hang-up counts as readiness: the read that follows sees end of file. */
    CHECK(poller_arm(p, fds[0], POLLER_READ) == 0);
    CHECK(poller_forget(p, fds[1]) == 0);
    close(fds[1]);
    CHECK(ready_on(p, fds[0], 100) & POLLER_READ);
    CHECK(read(fds[0], &c, 1) == 0);

    CHECK(poller_forget(p, fds[0]) == 0);
    close(fds[0]);
}

static void forget_removes_the_fd(poller_t *p)
{
    int fds[2];

    CHECK(pipe(fds) == 0);

    CHECK(poller_forget(p, fds[0]) == 0);       /* never armed: not an error */

    CHECK(poller_arm(p, fds[0], POLLER_READ) == 0);
    CHECK(poller_forget(p, fds[0]) == 0);
    put_byte(fds[1]);
    CHECK(ready_on(p, fds[0], 0) == 0);

    CHECK(poller_forget(p, fds[0]) == 0);       /* twice: not an error */

    close(fds[0]);
    close(fds[1]);

    /* A closed fd is an error, not something to arm. */
    CHECK(poller_arm(p, fds[0], POLLER_READ) == EBADF);
}

/*
 * Both directions on one fd. A socket with nothing to read reports only
 * WRITE. The caller then treats the whole fd as disarmed. kqueue still holds
 * the read filter at that point, and the next poller_arm must remove it.
 */
static void both_directions(poller_t *p)
{
    int sv[2];

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    CHECK(poller_arm(p, sv[0], POLLER_READ | POLLER_WRITE) == 0);
    CHECK(ready_on(p, sv[0], 100) == POLLER_WRITE);

    CHECK(poller_arm(p, sv[0], 0) == 0);
    put_byte(sv[1]);                            /* sv[0] is readable now */
    CHECK(ready_on(p, sv[0], 0) == 0);

    /* Armed for both while both hold: both are reported. */
    CHECK(poller_arm(p, sv[0], POLLER_READ | POLLER_WRITE) == 0);
    CHECK(ready_on(p, sv[0], 100) == (POLLER_READ | POLLER_WRITE));

    /* Used up again. */
    CHECK(ready_on(p, sv[0], 0) == 0);

    CHECK(poller_forget(p, sv[0]) == 0);
    close(sv[0]);
    close(sv[1]);
}

/* With nothing to report, the wait lasts as long as the timeout. */
static void wait_times_out(poller_t *p)
{
    poller_event_t events[1];
    long start = now_ms();

    CHECK(poller_wait(p, events, 1, 30) == 0);
    CHECK(now_ms() - start >= 25);
}

int main(void)
{
    poller_t *p = poller_new();

    CHECK(p != NULL);
    one_shot_and_level_triggered(p);
    forget_removes_the_fd(p);
    both_directions(p);
    wait_times_out(p);
    poller_free(p);
    return 0;
}
