/*
 * The epoll backend (Linux).
 *
 * EPOLLONESHOT disables the whole fd after one event, whichever direction
 * fired. That is exactly the contract: after an event, all of the fd's
 * interest is disarmed.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "runtime/poller.h"

enum { MAX_EVENTS = 64 };

struct poller {
    int epfd;
};

poller_t *poller_new(void)
{
    poller_t *p = malloc(sizeof *p);

    if (p == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epfd < 0) {
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
    close(p->epfd);
    free(p);
}

int poller_arm(poller_t *p, int fd, unsigned interest)
{
    struct epoll_event ev = { 0 };

    ev.events = EPOLLONESHOT;
    if (interest & POLLER_READ)
        ev.events |= EPOLLIN;
    if (interest & POLLER_WRITE)
        ev.events |= EPOLLOUT;
    ev.data.fd = fd;

    /* The poller keeps no state, so it finds out here whether the fd is known. */
    if (epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ev) == 0)
        return 0;
    if (errno != ENOENT)
        return errno;
    if (interest == 0)
        return 0;               /* never armed: nothing to disarm */
    if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev) == 0)
        return 0;
    return errno;
}

int poller_forget(poller_t *p, int fd)
{
    struct epoll_event ev = { 0 };      /* ignored, but old kernels want a pointer */

    if (epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, &ev) == 0 || errno == ENOENT)
        return 0;
    return errno;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    struct epoll_event evs[MAX_EVENTS];
    int n;

    if (max > MAX_EVENTS)
        max = MAX_EVENTS;
    n = epoll_wait(p->epfd, evs, max, timeout_ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;

    for (int i = 0; i < n; i++) {
        unsigned ready = 0;

        if (evs[i].events & EPOLLIN)
            ready |= POLLER_READ;
        if (evs[i].events & EPOLLOUT)
            ready |= POLLER_WRITE;
        /*
         * An error or a hang-up is reported whether it was asked for or not.
         * Call it ready in both directions: the caller's next system call
         * finds out what happened, and a direction nobody waits on is
         * ignored.
         */
        if (evs[i].events & (EPOLLERR | EPOLLHUP))
            ready |= POLLER_READ | POLLER_WRITE;

        out[i].fd = evs[i].data.fd;
        out[i].ready = ready;
    }
    return n;
}
