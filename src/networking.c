#include "server.h"
#include "atomicvar.h"

void linkClient(client *c) {
    listAddNodeTail(server.clients, c);
    /* Note that we remember the linked list node where the client is stored,
     * this way removing the client in unlinkClient() will not require
     * a linear scan, but just a constant time operation. */
    c->client_list_node = listLast(server.clients);
}

/* Client.reply list dup and free methods. */
void *dupClientReplyValue(void *o) {
    return sdsdup(o);
}

void freeClientReplyValue(void *o) {
    sdsfree(o);
}

client *createClient(int fd) {
    client *c;

    if ((c = zmalloc(sizeof(*c))) == NULL) return NULL;
    if (fd != -1) {
        anetNonBlock(NULL, fd);
        anetEnableTcpNoDelay(NULL, fd);
        if (server.tcpkeepalive)
            anetKeepAlive(NULL, fd, server.tcpkeepalive);
        if (aeCreateFileEvent(server.el, fd, AE_READABLE, 
            readMessageFromClient, c) == AE_ERR) 
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    uint64_t client_id;
    atomicGetIncr(server.next_client_id, client_id, 1);
    c->id = client_id;
    c->fd = fd;
    c->argc = 0;
    c->argv = NULL;
    c->reply = listCreate();
    c->reply_bytes = 0;
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->bufpos = 0;
    c->ctime = c->lastinteraction = server.unixtime;
    c->flags = 0;
    if (fd != -1) linkClient(c);
    return c;
}

void unlinkClient(client *c) {
    if (c->fd == -1) 
        return;

    /* Remove from the list of active clients. */
    if (c->client_list_node) {
        listDelNode(server.clients, c->client_list_node);
        c->client_list_node = NULL;
    }

    /* Unregister async I/O handlers and close the socket. */
    aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
    aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
    close(c->fd);
    c->fd = -1;
}

void freeClient(client *c) {
    unlinkClient(c);
    zfree(c);
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
int clientHasPendingReplies(client *c) {
    return c->bufpos || listLength(c->reply);
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers. */
int prepareClientToWrite(client *c) {
    if (c->fd <= 0) return C_ERR; /* The client is going to close. */

    /* Schedule the client to write the output buffers to the socket only
     * if not already done (there were no pending writes already and the client
     * was yet not flagged), and, for slaves, if the slave can actually
     * receive writes at this stage. */
    if (!clientHasPendingReplies(c) &&
        !(c->flags & CLIENT_PENDING_WRITE))
    {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        c->flags |= CLIENT_PENDING_WRITE;
        listAddNodeHead(server.clients_pending_write, c);
    }

    /* Authorize the caller to queue in the output buffer of this client. */
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

/* We use reply rather than buf normally. So we don't use this function
 * in most cases. */
int _addReplyToBuffer(client *c, const char *s, size_t len) {
    size_t available = sizeof(c->buf)-c->bufpos;

    /* Check that the buffer has enough space available for this string. */
    if (len > available) return C_ERR;

    memcpy(c->buf+c->bufpos, s, len);
    c->bufpos += len;
    return C_OK;
}

void _addReplyStringToList(client *c, const char *s, size_t len) {
    sds node = sdsnewlen(s,len);
    listAddNodeTail(c->reply,node);
    c->reply_bytes += len;
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

/* Add the SDS 's' string to the client output buffer, as a side effect
 * the SDS string is freed. */
void addReplySds(client *c, sds s) {
    if (prepareClientToWrite(c) != C_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    _addReplyStringToList(c,s,sdslen(s));
    sdsfree(s);
}

/* This low level function just adds whatever protocol you send it to the
 * client buffer, trying the static buffer initially, and using the string
 * of objects if not possible.
 *
 * It is efficient because does not create an SDS object nor an Redis object
 * if not needed. The object will only be created by calling
 * _addReplyStringToList() if we fail to extend the existing tail object
 * in the list of objects. */
void addReplyString(client *c, const char *s, size_t len) {
    if (prepareClientToWrite(c) != C_OK) return;
    _addReplyStringToList(c,s,len);
}

void addReplyLongLongWithPrefix(client *c, long long ll, char prefix) {
    char buf[128];
    int len;

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyString(c,buf,len+3);
}

void addReplyLongLong(client *c, long long ll) {
    addReplyLongLongWithPrefix(c,ll,':');
}

void addReplyError(client *c, const char *err) {
    addReplyString(c,err,strlen(err));
}

void addReplyErrorFormat(client *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyString(c,s,sdslen(s));
    sdsfree(s);
}

int writeToClient(int fd, client *c, int handler_installed) {
    ssize_t nwritten = 0, totwritten = 0;
    size_t objlen;
    sds o;

    while(clientHasPendingReplies(c)) {
        if (c->bufpos > 0) {
            nwritten = write(fd, c->buf, c->bufpos);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
            * the remainder of the reply. */
            if ((int)c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o);

            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                continue;
            }

            nwritten = write(fd, o + c->sentlen, objlen - c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objlen;
                /* If there are no longer objects in the list, we expect
                 * the count of reply bytes to be exactly zero. */
                if (listLength(c->reply) == 0)
                    if (c->reply_bytes != 0) exit(-1);
            }
        }
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        c->lastinteraction = server.unixtime;
    }
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        if (handler_installed) aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);        
    }
    return C_OK;
}

void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    writeToClient(fd, privdata, 1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
int handleClientsWithPendingWrites(void) {
    listNode *ln = listFirst(server.clients_pending_write);
    int processed = listLength(server.clients_pending_write);

    while(ln != NULL) {
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
        listDelNode(server.clients_pending_write,ln);

        /* Try to write buffers to the client socket. */
        if (writeToClient(c->fd,c,0) == C_ERR) continue;

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        if (clientHasPendingReplies(c)) {
            int ae_flags = AE_WRITABLE;
            if (aeCreateFileEvent(server.el, c->fd, ae_flags,
                sendReplyToClient, c) == AE_ERR)
            {
                /* Nothing to do, Just to avoid the warning... */                
            }
        }
        ln = listNodeNext(ln);
    }
    return processed;
}

#define READ_MESSAGE_LENGTH (16*1024)
void readMessageFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    client *c = (client*) privdata;
    int nread, nwrite;
    char readbuf[READ_MESSAGE_LENGTH];
    UNUSED(el);
    UNUSED(mask);

    nread = read(fd, readbuf, READ_MESSAGE_LENGTH);

    /* build argc and argv */
    c->argv = sdssplitlen(readbuf, nread, " ", 1, &c->argc);
    pingCommand(c);
    // addReplyString(c, readbuf, nread);
}
