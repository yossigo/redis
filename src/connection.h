
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

struct connection;

typedef struct connection_type {
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr);
    int (*write)(struct connection *conn, const void *data, size_t data_len); 
    int (*read)(struct connection *conn, void *buf, size_t buf_len); 
    int (*close)(struct connection *conn, int shutdown);
} connection_type;

extern connection_type ConnectionTypeTCP;

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

#define CONN_STATE_CONNECTING   1
#define CONN_STATE_ACCEPTING    2
#define CONN_STATE_CONNECTED    2
#define CONN_STATE_ERROR        3

typedef struct connection {
    connection_type *t;
    int state;
    int last_errno;
    void *privdata;
    ConnectionCallbackFunc write_handler;
    ConnectionCallbackFunc read_handler;
    int fd;
} connection;

int connIsInitialized(connection *conn);
void connInitialize(connection *conn, connection_type *type, int fd, void *privdata);
int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler);
int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout);

void connClone(connection *target, const connection *source);
int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func);
int connSetReadHandler(connection *conn, ConnectionCallbackFunc func);
int connHasWriteHandler(connection *conn);
int connWrite(connection *conn, const void *data, size_t data_len);
int connRead(connection *conn, void *buf, size_t buf_len);
int connClose(connection *conn, int shutdown);
int connGetSocketError(connection *conn);
void connSetPrivData(connection *conn, void *privdata);

ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout);
ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout);
ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout);

