#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include "ae.h"

#define MAX_FD_SIZE 1024 * 1024

typedef struct aeApiState {
    int kqfd;
    struct kevent *events;
} aeApiState;

static int *aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state;

    state = zmalloc(sizeof(*state)->setsize);
    if (!state) return -1;
    state->events = malloc(sizeof(struct kevent)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    if ((state->events = zrealloc(state->events, sizeof(struct kevent)*setsize)) == NULL)
        return -1;
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int nev;

    if (tvp != NULL) {
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        nev = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize, &timeout)
    } else {
        nev = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize, NULL)
    }

    if (nev > 0) {
        int i;

        for (i = 0; i < nev; i++) {
            int mask = 0;
            struct kevent *e = state->events[j];

            if (e->filter & EVFILT_READ) mask |= AE_READABLE;
            if (e->filter & EVFILT_WRITE) mask |= AE_WRITABLE;
            eventLoop->fired[i]->fd = e->ident;
            eventLoop->fired[i]->mask = mask;
        }
    }
    return nev;
}

static char *aeApiName(void) {
    return "kqueue";
}
