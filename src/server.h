#ifndef __SERVER_H__
#define __SERVER_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <signal.h>
#include <stdint.h>

#include "util.h"
#include "zmalloc.h"
#include "sds.h"
#include "adlist.h"
#include "dict.h"
#include "anet.h"
#include "ae.h"
#include "thread_pool.h"

#define PROTO_BUFFER_BYTES (16*1024)

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_RAW (1<<10) /* Modifier to log without timestamp */
#define CONFIG_DEFAULT_VERBOSITY LL_DEBUG

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Anti-warning macro... */
#define UNUSED(x) ((void) x)

/* Client flags */
#define CLIENT_PENDING_WRITE (1<<0) /* Client has output to send but a write
                                       handler is yet not installed. */

/* We can print the stacktrace, so our assert is defined this way: */
#define serverAssert(_e) ((_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define serverPanic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),_exit(1)


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
    int argc;               /* Num of arguments of current command. */
    sds **argv;            /* Arguments of current command. */
    list *reply;
    unsigned long long reply_bytes;
    size_t sentlen;
    time_t ctime;
    time_t lastinteraction;
    int flags;
    listNode *client_list_node;

    /* Response buffer */
    int bufpos;
    char buf[PROTO_BUFFER_BYTES];

    pthread_mutex_t lock;
} client;

/* Static server configuration */
#define CONFIG_DEFAULT_HZ        10      /* Time interrupt calls/sec. */
#define CONFIG_DEFAULT_SERVER_PORT       9528    /* TCP port */
#define CONFIG_DEFAULT_CLIENT_TIMEOUT    30      /* default client timeout: infinite */
#define CONFIG_DEFAULT_TCP_BACKLOG       511     /* TCP listen backlog */
#define CONFIG_DEFAULT_TCP_KEEPALIVE 300
#define CONFIG_DEFAULT_MAX_CLIENTS 10000
#define CONFIG_DEFAULT_MAXMEMORY 0
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN is 46, but we need to be sure */
#define LOG_MAX_LEN    1024 /* Default maximum length of syslog messages */
#define CONFIG_DEFAULT_THREADS 10 /* Default number of threads */
#define CONFIG_DEFAULT_MAX_TASKS 100 /* Default maximum size of thread tasks */

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. */
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

struct server {
    /* General */
    pid_t pid;                  /* Main process pid. */
    aeEventLoop *el;
    dict *commands;             /* Command table */
    size_t initial_memory_usage; /* Bytes used after initialization. */

    int ipfd[CONFIG_BINDADDR_MAX]; /* TCP socket file descriptors */
    int ipfd_count;             /* Used slots in ipfd[] */
    list *clients;              /* List of active clients */
    list *clients_pending_write; /* There is to write or install handler. */
    int hz;                     /* serverCron() calls frequency in hertz */
    int cronloops;              /* Number of times the cron function run */

    /* Networking */
    int port;
    int tcp_backlog;            /* TCP listen() backlog */
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* Addresses we should bind to */
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] */
    uint64_t next_client_id;    /* Next client unique ID. Incremental. */
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */

    /* time cache */
    time_t unixtime;
    long long mstime;   /* Like 'unixtime' but with milliseconds resolution. */

    /* Configuration */
    int verbosity;                  /* Loglevel */
    int maxidletime;
    int tcpkeepalive;

    /* Limits */
    unsigned int maxclients;            /* Max number of simultaneous clients */
    unsigned long long maxmemory;   /* Max number of memory bytes to use */
    thread_pool_t *tpool;  /* thread pool */

    /* Fields used only for stats */
    long long stat_rejected_conn;   /* Clients rejected because of maxclients */

    /* System hardware info */
    size_t system_memory_size;  /* Total memory in system as reported by OS */

    /* Mutexes used to protect atomic variables when atomic builtins are
     * not available. */
    pthread_mutex_t next_client_id_mutex;
    pthread_mutex_t lock;
};

typedef void pusherCommandProc(client *c);
struct pusherCommand {
    char *name;
    pusherCommandProc *proc;
    int arity;
    long long microseconds, calls;
};

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct server server;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Core functions */
struct pusherCommand *lookupCommand(sds name);
void populateCommandTable(void);

/* Utils */
long long ustime(void);
long long mstime(void);
void serverLog(int level, const char *fmt, ...);

/* networking.c -- Networking and Client related operations */
client *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void addReplySds(client *c, sds s);
void addReplyString(client *c, const char *s, size_t len);
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix);
void addReplyLongLong(client *c, long long ll);
void addReplyError(client *c, const char *err);
void addReplyErrorFormat(client *c, const char *fmt, ...);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);
void readMessageFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
int handleClientsWithPendingWrites(void);

/* Commands prototypes */
void pingCommand(client *c);

/* pubsub.c -- Pub/Sub related operations */
void publishCommand(client *c);

/* Debugging stuff */
void _serverAssert(const char *estr, const char *file, int line);
void _serverPanic(const char *file, int line, const char *msg, ...);

#endif /* __SERVER_H__ */
