/*
 * The kqueue backend (macOS, FreeBSD).
 *
 * kqueue has one filter per direction, and EV_ONESHOT removes only the
 * filter that fired. After a read event, a write filter on the same fd is
 * therefore still armed. The contract allows that: the caller treats the fd
 * as disarmed, ignores an event it does not wait for, and the next
 * poller_arm deletes whatever is not in the new mask.
 *
 * This file does not define _POSIX_C_SOURCE: kqueue is not POSIX, and the
 * macro would hide it.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include "runtime/poller.h"

enum { MAX_EVENTS = 64 };

struct poller {
    int kq;
};

poller_t *poller_new(void)
{
    poller_t *p = malloc(sizeof *p);

    if (p == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    p->kq = kqueue();
    if (p->kq < 0) {
        int err = errno;

        free(p);
        errno = err;
        return NULL;
    }
    return p;
}

void poller_free(poller_t *p)
{
    if (p == NULL)
        return;
    close(p->kq);
    free(p);
}

/*
 * Applies one change per direction in a single kevent call. EV_RECEIPT makes
 * kevent report the result of each change instead of stopping at the first
 * failure. Deleting a filter that is not there (ENOENT) is not an error.
 */
static int apply(poller_t *p, int fd, unsigned add_mask)
{
    static const struct timespec no_wait = { 0, 0 };
    struct kevent changes[2], receipts[2];
    int n;

    EV_SET(&changes[0], (uintptr_t)fd, EVFILT_READ,
           (add_mask & POLLER_READ) ? EV_ADD | EV_ONESHOT | EV_RECEIPT
                                    : EV_DELETE | EV_RECEIPT,
           0, 0, NULL);
    EV_SET(&changes[1], (uintptr_t)fd, EVFILT_WRITE,
           (add_mask & POLLER_WRITE) ? EV_ADD | EV_ONESHOT | EV_RECEIPT
                                     : EV_DELETE | EV_RECEIPT,
           0, 0, NULL);

    n = kevent(p->kq, changes, 2, receipts, 2, &no_wait);
    if (n < 0)
        return errno;

    for (int i = 0; i < n; i++) {
        int err = (int)receipts[i].data;
        int deleting = !(add_mask & (receipts[i].filter == EVFILT_READ ? POLLER_READ
                                                                        : POLLER_WRITE));

        if (!(receipts[i].flags & EV_ERROR) || err == 0)
            continue;
        if (deleting && err == ENOENT)
            continue;
        return err;
    }
    return 0;
}

int poller_arm(poller_t *p, int fd, unsigned interest)
{
    return apply(p, fd, interest);
}

int poller_forget(poller_t *p, int fd)
{
    return apply(p, fd, 0);
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    struct kevent evs[MAX_EVENTS];
    struct timespec ts, *timeout = NULL;
    int n;

    if (max > MAX_EVENTS)
        max = MAX_EVENTS;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        timeout = &ts;
    }

    n = kevent(p->kq, NULL, 0, evs, max, timeout);
    if (n < 0)
        return errno == EINTR ? 0 : -1;

    /*
     * One event per filter. EV_EOF and EV_ERROR arrive on the filter they
     * concern, so the direction is always the filter's own.
     */
    for (int i = 0; i < n; i++) {
        out[i].fd = (int)evs[i].ident;
        out[i].ready = evs[i].filter == EVFILT_WRITE ? POLLER_WRITE : POLLER_READ;
    }
    return n;
}
