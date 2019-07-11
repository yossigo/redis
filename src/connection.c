/*
 * Copyright (c) 2019, Redis Labs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"

int connTcpWrite(connection *conn, const void *data, size_t data_len) {
    return write(conn->fd, data, data_len);
}

int connTcpRead(connection *conn, void *buf, size_t buf_len) {
    return read(conn->fd, buf, buf_len);
}

connection_type ConnectionTypeTCP = {
    .write = connTcpWrite,
    .read = connTcpRead
};

int connIsInitialized(connection *conn) {
    return conn->fd >= 0;
}

int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->t->write(conn, data, data_len);
}

int connRead(connection *conn, void *buf, size_t buf_len) {
    return conn->t->read(conn, buf, buf_len);
}

int connClose(connection *conn, int do_shutdown) {
    if (conn->fd == -1) return 0;

    if (do_shutdown) shutdown(conn->fd,SHUT_RDWR);
    aeDeleteFileEvent(server.el,conn->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);
    close(conn->fd);
    conn->fd = -1;

    return 0;
}

void connEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask)
{
    UNUSED(el);
    UNUSED(fd);
    connection *conn = clientData;

    if (conn->state == CONN_STATE_CONNECTING &&
            (mask & AE_WRITABLE) && conn->write_handler) {
        conn->state = CONN_STATE_CONNECTED;

        /* Call connect handler. We need to do it only once and remove it, but
         * do it before calling the handler not to overwrite a a new write handler
         * that may be set by the user.
         */
        ConnectionCallbackFunc handler = conn->write_handler;

        /* Remove write handler.  The callback may register a new one if needed.
         */
        conn->write_handler = NULL;
        aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);

        handler(conn);
    }

    /* Handle normal I/O flows */
    if ((mask & AE_READABLE) && conn->read_handler) {
        conn->read_handler(conn);
    }
    if ((mask & AE_WRITABLE) && conn->write_handler) {
        conn->write_handler(conn);
    }
}

int updateConnEvent(connection *conn)
{
    int mask = 0;
    if (conn->write_handler) mask |= AE_WRITABLE;
    if (conn->read_handler) mask |= AE_READABLE;

    return aeCreateFileEvent(server.el,conn->fd,mask,connEventHandler,conn);
}

int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    if (func == conn->write_handler) return C_OK;

    conn->write_handler = func;
    if (!conn->write_handler) 
        aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);
    else
        aeCreateFileEvent(server.el,conn->fd,AE_WRITABLE,connEventHandler,conn);
    return C_OK;
}

int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    if (func == conn->read_handler) return C_OK;

    conn->read_handler = func;
    if (!conn->read_handler)
        aeDeleteFileEvent(server.el,conn->fd,AE_READABLE);
    else
        aeCreateFileEvent(server.el,conn->fd,AE_READABLE,connEventHandler,conn);
    return C_OK;
}

int connHasWriteHandler(connection *conn) {
    return conn->write_handler != NULL;
}

void connInitialize(connection *conn, connection_type *type, int fd, void *privdata) {
    conn->privdata = privdata;
    conn->write_handler = NULL;
    conn->read_handler = NULL;
    conn->t = type;
    conn->fd = fd;
}

void connClone(connection *target, const connection *source) {
    memcpy(target, source, sizeof(connection));
}

int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler) {
    int fd = anetTcpNonBlockBestEffortBindConnect(NULL,addr,port,src_addr);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTING;
    connSetWriteHandler(conn, connect_handler);

    serverLog(LL_VERBOSE, "~~~~~ connConnect fd=%d\n", fd);

    return C_OK;
}

int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    int fd = anetTcpNonBlockConnect(NULL,addr,port);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = ETIMEDOUT;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTED;
    return C_OK;
}

int connGetSocketError(connection *conn) {
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);

    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    return sockerr;
}

void connSetPrivData(connection *conn, void *privdata) {
    conn->privdata = privdata;
}

ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

const char *connGetErrorString(connection *conn) {
    if (conn->state == CONN_STATE_ERROR) return strerror(conn->last_errno);
    return NULL;
}
