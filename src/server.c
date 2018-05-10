#include "server.h"
#include "adlist.h"
#include "atomicvar.h"

#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <syslog.h>

/* Global vars */
struct server server; /* Server global state */

struct pusherCommand pusherCommandTable[] = {
    {"ping",pingCommand,0,0,0}
};

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            "ping");
        return;
    }

    if (c->argc == 1)
        addReplySds(c, sdsnew("PONG"));
    else
        addReplySds(c,c->argv[1]);
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys. */

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

/* Return the UNIX time in milliseconds */
mstime_t mstime(void) {
    return ustime()/1000;
}

/* Low level logging. To use only for very big messages, otherwise
 * serverLog() is to prefer. */
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    // int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < server.verbosity) return;

    // fp = log_to_stdout ? stdout : fopen(server.logfile,"a");
    fp = stdout;
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        int off;
        struct timeval tv;
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        off = strftime(buf,sizeof(buf),"%d %b %H:%M:%S.",localtime(&tv.tv_sec));
        snprintf(buf+off,sizeof(buf)-off,"%03d",(int)tv.tv_usec/1000);
        fprintf(fp,"%d %s %c %s\n",
            (int)getpid(), buf,c[level],msg);
    }
    fflush(fp);

    // if (!log_to_stdout) fclose(fp);
    // if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    if ((level&0xff) < server.verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level,msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Pusher. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
void serverLogFromHandler(int level, const char *msg) {
    int fd;
    char buf[64];

    fd = STDOUT_FILENO;
    if (fd == -1) return;
    // ll2string(buf,sizeof(buf),getpid());
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,":signal-handler (",17) == -1) goto err;
    // ll2string(buf,sizeof(buf),time(NULL));
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,") ",2) == -1) goto err;
    if (write(fd,msg,strlen(msg)) == -1) goto err;
    if (write(fd,"\n",1) == -1) goto err;
err:
    return;
}

struct pusherCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL) */
void updateCachedTime(void) {
    time_t unixtime = time(NULL);
    atomicSet(server.unixtime, unixtime);
    server.mstime = mstime();
}

/* Check for timeouts. Returns non-zero if the client was terminated.
 * The function gets the current time in milliseconds as argument since
 * it gets called multiple times in a loop, so calling gettimeofday() for
 * each iteration would be costly without any actual gain. */
int clientsCronHandleTimeout(client *c, mstime_t now_ms) {
    time_t now = now_ms / 1000;

    if (server.maxidletime + c->lastinteraction < now) {
        serverLog(LL_VERBOSE, "Closing idle client");
        freeClient(c);
        return 1;
    }
    return 0;
}

#define CLIENTS_CRON_MIN_ITERATIONS 5
void clientsCron(void) {
    /* Make sure to process at least numclients/server.hz of clients
     * per call. Since this function is called server.hz times per second
     * we are sure that in the worst case we process all the clients in 1
     * second. */
    int numclients = listLength(server.clients);
    int iterations = numclients/server.hz;
    mstime_t now = mstime();

    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ? 
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;
    
    while (listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        listRotate(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);

        if (clientsCronHandleTimeout(c, now)) continue;
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables.
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 * - Clients timeout of different kinds.
 * - Replication reconnection.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */
int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Update the time cache. */
    updateCachedTime();

    run_with_period(5000) {
        serverLog(LL_VERBOSE,
            "%lu clients connected, %zu bytes in use",
            listLength(server.clients),
            zmalloc_used_memory());
    }

    /* We need to do a few operations on clients asynchronously. */
    clientsCron();

    server.cronloops++;
    return 1000/server.hz;
}

/* This function gets called every time server is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors. */
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Handle writes with pending output buffers. */
    handleClientsWithPendingWrites();
}

void initServerConfig(void) {
    pthread_mutex_init(&server.next_client_id_mutex, NULL);

    server.hz = CONFIG_DEFAULT_HZ;
    server.port = CONFIG_DEFAULT_SERVER_PORT;
    server.tcp_backlog = CONFIG_DEFAULT_TCP_BACKLOG;
    server.bindaddr_count = 0;
    server.verbosity = CONFIG_DEFAULT_VERBOSITY;
    server.maxidletime = CONFIG_DEFAULT_CLIENT_TIMEOUT;
    server.tcpkeepalive = CONFIG_DEFAULT_TCP_KEEPALIVE;
    server.maxclients = CONFIG_DEFAULT_MAX_CLIENTS;
    server.maxmemory = CONFIG_DEFAULT_MAXMEMORY;
    server.commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
}

