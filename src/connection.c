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

/* The connections module provides a lean abstraction of network connections
 * to avoid direct socket and async event management across the Redis code base.
 *
 * It does NOT provide advanced connection features commonly found in similar
 * libraries such as complete in/out buffer management, throttling, etc. These
 * functions remain in networking.c.
 *
 * The primary goal is to allow transparent handling of TCP and TLS based
 * connections. To do so, connections have the following properties:
 *
 * 1. A connection may live before its corresponding socket exists.  This
 *    allows various context and configuration setting to be handled before
 *    establishing the actual connection.
 * 2. The caller may register/unregister logical read/write handlers to be
 *    called when the connection has data to read from/can accept writes.
 *    These logical handlers may or may not correspond to actual AE events,
 *    depending on the implementation (for TCP they are; for TLS they aren't).
 */

/* ConnectionType should hold all low level building blocks that change across
 * different implementations.
 *
 * Callers shall use connXXX() functions and not be aware of this.
 *
 * Currently connXXX() functions are socket-only, as TLS support will be
 * integrated we'll transition this into ConnectionType properly.
 */

ConnectionType CT_Socket;

/* When a connection is created we must know its type already, but the
 * underlying socket may or may not exist:
 *
 * - For accepted connections, it exists as we do not model the listen/accept
 *   part; So caller calls connCreateSocket() followed by connAccept().
 * - For outgoing connections, the socket is created by the connection module
 *   itself; So caller calls connCreateSocket() followed by connConnect(),
 *   which registers a connect callback that fires on connected/error state
 *   (and after any transport level handshake was done).
 *
 * NOTE: An earlier version relied on connections being part of other structs
 * and not independently allocated. This could lead to further optimizations
 * like using container_of(), etc.  However it was discontinued in favor of
 * this approach for these reasons:
 *
 * 1. In some cases conns are created/handled outside the context of the
 * containing struct, in which case it gets a bit awkward to copy them.
 * 2. Future implementations may wish to allocate arbitrary data for the
 * connection.
 * 3. The container_of() approach is anyway risky because connections may
 * be embedded in different structs, not just client.
 */

static connection *connCreateGeneric(ConnectionType *type) {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = type;
    conn->fd = -1;

    return conn;
}

connection *connCreateSocket() {
    return connCreateGeneric(&CT_Socket);
}

connection *connCreateAcceptedSocket(int fd) {
    connection *conn = connCreateGeneric(&CT_Socket);
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    return conn;
}

/* The connection module does not deal with listening and accepting sockets,
 * so we assume we have a socket when an incoming connection is created.
 *
 * The fd supplied should therefore be associated with an already accept()ed
 * socket.
 *
 * connAccept() may directly call accept_handler(), or return and call it
 * at a later time. This behavior is a bit awkward but aims to reduce the need
 * to wait for the next event loop, if no additional handshake is required.
 */

int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    if (conn->state != CONN_STATE_ACCEPTING) return C_ERR;
    accept_handler(conn);
    return C_OK;
}

/* ------ Pure socket connections ------- */

static int connSocketWrite(connection *conn, const void *data, size_t data_len) {
    return write(conn->fd, data, data_len);
}

static int connSocketRead(connection *conn, void *buf, size_t buf_len) {
    return read(conn->fd, buf, buf_len);
}

ConnectionType CT_Socket = {
    .write = connSocketWrite,
    .read = connSocketRead
};

int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->type->write(conn, data, data_len);
}

int connRead(connection *conn, void *buf, size_t buf_len) {
    return conn->type->read(conn, buf, buf_len);
}

int connClose(connection *conn, int do_shutdown) {
    if (conn->fd == -1) return 0;

    if (do_shutdown) shutdown(conn->fd,SHUT_RDWR);
    aeDeleteFileEvent(server.el,conn->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);
    close(conn->fd);
    conn->fd = -1;

    zfree(conn);

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

int connHasReadHandler(connection *conn) {
    return conn->read_handler != NULL;
}

int connGetFd(connection *conn) {
    return conn->fd;
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

void connSetPrivateData(connection *conn, void *data) {
    conn->private_data = data;
}

void *connGetPrivateData(connection *conn) {
    return conn->private_data;
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

int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port) {
    return anetPeerToString(conn ? conn->fd : -1, ip, ip_len, port);
}

int connFormatPeer(connection *conn, char *buf, size_t buf_len) {
    return anetFormatPeer(conn ? conn->fd : -1, buf, buf_len);
}

int connBlock(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetBlock(NULL, conn->fd);
}

int connNonBlock(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetNonBlock(NULL, conn->fd);
}

int connEnableTcpNoDelay(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetEnableTcpNoDelay(NULL, conn->fd);
}

int connDisableTcpNoDelay(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetDisableTcpNoDelay(NULL, conn->fd);
}

int connKeepAlive(connection *conn, int interval) {
    if (conn->fd == -1) return C_ERR;
    return anetKeepAlive(NULL, conn->fd, interval);
}

int connSendTimeout(connection *conn, long long ms) {
    return anetSendTimeout(NULL, conn->fd, ms);
}

const char *connGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

int connGetState(connection *conn) {
    return conn->state;
}

