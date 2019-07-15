
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

typedef struct connection connection;

typedef enum {
    CONN_STATE_CONNECTING = 1,
    CONN_STATE_ACCEPTING,
    CONN_STATE_CONNECTED,
    CONN_STATE_ERROR
} ConnectionState;

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

connection *connCreateSocket();
connection *connCreateAcceptedSocket(int fd);
int connAccept(connection *conn, ConnectionCallbackFunc accept_handler);
void connSetPrivateData(connection *conn, void *data);
void *connGetPrivateData(connection *conn);
int connGetState(connection *conn);
int connIsCreated(connection *conn);
int connBlock(connection *conn);
int connNonBlock(connection *conn);
int connEnableTcpNoDelay(connection *conn);
int connDisableTcpNoDelay(connection *conn);
int connKeepAlive(connection *conn, int interval);
int connSendTimeout(connection *conn, long long ms);

int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port);
int connFormatPeer(connection *conn, char *buf, size_t buf_len);
int connGetFd(connection *conn);

const char *connGetLastError(connection *conn);

/**/

int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler);
int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout);

int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func);
int connSetReadHandler(connection *conn, ConnectionCallbackFunc func);
int connHasWriteHandler(connection *conn);
int connHasReadHandler(connection *conn);
int connWrite(connection *conn, const void *data, size_t data_len);
int connRead(connection *conn, void *buf, size_t buf_len);
void connClose(connection *conn, int shutdown);
int connGetSocketError(connection *conn);
void connSetPrivData(connection *conn, void *privdata);

ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout);
ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout);
ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout);

