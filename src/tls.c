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
#include "connhelpers.h"

#ifdef USE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

extern ConnectionType CT_Socket;

static SSL_CTX *tls_ctx;

void tlsInit(void) {
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    SSL_library_init();

    if (!RAND_poll()) {
        serverLog(LL_WARNING, "OpenSSL: Failed to seed random number generator.");
    }
}

int tlsConfigureServer(void) {
    return tlsConfigure(server.tls_cert_file, server.tls_key_file,
            server.tls_dh_params_file, server.tls_ca_cert_file);
}

/* Attempt to configure/reconfigure TLS. This operation is atomic and will
 * leave the SSL_CTX unchanged if fails.
 */
int tlsConfigure(const char *cert_file, const char *key_file,
        const char *dh_params_file, const char *ca_cert_file) {

    char errbuf[256];
    SSL_CTX *ctx = NULL;

    if (!cert_file) {
        serverLog(LL_WARNING, "No tls-cert-file configured!");
        goto error;
    }

    if (!key_file) {
        serverLog(LL_WARNING, "No tls-key-file configured!");
        goto error;
    }

    if (!ca_cert_file) {
        serverLog(LL_WARNING, "No tls-ca-cert-file configured!");
        goto error;
    }

    ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_ecdh_auto(ctx, 1);

    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load certificate: %s: %s", cert_file, errbuf);
        goto error;
    }
        
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load private key: %s: %s", key_file, errbuf);
        goto error;
    }
    
    if (SSL_CTX_load_verify_locations(ctx, ca_cert_file, NULL) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load CA certificate(s) file: %s: %s", ca_cert_file, errbuf);
        goto error;
    }

    if (dh_params_file) {
        FILE *dhfile = fopen(dh_params_file, "r");
        DH *dh = NULL;
        if (!dhfile) {
            serverLog(LL_WARNING, "Failed to load %s: %s", dh_params_file, strerror(errno));
            goto error;
        }

        dh = PEM_read_DHparams(dhfile, NULL, NULL, NULL);
        fclose(dhfile);
        if (!dh) {
            serverLog(LL_WARNING, "%s: failed to read DH params.", dh_params_file);
            goto error;
        }

        if (SSL_CTX_set_tmp_dh(ctx, dh) <= 0) {
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            serverLog(LL_WARNING, "Failed to load DH params file: %s: %s", dh_params_file, errbuf);
            DH_free(dh);
            goto error;
        }

        DH_free(dh);
    }

    SSL_CTX_free(tls_ctx);
    tls_ctx = ctx;

    return C_OK;

error:
    if (ctx) SSL_CTX_free(ctx);
    return C_ERR;
}

#ifdef TLS_DEBUGGING
#define TLSCONN_DEBUG(fmt, ...) \
    serverLog(LL_DEBUG, "TLSCONN: " fmt, __VA_ARGS__)
#else
#define TLSCONN_DEBUG(fmt, ...)
#endif

ConnectionType CT_TLS;

/* Normal socket connections have a simple events/handler correlation.
 *
 * With TLS connections we need to handle cases where during a logical read
 * or write operation, the SSL library asks to block for the opposite
 * socket operation.
 *
 * When this happens, we need to do two things:
 * 1. Make sure we register for the even.
 * 2. Make sure we know which handler needs to execute when the
 *    event fires.  That is, if we notify the caller of a write operation
 *    that it blocks, and SSL asks for a read, we need to trigger the
 *    write handler again on the next read event.
 *
 */

typedef enum {
    WANT_READ = 1,
    WANT_WRITE
} WantIOType;

#define TLS_CONN_FLAG_READ_WANT_WRITE   (1<<0)
#define TLS_CONN_FLAG_WRITE_WANT_READ   (1<<1)
#define TLS_CONN_FLAG_FD_SET            (1<<2)

typedef struct tls_connection {
    connection c;
    int flags;
    SSL *ssl;
    char *ssl_error;
} tls_connection;

connection *connCreateTLS(void) {
    tls_connection *conn = zcalloc(sizeof(tls_connection));
    conn->c.type = &CT_TLS;
    conn->c.fd = -1;
    conn->ssl = SSL_new(tls_ctx);
    return (connection *) conn;
}

connection *connCreateAcceptedTLS(int fd, int require_auth) {
    tls_connection *conn = (tls_connection *) connCreateTLS();
    conn->c.fd = fd;
    conn->c.state = CONN_STATE_ACCEPTING;

    if (!require_auth) {
        /* We still verify certificates if provided, but don't require them.
         */
        SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, NULL);
    }

    SSL_set_fd(conn->ssl, conn->c.fd);
    SSL_set_accept_state(conn->ssl);

    return (connection *) conn;
}

