#include <sys/time.h>
#include <sys/epoll.h>

#include "ae.h"

#define MAX_FD_SIZE 1024 * 1024

typedef struct aeApiState {
    int epfd;
    struct epoll_event *events;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state;

    state = zmalloc(sizeof(*state)*eventLoop->setsize);
    if (!state) return -1;
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->epfd = epoll_create1(0);
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    if ((state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize)) == NULL)
        return -1;
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ev;

    ev.events = 0;
    if (mask & AE_READABLE) ev.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    if (epoll_ctl(state->epfd, EPOLL_CTL_ADD, fd, &ev) == -1)
        return -1;
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ev;
    int mask = eventLoop->events[fd].mask & (~delmask);

    ev.events = 0;
    if (mask & AE_READABLE) ev.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    if (mask & AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ev);
    } else {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "epoll";
}