static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
    case SIGINT:
        msg = "Received SIGINT scheduling shutdown...";
        break;
    case SIGTERM:
        msg = "Received SIGTERM scheduling shutdown...";
        break;
    default:
        msg = "Received shutdown signal, scheduling shutdown...";
    };

    serverLogFromHandler(LL_WARNING, msg);
    exit(0);
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    // sigemptyset(&act.sa_mask);
    // act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    // act.sa_sigaction = sigsegvHandler;
    // sigaction(SIGSEGV, &act, NULL);
    // sigaction(SIGBUS, &act, NULL);
    // sigaction(SIGFPE, &act, NULL);
    // sigaction(SIGILL, &act, NULL);
    return;
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags, char *ip) {
    client *c;
    if ((c = createClient(fd)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    if (listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        freeClient(c);
        return;
    }
}

#define MAX_ACCEPTS_PER_CALL 1000
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(cfd,0,cip);
    }
}

int listenToPort(int port, int *fds, int *count) {
    int j;

    /* Force binding of 0.0.0.0 if no bind address is specified, always
     * entering the loop if j == 0. */
    if (server.bindaddr_count == 0) server.bindaddr[0] = NULL;
    for (j = 0; j < server.bindaddr_count || j == 0; j++) {
        if (server.bindaddr[j] == NULL) {
            int unsupported = 0;
            /* Bind * for both IPv6 and IPv4, we enter here only if
             * server.bindaddr_count == 0. */
            fds[*count] = anetTcp6Server(server.neterr,port,NULL,
                server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            } else if (errno == EAFNOSUPPORT) {
                unsupported++;
                serverLog(LL_WARNING,"Not listening to IPv6: unsupproted");
            }

            if (*count == 1 || unsupported) {
                /* Bind the IPv4 address as well. */
                fds[*count] = anetTcpServer(server.neterr,port,NULL,
                    server.tcp_backlog);
                if (fds[*count] != ANET_ERR) {
                    anetNonBlock(NULL,fds[*count]);
                    (*count)++;
                } else if (errno == EAFNOSUPPORT) {
                    unsupported++;
                    serverLog(LL_WARNING,"Not listening to IPv4: unsupproted");
                }
            }
            /* Exit the loop if we were able to bind * on IPv4 and IPv6,
             * otherwise fds[*count] will be ANET_ERR and we'll print an
             * error and return to the caller with an error. */
            if (*count + unsupported == 2) break;
        } else if (strchr(server.bindaddr[j],':')) {
            /* Bind IPv6 address. */
            fds[*count] = anetTcp6Server(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        } else {
            /* Bind IPv4 address. */
            fds[*count] = anetTcpServer(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        }
        if (fds[*count] == ANET_ERR) {
            serverLog(LL_WARNING,
                "Creating Server TCP listening socket %s:%d: %s",
                server.bindaddr[j] ? server.bindaddr[j] : "*",
                port, server.neterr);
            return C_ERR;
        }
        anetNonBlock(NULL,fds[*count]);
        (*count)++;
    }
    return C_OK;
}

void initServer(void) {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();

    server.pid = getpid();
    server.clients = listCreate();
    server.clients_pending_write = listCreate();
    server.system_memory_size = zmalloc_get_memory_size();
    server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
    if (server.el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }

    /* Open the TCP listening socket for the user commands. */
    if (server.port != 0 &&
        listenToPort(server.port,server.ipfd,&server.ipfd_count) == C_ERR)
        exit(1);

    /* Abort if there are no listening sockets at all. */
    if (server.ipfd_count == 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    server.cronloops = 0;

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. */
    for (j = 0; j < server.ipfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE,
            acceptTcpHandler, NULL) == AE_ERR)
            {
                serverPanic(
                    "Unrecoverable error creating server.ipfd file event.");
            }
    }
    
    server.initial_memory_usage = zmalloc_used_memory();
}

/* Populates the Pusher Command Table starting from the hard coded list
 * we have on top of server.c file. */
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(pusherCommandTable)/sizeof(struct pusherCommand);

    for (j = 0; j < numcommands; j++) {
        struct pusherCommand *c = pusherCommandTable+j;
        dictAdd(server.commands, sdsnew(c->name), c);
    }
}

int main(int argc, char **argv) {
    struct timeval tv;
    int i;

    initServerConfig();
    initServer();
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}