static void tlsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask);

/* Process the return code received from OpenSSL>
 * Update the want parameter with expected I/O.
 * Update the connection's error state if a real error has occured.
 * Returns an SSL error code, or 0 if no further handling is required.
 */
static int handleSSLReturnCode(tls_connection *conn, int ret_value, WantIOType *want) {
    if (ret_value <= 0) {
        int ssl_err = SSL_get_error(conn->ssl, ret_value);
        switch (ssl_err) {
            case SSL_ERROR_WANT_WRITE:
                *want = WANT_WRITE;
                return 0;
            case SSL_ERROR_WANT_READ:
                *want = WANT_READ;
                return 0;
            case SSL_ERROR_SYSCALL:
                conn->c.last_errno = errno;
                if (conn->ssl_error) zfree(conn->ssl_error);
                conn->ssl_error = errno ? zstrdup(strerror(errno)) : NULL;
                break;
            default:
                /* Error! */
                conn->c.last_errno = 0;
                if (conn->ssl_error) zfree(conn->ssl_error);
                conn->ssl_error = zmalloc(512);
                ERR_error_string_n(ERR_get_error(), conn->ssl_error, 512);
                break;
        }

        return ssl_err;
    }

    return 0;
}

void registerSSLEvent(tls_connection *conn, WantIOType want) {
    int mask = aeGetFileEvents(server.el, conn->c.fd);

    switch (want) {
        case WANT_READ:
            if (mask & AE_WRITABLE) aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
            if (!(mask & AE_READABLE)) aeCreateFileEvent(server.el, conn->c.fd, AE_READABLE,
                        tlsEventHandler, conn);
            break;
        case WANT_WRITE:
            if (mask & AE_READABLE) aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);
            if (!(mask & AE_WRITABLE)) aeCreateFileEvent(server.el, conn->c.fd, AE_WRITABLE,
                        tlsEventHandler, conn);
            break;
        default:
            serverAssert(0);
            break;
    }
}

void updateSSLEvent(tls_connection *conn) {
    int mask = aeGetFileEvents(server.el, conn->c.fd);
    int need_read = conn->c.read_handler || (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ);
    int need_write = conn->c.write_handler || (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE);

    if (need_read && !(mask & AE_READABLE))
        aeCreateFileEvent(server.el, conn->c.fd, AE_READABLE, tlsEventHandler, conn);
    if (!need_read && (mask & AE_READABLE))
        aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);

    if (need_write && !(mask & AE_WRITABLE))
        aeCreateFileEvent(server.el, conn->c.fd, AE_WRITABLE, tlsEventHandler, conn);
    if (!need_write && (mask & AE_WRITABLE))
        aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
}


static void tlsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    UNUSED(fd);
    tls_connection *conn = clientData;
    int ret;

    TLSCONN_DEBUG("tlsEventHandler(): fd=%d, state=%d, mask=%d, r=%d, w=%d, flags=%d",
            fd, conn->c.state, mask, conn->c.read_handler != NULL, conn->c.write_handler != NULL,
            conn->flags);

    ERR_clear_error();

    switch (conn->c.state) {
        case CONN_STATE_CONNECTING:
            if (connGetSocketError((connection *) conn)) {
                conn->c.last_errno = errno;
                conn->c.state = CONN_STATE_ERROR;
            } else {
                if (!(conn->flags & TLS_CONN_FLAG_FD_SET)) {
                    SSL_set_fd(conn->ssl, conn->c.fd);
                    conn->flags |= TLS_CONN_FLAG_FD_SET;
                }
                ret = SSL_connect(conn->ssl);
                if (ret <= 0) {
                    WantIOType want = 0;
                    if (!handleSSLReturnCode(conn, ret, &want)) {
                        registerSSLEvent(conn, want);

                        /* Avoid hitting UpdateSSLEvent, which knows nothing
                         * of what SSL_connect() wants and instead looks at our
                         * R/W handlers.
                         */
                        return;
                    }

                    /* If not handled, it's an error */
                    conn->c.state = CONN_STATE_ERROR;
                } else {
                    conn->c.state = CONN_STATE_CONNECTED;
                }
            }

            if (!callHandler((connection *) conn, conn->c.conn_handler)) return;
            conn->c.conn_handler = NULL;
            break;
        case CONN_STATE_ACCEPTING:
            ret = SSL_accept(conn->ssl);
            if (ret <= 0) {
                WantIOType want = 0;
                if (!handleSSLReturnCode(conn, ret, &want)) {
                    /* Avoid hitting UpdateSSLEvent, which knows nothing
                     * of what SSL_connect() wants and instead looks at our
                     * R/W handlers.
                     */
                    registerSSLEvent(conn, want);
                    return;
                }

                /* If not handled, it's an error */
                conn->c.state = CONN_STATE_ERROR;
            } else {
                conn->c.state = CONN_STATE_CONNECTED;
            }

            if (!callHandler((connection *) conn, conn->c.conn_handler)) return;
            conn->c.conn_handler = NULL;
            break;
        case CONN_STATE_CONNECTED:
            if ((mask & AE_READABLE) && (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ)) {
                conn->flags &= ~TLS_CONN_FLAG_WRITE_WANT_READ;
                if (!callHandler((connection *) conn, conn->c.write_handler)) return;
            }

            if ((mask & AE_WRITABLE) && (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE)) {
                conn->flags &= ~TLS_CONN_FLAG_READ_WANT_WRITE;
                if (!callHandler((connection *) conn, conn->c.read_handler)) return;
            }

            if ((mask & AE_READABLE) && conn->c.read_handler) {
                if (!callHandler((connection *) conn, conn->c.read_handler)) return;
            }

            if ((mask & AE_WRITABLE) && conn->c.write_handler) {
                if (!callHandler((connection *) conn, conn->c.write_handler)) return;
            }
            break;
        default:
            break;
    }

    updateSSLEvent(conn);
}

