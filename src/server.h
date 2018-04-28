#ifndef __SERVER_H__
#define __SERVER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <signal.h>

#include "zmalloc.h"
#include "anet.h"
#include "ae.h"

#define PROTO_BUFFER_BYTES (16*1024)

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define CONFIG_DEFAULT_VERBOSITY LL_NOTICE

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Anti-warning macro... */
#define UNUSED(x) ((void) x)

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

typedef long long mstime_t; /* millisecond time type. */

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list. */
typedef struct client {
    uint64_t id;
    int fd;
    list *reply;
    size_t sentlen;
    time_t ctime;
    time_t lastinteraction;
    int flags;

    /* Response buffer */
    int bufpos;
    char buf[PROTO_BUFFER_BYTES];
} client;

/* Static server configuration */
#define CONFIG_DEFAULT_HZ        10      /* Time interrupt calls/sec. */
#define CONFIG_DEFAULT_SERVER_PORT        6379    /* TCP port */
#define CONFIG_DEFAULT_CLIENT_TIMEOUT       0       /* default client timeout: infinite */
#define CONFIG_DEFAULT_TCP_KEEPALIVE 300
#define CONFIG_DEFAULT_MAX_CLIENTS 10000
#define CONFIG_DEFAULT_MAXMEMORY 0
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. */
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

struct server {
    /* General */
    pid_t pid;                  /* Main process pid. */

    int ipfd[CONFIG_BINDADDR_MAX]; /* TCP socket file descriptors */
    int ipfd_count;             /* Used slots in ipfd[] */
    int hz;                     /* serverCron() calls frequency in hertz */
    int cronloops;              /* Number of times the cron function run */

    /* Networking */
    int port;

    /* Configuration */
    int verbosity;                  /* Loglevel */
    int maxidletime;
    int tcpkeepalive;

    /* Limits */
    unsigned int maxclients;            /* Max number of simultaneous clients */
    unsigned long long maxmemory;   /* Max number of memory bytes to use */

    /* Mutexes used to protect atomic variables when atomic builtins are
     * not available. */
    pthread_mutex_t next_client_id_mutex;
}

/* Utils */
long long ustime(void);
long long mstime(void);

/* networking.c -- Networking and Client related operations */
client *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);

#endif /* __SERVER_H__ */
