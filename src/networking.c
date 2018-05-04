#include "server.h"
#include "atomicvar.h"

void linkClient(client *c) {
    listAddNodeTail(server.clients, c);
    /* Note that we remember the linked list node where the client is stored,
     * this way removing the client in unlinkClient() will not require
     * a linear scan, but just a constant time operation. */
    c->client_list_node = listLast(server.clients);
}

#define READ_MESSAGE_LENGTH (16*1024)
void echoMessageFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    // client *c = (client*) privdata;
    int nread, nwrite;
    char readbuf[READ_MESSAGE_LENGTH];
    UNUSED(el);
    UNUSED(mask);

    nread = read(fd, readbuf, READ_MESSAGE_LENGTH);
    nwrite = write(fd, readbuf, nread);
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
            echoMessageFromClient, c) == AE_ERR) 
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
    c->reply = listCreate();
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

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

int _addReplyToBuffer(client *c, const char *s, size_t len) {
    size_t available = sizeof(c->buf)-c->bufpos;

    /* Check that the buffer has enough space available for this string. */
    if (len > available) return C_ERR;

    memcpy(c->buf+c->bufpos, s, len);
    c->bufpos += len;
    return C_OK;
}

int writeToClient(int fd, client *c, int handler_installed) {
    ssize_t nwritten = 0, totwritten = 0;

    while(c->bufpos) {
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
    if (!c->bufpos) {
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