static void connTLSClose(connection *conn_) {
    tls_connection *conn = (tls_connection *) conn_;

    if (conn->ssl) {
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    if (conn->ssl_error) {
        zfree(conn->ssl_error);
        conn->ssl_error = NULL;
    }

    CT_Socket.close(conn_);
}

static int connTLSAccept(connection *_conn, ConnectionCallbackFunc accept_handler) {
    tls_connection *conn = (tls_connection *) _conn;
    int ret;

    if (conn->c.state != CONN_STATE_ACCEPTING) return C_ERR;
    ERR_clear_error();

    /* Try to accept */
    conn->c.conn_handler = accept_handler;
    ret = SSL_accept(conn->ssl);

    if (ret <= 0) {
        WantIOType want = 0;
        if (!handleSSLReturnCode(conn, ret, &want)) {
            registerSSLEvent(conn, want);   /* We'll fire back */
            return C_OK;
        } else {
            conn->c.state = CONN_STATE_ERROR;
            return C_ERR;
        }
    }

    conn->c.state = CONN_STATE_CONNECTED;
    if (!callHandler((connection *) conn, conn->c.conn_handler)) return C_OK;
    conn->c.conn_handler = NULL;

    return C_OK;
}

static int connTLSConnect(connection *conn_, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    tls_connection *conn = (tls_connection *) conn_;

    if (conn->c.state != CONN_STATE_NONE) return C_ERR;
    ERR_clear_error();

    /* Initiate Socket connection first */
    if (CT_Socket.connect(conn_, addr, port, src_addr, connect_handler) == C_ERR) return C_ERR;

    /* Return now, once the socket is connected we'll initiate
     * TLS connection from the event handler.
     */
    return C_OK;
}

static int connTLSWrite(connection *conn_, const void *data, size_t data_len) {
    tls_connection *conn = (tls_connection *) conn_;
    int ret, ssl_err;

    if (conn->c.state != CONN_STATE_CONNECTED) return -1;
    ERR_clear_error();
    ret = SSL_write(conn->ssl, data, data_len);

    if (ret <= 0) {
        WantIOType want = 0;
        if (!(ssl_err = handleSSLReturnCode(conn, ret, &want))) {
            if (want == WANT_READ) conn->flags |= TLS_CONN_FLAG_WRITE_WANT_READ;
            updateSSLEvent(conn);
            errno = EAGAIN;
            return -1;
        } else {
            if (ssl_err == SSL_ERROR_ZERO_RETURN ||
                    ((ssl_err == SSL_ERROR_SYSCALL && !errno))) {
                conn->c.state = CONN_STATE_CLOSED;
                return 0;
            } else {
                conn->c.state = CONN_STATE_ERROR;
                return -1;
            }
        }
    }

    return ret;
}

static int connTLSRead(connection *conn_, void *buf, size_t buf_len) {
    tls_connection *conn = (tls_connection *) conn_;
    int ret;
    int ssl_err;

    if (conn->c.state != CONN_STATE_CONNECTED) return -1;
    ERR_clear_error();
    ret = SSL_read(conn->ssl, buf, buf_len);
    if (ret <= 0) {
        WantIOType want = 0;
        if (!(ssl_err = handleSSLReturnCode(conn, ret, &want))) {
            if (want == WANT_WRITE) conn->flags |= TLS_CONN_FLAG_READ_WANT_WRITE;
            updateSSLEvent(conn);

            errno = EAGAIN;
            return -1;
        } else {
            if (ssl_err == SSL_ERROR_ZERO_RETURN ||
                    ((ssl_err == SSL_ERROR_SYSCALL) && !errno)) {
                conn->c.state = CONN_STATE_CLOSED;
                return 0;
            } else {
                conn->c.state = CONN_STATE_ERROR;
                return -1;
            }
        }
    }

    return ret;
}

static const char *connTLSGetLastError(connection *conn_) {
    tls_connection *conn = (tls_connection *) conn_;

    if (conn->ssl_error) return conn->ssl_error;
    return NULL;
}

int connTLSSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    conn->write_handler = func;
    updateSSLEvent((tls_connection *) conn);
    return C_OK;
}

int connTLSSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    conn->read_handler = func;
    updateSSLEvent((tls_connection *) conn);
    return C_OK;
}

static void setBlockingTimeout(tls_connection *conn, long long timeout) {
    anetBlock(NULL, conn->c.fd);
    anetSendTimeout(NULL, conn->c.fd, timeout);
    anetRecvTimeout(NULL, conn->c.fd, timeout);
}

static void unsetBlockingTimeout(tls_connection *conn) {
    anetNonBlock(NULL, conn->c.fd);
    anetSendTimeout(NULL, conn->c.fd, 0);
    anetRecvTimeout(NULL, conn->c.fd, 0);
}

static int connTLSBlockingConnect(connection *conn_, const char *addr, int port, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;
    int ret;

    if (conn->c.state != CONN_STATE_NONE) return C_ERR;

    /* Initiate socket blocking connect first */
    if (CT_Socket.blocking_connect(conn_, addr, port, timeout) == C_ERR) return C_ERR;

    /* Initiate TLS connection now.  We set up a send/recv timeout on the socket,
     * which means the specified timeout will not be enforced accurately. */
    SSL_set_fd(conn->ssl, conn->c.fd);
    setBlockingTimeout(conn, timeout);

    if ((ret = SSL_connect(conn->ssl)) <= 0) {
        conn->c.state = CONN_STATE_ERROR;
        return C_ERR;
    }
    unsetBlockingTimeout(conn);

    conn->c.state = CONN_STATE_CONNECTED;
    return C_OK;
}

static ssize_t connTLSSyncWrite(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;

    setBlockingTimeout(conn, timeout);
    SSL_clear_mode(conn->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    int ret = SSL_write(conn->ssl, ptr, size);
    SSL_set_mode(conn->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    unsetBlockingTimeout(conn);

    return ret;
}

static ssize_t connTLSSyncRead(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;

    setBlockingTimeout(conn, timeout);
    int ret = SSL_read(conn->ssl, ptr, size);
    unsetBlockingTimeout(conn);

    return ret;
}

static ssize_t connTLSSyncReadLine(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *) conn_;
    ssize_t nread = 0;

    setBlockingTimeout(conn, timeout);

    size--;
    while(size) {
        char c;

        if (SSL_read(conn->ssl,&c,1) <= 0) {
            nread = -1;
            goto exit;
        }
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            goto exit;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
        size--;
    }
exit:
    unsetBlockingTimeout(conn);
    return nread;
}

/* TODO: This is probably not the right thing to do, but as we handle proxying from child
 * processes we'll probably not need any shutdown mechanism anyway so this is just a
 * place holder for now.
 */
static int connTLSShutdown(connection *conn_, int how) {
    UNUSED(how);
    tls_connection *conn = (tls_connection *) conn_;

    return SSL_shutdown(conn->ssl);
}

ConnectionType CT_TLS = {
    .ae_handler = tlsEventHandler,
    .accept = connTLSAccept,
    .connect = connTLSConnect,
    .blocking_connect = connTLSBlockingConnect,
    .read = connTLSRead,
    .write = connTLSWrite,
    .close = connTLSClose,
    .set_write_handler = connTLSSetWriteHandler,
    .set_read_handler = connTLSSetReadHandler,
    .get_last_error = connTLSGetLastError,
    .sync_write = connTLSSyncWrite,
    .sync_read = connTLSSyncRead,
    .sync_readline = connTLSSyncReadLine,
    .shutdown = connTLSShutdown
};

#else   /* USE_OPENSSL */

void tlsInit(void) {
}

int tlsConfigure(const char *cert_file, const char *key_file,
        const char *dh_params_file, const char *ca_cert_file) {
    UNUSED(cert_file);
    UNUSED(key_file);
    UNUSED(dh_params_file);
    UNUSED(ca_cert_file);
    return C_OK;
}

int tlsConfigureServer(void) {
    return C_OK;
}

connection *connCreateTLS(void) { 
    return NULL;
}

connection *connCreateAcceptedTLS(int fd, int require_auth) {
    UNUSED(fd);

    return NULL;
}

#endif
