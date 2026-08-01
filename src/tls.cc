/*
 * A TLS client built directly on aws-lc, as a drop-in replacement for the parts
 * of ssl.SSLSocket that SABnzbd uses on the download path.
 *
 * Unlike unlocked_ssl_recv_into (which reaches into CPython's private _ssl structs
 * and dlsym's whatever libssl happens to be loaded), this owns its own SSL object,
 * so it can drain many TLS records per call, write without the GIL, and resume
 * sessions across reconnects without depending on any CPython internals.
 *
 * Copyright 2007-2026 The SABnzbd-Team (sabnzbd.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "tls.h"

#ifdef SABCTOOLS_AWS_LC

#include <pythread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/service_indicator.h>

#include <chrono>

#if defined(_WIN32) || defined(__CYGWIN__)
# define WIN32_LEAN_AND_MEAN
# include <Windows.h>
# include <winsock2.h>
typedef SOCKET SOCKET_T;
# define SABCTOOLS_POLL WSAPoll
# define SABCTOOLS_POLLFD WSAPOLLFD
#else
# include <poll.h>
# include <errno.h>
typedef int SOCKET_T;
# define SABCTOOLS_POLL poll
# define SABCTOOLS_POLLFD struct pollfd
#endif

/* Exception types borrowed from the standard library, so that the "except
   ssl.SSLWantReadError" clauses in the calling code keep matching. */
static PyObject *SSLWantReadError = NULL;
static PyObject *SSLWantWriteError = NULL;
static PyObject *SSLCertVerificationError = NULL;
static PyObject *SSLError = NULL;

/* A negative timeout means "block indefinitely", zero means "non-blocking".
   These mirror what socket.gettimeout() reports. */
#define TLS_TIMEOUT_BLOCKING (-1.0)

/* How much aws-lc may pull out of the socket in one go when read-ahead is on.
   65535 is the maximum it accepts, roughly four TLS records. */
#define TLS_READ_BUFFER_LEN 65535

typedef struct {
    PyObject_HEAD
    SSL_CTX *ctx;
    int check_hostname;
    int session_cache;
    /* Most recent session, for resumption on the next connection to this server.
       Guarded by session_lock because the callback fires without the GIL. */
    SSL_SESSION *session;
    PyThread_type_lock session_lock;
} TLSContextObject;

typedef struct {
    PyObject_HEAD
    /* Strong reference to the Python socket. Owning it rather than a bare fd keeps
       the fd lifetime in Python's hands, so close() semantics are unchanged. */
    PyObject *socket;
    PyObject *context;
    SSL *ssl;
    SOCKET_T fd;
    double timeout;
    char closed;
} TLSSocketObject;

/* Zero-initialised here and filled in by tls_init, so the slots can refer to
   functions that are defined further down this file */
static PyTypeObject TLSContextType = {PyVarObject_HEAD_INIT(NULL, 0)};
static PyTypeObject TLSSocketType = {PyVarObject_HEAD_INIT(NULL, 0)};

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/* Formats the top of the aws-lc error queue, falling back to a generic message.
   The queue is always drained so it cannot leak into a later operation. */
static void tls_format_error(char *buffer, size_t length, const char *fallback)
{
    unsigned long error = ERR_get_error();
    if (error) {
        ERR_error_string_n(error, buffer, length);
        ERR_clear_error();
    } else {
        snprintf(buffer, length, "%s", fallback);
    }
}

/* Raises the appropriate Python exception for a failed SSL_* call. Never called
   for SSL_ERROR_WANT_READ/WANT_WRITE, which the callers handle themselves. */
static void tls_set_error(TLSSocketObject *self, int error, int retval, const char *action)
{
    char message[256];

    if (error == SSL_ERROR_SSL) {
        /* Only the queued reason distinguishes a rejected certificate from any other
           protocol failure. SSL_get_verify_result alone does not: it also reports a
           failed verification that was not enforced because verify_mode is off. */
        unsigned long queued = ERR_peek_error();
        if (ERR_GET_LIB(queued) == ERR_LIB_SSL && ERR_GET_REASON(queued) == SSL_R_CERTIFICATE_VERIFY_FAILED) {
            long verify = SSL_get_verify_result(self->ssl);
            ERR_clear_error();
            PyErr_Format(
                SSLCertVerificationError,
                "certificate verify failed: %s (%ld)",
                X509_verify_cert_error_string(verify),
                verify
            );
            return;
        }
        tls_format_error(message, sizeof(message), "protocol error");
        PyErr_Format(SSLError, "%s failed: %s", action, message);
        return;
    }

    if (error == SSL_ERROR_SYSCALL) {
        ERR_clear_error();
        if (retval == 0) {
            /* The peer went away without a close_notify */
            PyErr_Format(PyExc_ConnectionResetError, "%s failed: connection closed by peer", action);
        } else {
#if defined(_WIN32) || defined(__CYGWIN__)
            PyErr_SetExcFromWindowsErr(PyExc_ConnectionAbortedError, WSAGetLastError());
#else
            PyErr_SetFromErrno(PyExc_ConnectionAbortedError);
#endif
        }
        return;
    }

    tls_format_error(message, sizeof(message), "unknown error");
    PyErr_Format(PyExc_ConnectionAbortedError, "%s failed: %s", action, message);
}

/* Waits for the socket to become readable or writable. Returns 1 when ready,
   0 on timeout and -1 with an exception set on error or interrupt.
   Must be called with the GIL held; it releases it while waiting. */
static int tls_wait(TLSSocketObject *self, int want_write, std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        int remaining = -1;
        if (self->timeout > 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                return 0;
            }
            remaining = (int)left;
        }

        if (PyErr_CheckSignals()) {
            return -1;
        }

        SABCTOOLS_POLLFD fds;
        fds.fd = self->fd;
        fds.events = (short)(want_write ? POLLOUT : POLLIN);
        fds.revents = 0;

        int result;
        Py_BEGIN_ALLOW_THREADS;
        result = SABCTOOLS_POLL(&fds, 1, remaining);
        Py_END_ALLOW_THREADS;

        if (result > 0) {
            return 1;
        }
        if (result == 0) {
            return 0;
        }
#if !defined(_WIN32) && !defined(__CYGWIN__)
        if (errno == EINTR) {
            /* Retry, PyErr_CheckSignals above gives the handler a chance to run */
            continue;
        }
#endif
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
}

/* Common tail for a blocked read/write: either report "would block" on a
   non-blocking socket, or wait for readiness. Returns 1 to retry the operation,
   0 when the caller should return NULL (exception set). */
static int tls_handle_would_block(
    TLSSocketObject *self,
    int error,
    std::chrono::steady_clock::time_point deadline
)
{
    if (self->timeout == 0) {
        PyErr_SetString(
            error == SSL_ERROR_WANT_WRITE ? SSLWantWriteError : SSLWantReadError,
            error == SSL_ERROR_WANT_WRITE ? "The operation did not complete (write)"
                                          : "The operation did not complete (read)"
        );
        return 0;
    }

    int ready = tls_wait(self, error == SSL_ERROR_WANT_WRITE, deadline);
    if (ready < 0) {
        return 0;
    }
    if (ready == 0) {
        PyErr_SetString(PyExc_TimeoutError, "The operation timed out");
        return 0;
    }
    return 1;
}

static std::chrono::steady_clock::time_point tls_deadline(double timeout)
{
    if (timeout <= 0) {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(timeout * 1e6));
}

/* Read-ahead makes aws-lc ask the transport for more than the record it is currently
   parsing, which it may only do when a short read is possible. Python leaves the fd
   non-blocking for every timeout except None, so that is exactly when we enable it. */
static void tls_apply_read_ahead(TLSSocketObject *self)
{
    if (self->ssl) {
        SSL_set_read_ahead(self->ssl, self->timeout >= 0 ? 1 : 0);
    }
}

/* Refuses to operate on a socket that has been closed */
static int tls_check_open(TLSSocketObject *self)
{
    if (self->closed || !self->ssl) {
        PyErr_SetString(PyExc_ValueError, "Operation on a closed TLSSocket");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* TLSContext                                                                 */
/* ------------------------------------------------------------------------- */

/* Called by aws-lc when a resumable session becomes available, possibly without
   the GIL, so it must not touch the Python API. Returning 1 transfers ownership
   of the session reference to us. */
static int tls_new_session_cb(SSL *ssl, SSL_SESSION *session)
{
    TLSContextObject *context = (TLSContextObject *)SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    if (!context || !context->session_lock) {
        return 0;
    }

    PyThread_acquire_lock(context->session_lock, WAIT_LOCK);
    SSL_SESSION *previous = context->session;
    context->session = session;
    PyThread_release_lock(context->session_lock);

    if (previous) {
        SSL_SESSION_free(previous);
    }
    return 1;
}

/* Adds every certificate in a concatenated PEM blob to the context's trust store.
   Returns the number added, or -1 when nothing could be parsed at all. */
static int tls_load_ca_certs(SSL_CTX *ctx, const char *pem, Py_ssize_t length)
{
    BIO *bio = BIO_new_mem_buf(pem, (int)length);
    if (!bio) {
        return -1;
    }

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    int added = 0;
    X509 *cert;
    while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        /* Duplicates are expected when several sources overlap, they are not fatal */
        if (X509_STORE_add_cert(store, cert)) {
            added++;
        }
        X509_free(cert);
    }

    /* The loop always terminates on a "no start line" error */
    ERR_clear_error();
    BIO_free(bio);
    return added;
}

static PyObject *TLSContext_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    TLSContextObject *self = (TLSContextObject *)type->tp_alloc(type, 0);
    if (self) {
        self->ctx = NULL;
        self->check_hostname = 1;
        self->session_cache = 0;
        self->session = NULL;
        self->session_lock = NULL;
    }
    return (PyObject *)self;
}

static int TLSContext_init(TLSContextObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {
        "ca_certs", "verify_mode", "check_hostname", "ciphers",
        "minimum_version", "maximum_version", "session_cache", NULL
    };

    const char *ca_certs = NULL;
    Py_ssize_t ca_certs_length = 0;
    int verify_mode = 1;
    int check_hostname = 1;
    const char *ciphers = NULL;
    int minimum_version = 0;
    int maximum_version = 0;
    int session_cache = 1;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|y#ipziip:TLSContext", (char **)kwlist,
            &ca_certs, &ca_certs_length, &verify_mode, &check_hostname,
            &ciphers, &minimum_version, &maximum_version, &session_cache)) {
        return -1;
    }

    ERR_clear_error();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        char message[256];
        tls_format_error(message, sizeof(message), "could not allocate context");
        PyErr_Format(SSLError, "TLSContext: %s", message);
        return -1;
    }

    /* Replace any context left over from a re-initialisation */
    if (self->ctx) {
        SSL_CTX_free(self->ctx);
    }
    self->ctx = ctx;
    self->check_hostname = check_hostname && verify_mode;

    /* Lets send() report a partial write instead of insisting on retrying the
       exact same buffer, which is what the caller's write buffer expects */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    /* Without read-ahead aws-lc asks the transport for exactly the record it is
       parsing, so a bulk transfer costs a syscall per record header plus one per
       body. These two flags together let it pull several records out of the socket
       at a time. The context flag is what sizes the read buffer, while the per
       connection flag set in tls_apply_read_ahead decides whether we actually try
       to fill it, which is only safe on a non-blocking transport.
       Measured on a loopback bulk transfer this costs ~5% more CPU at 4 concurrent
       connections and saves ~21% at 16 and above, so it is worth it for a downloader. */
    SSL_CTX_set_read_ahead(ctx, 1);
    SSL_CTX_set_default_read_buffer_len(ctx, TLS_READ_BUFFER_LEN);

    /* ssl.TLSVersion.MINIMUM_SUPPORTED/MAXIMUM_SUPPORTED are negative, and zero
       means "library default" to aws-lc, so both collapse to zero */
    if (!SSL_CTX_set_min_proto_version(ctx, (uint16_t)(minimum_version > 0 ? minimum_version : 0)) ||
        !SSL_CTX_set_max_proto_version(ctx, (uint16_t)(maximum_version > 0 ? maximum_version : 0))) {
        ERR_clear_error();
        PyErr_SetString(SSLError, "TLSContext: unsupported protocol version");
        return -1;
    }

    if (ciphers) {
        if (!SSL_CTX_set_cipher_list(ctx, ciphers)) {
            char message[256];
            tls_format_error(message, sizeof(message), "no cipher match");
            /* aws-lc understands a subset of the OpenSSL cipher string language,
               so the caller may need to fall back to the stdlib ssl module */
            PyErr_Format(SSLError, "TLSContext: could not set ciphers %s: %s", ciphers, message);
            return -1;
        }
    }

    SSL_CTX_set_verify(ctx, verify_mode ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

    if (ca_certs && ca_certs_length > 0) {
        int added = tls_load_ca_certs(ctx, ca_certs, ca_certs_length);
        if (added <= 0 && verify_mode) {
            PyErr_SetString(SSLError, "TLSContext: no usable certificates in ca_certs");
            return -1;
        }
    } else if (verify_mode) {
        PyErr_SetString(SSLError, "TLSContext: ca_certs is required when verify_mode is enabled");
        return -1;
    }

    if (session_cache) {
        if (!self->session_lock && !(self->session_lock = PyThread_allocate_lock())) {
            PyErr_NoMemory();
            return -1;
        }
        self->session_cache = 1;
        SSL_CTX_set_app_data(ctx, self);
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
        SSL_CTX_sess_set_new_cb(ctx, tls_new_session_cb);
    }

    return 0;
}

static void TLSContext_dealloc(TLSContextObject *self)
{
    if (self->ctx) {
        /* Drop the back pointer first, no callback may see a half-freed context */
        SSL_CTX_set_app_data(self->ctx, NULL);
        SSL_CTX_free(self->ctx);
        self->ctx = NULL;
    }
    if (self->session) {
        SSL_SESSION_free(self->session);
        self->session = NULL;
    }
    if (self->session_lock) {
        PyThread_free_lock(self->session_lock);
        self->session_lock = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TLSContext_wrap_socket(TLSContextObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"sock", "server_hostname", NULL};
    PyObject *sock = NULL;
    const char *server_hostname = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "Oz:wrap_socket", (char **)kwlist, &sock, &server_hostname)) {
        return NULL;
    }

    /* The socket must already be connected, we only layer TLS on top of it */
    PyObject *fileno = PyObject_CallMethod(sock, "fileno", NULL);
    if (!fileno) {
        return NULL;
    }
    long fd = PyLong_AsLong(fileno);
    Py_DECREF(fileno);
    if (fd == -1 && PyErr_Occurred()) {
        return NULL;
    }
    if (fd < 0) {
        PyErr_SetString(PyExc_ValueError, "wrap_socket: the socket is closed");
        return NULL;
    }

    PyObject *timeout_object = PyObject_CallMethod(sock, "gettimeout", NULL);
    if (!timeout_object) {
        return NULL;
    }
    double timeout = TLS_TIMEOUT_BLOCKING;
    if (timeout_object != Py_None) {
        timeout = PyFloat_AsDouble(timeout_object);
    }
    Py_DECREF(timeout_object);
    if (timeout == -1.0 && PyErr_Occurred()) {
        return NULL;
    }

    TLSSocketObject *wrapper = PyObject_New(TLSSocketObject, &TLSSocketType);
    if (!wrapper) {
        return NULL;
    }
    wrapper->socket = Py_NewRef(sock);
    wrapper->context = Py_NewRef((PyObject *)self);
    wrapper->fd = (SOCKET_T)fd;
    wrapper->timeout = timeout;
    wrapper->closed = 0;

    ERR_clear_error();
    wrapper->ssl = SSL_new(self->ctx);
    if (!wrapper->ssl) {
        char message[256];
        tls_format_error(message, sizeof(message), "could not allocate connection");
        PyErr_Format(SSLError, "wrap_socket: %s", message);
        Py_DECREF(wrapper);
        return NULL;
    }

    if (!SSL_set_fd(wrapper->ssl, (int)fd)) {
        PyErr_SetString(SSLError, "wrap_socket: could not attach the socket");
        Py_DECREF(wrapper);
        return NULL;
    }
    tls_apply_read_ahead(wrapper);

    if (server_hostname) {
        /* SNI, and when verifying also the name the certificate has to match.
           An IP literal is not a valid SNI value, which aws-lc reports by failing;
           the connection is still fine without it. */
        if (!SSL_set_tlsext_host_name(wrapper->ssl, server_hostname)) {
            ERR_clear_error();
        }
        if (self->check_hostname && !SSL_set1_host(wrapper->ssl, server_hostname)) {
            PyErr_Format(SSLError, "wrap_socket: invalid hostname %s", server_hostname);
            Py_DECREF(wrapper);
            return NULL;
        }
    } else if (self->check_hostname) {
        PyErr_SetString(PyExc_ValueError, "wrap_socket: server_hostname is required when check_hostname is set");
        Py_DECREF(wrapper);
        return NULL;
    }

    if (self->session_cache) {
        PyThread_acquire_lock(self->session_lock, WAIT_LOCK);
        if (self->session) {
            /* SSL_set_session takes its own reference */
            SSL_set_session(wrapper->ssl, self->session);
        }
        PyThread_release_lock(self->session_lock);
    }

    /* Drive the handshake to completion, releasing the GIL for the duration of
       every SSL_connect and every wait in between */
    auto deadline = tls_deadline(timeout);
    for (;;) {
        int result;
        Py_BEGIN_ALLOW_THREADS;
        result = SSL_connect(wrapper->ssl);
        Py_END_ALLOW_THREADS;

        if (result == 1) {
            break;
        }

        int error = SSL_get_error(wrapper->ssl, result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            if (!tls_handle_would_block(wrapper, error, deadline)) {
                Py_DECREF(wrapper);
                return NULL;
            }
            continue;
        }

        tls_set_error(wrapper, error, result, "handshake");
        Py_DECREF(wrapper);
        return NULL;
    }

    return (PyObject *)wrapper;
}

static PyObject *TLSContext_get_session_reused(TLSContextObject *self, void *closure)
{
    PyThread_type_lock lock = self->session_lock;
    int available;
    if (lock) {
        PyThread_acquire_lock(lock, WAIT_LOCK);
        available = self->session != NULL;
        PyThread_release_lock(lock);
    } else {
        available = 0;
    }
    return PyBool_FromLong(available);
}

static PyMethodDef TLSContext_methods[] = {
    {
        "wrap_socket",
        (PyCFunction)(void (*)(void))TLSContext_wrap_socket,
        METH_VARARGS | METH_KEYWORDS,
        "wrap_socket(sock, server_hostname) -> TLSSocket"
    },
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef TLSContext_getset[] = {
    {
        (char *)"has_session",
        (getter)TLSContext_get_session_reused,
        NULL,
        (char *)"whether a session is cached for resumption",
        NULL
    },
    {NULL, NULL, NULL, NULL, NULL}
};

/* ------------------------------------------------------------------------- */
/* TLSSocket                                                                  */
/* ------------------------------------------------------------------------- */

static void TLSSocket_dealloc(TLSSocketObject *self)
{
    /* Deliberately not freed by close(): a receive thread may still be inside
       SSL_read with the GIL released when another thread closes the socket.
       By the time we are deallocated no reference can be outstanding. */
    if (self->ssl) {
        SSL_free(self->ssl);
        self->ssl = NULL;
    }
    Py_CLEAR(self->socket);
    Py_CLEAR(self->context);
    PyObject_Free(self);
}

/* Reads into a plain buffer, draining as many TLS records as fit. Returns the byte
   count (zero on a clean shutdown) or -1 with an exception set. */
static Py_ssize_t tls_read(TLSSocketObject *self, char *mem, Py_ssize_t length)
{
    Py_ssize_t remaining = length;
    size_t count = 0;
    auto deadline = tls_deadline(self->timeout);

    for (;;) {
        int result = 0;
        int error = SSL_ERROR_NONE;

        ERR_clear_error();
        Py_BEGIN_ALLOW_THREADS;
        /* The whole point of this module: keep pulling records out of the
           connection until the buffer is full or the peer has nothing more,
           all without reacquiring the GIL in between */
        do {
            result = SSL_read(self->ssl, mem + count, (int)(remaining > INT_MAX ? INT_MAX : remaining));
            if (result <= 0) {
                break;
            }
            count += (size_t)result;
            remaining -= result;
        } while (remaining > 0);

        if (result <= 0) {
            error = SSL_get_error(self->ssl, result);
        }
        Py_END_ALLOW_THREADS;

        /* Whatever we already have beats reporting the reason we stopped */
        if (count > 0) {
            return (Py_ssize_t)count;
        }

        if (error == SSL_ERROR_ZERO_RETURN) {
            /* Clean shutdown, the caller reads this as "server closed connection" */
            ERR_clear_error();
            return 0;
        }

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            if (!tls_handle_would_block(self, error, deadline)) {
                return -1;
            }
            continue;
        }

        tls_set_error(self, error, result, "read");
        return -1;
    }
}

static PyObject *TLSSocket_recv_into(TLSSocketObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"buffer", "nbytes", NULL};
    Py_buffer buffer;
    Py_ssize_t nbytes = 0;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "w*|n:recv_into", (char **)kwlist, &buffer, &nbytes)) {
        return NULL;
    }

    Py_ssize_t length = -1;
    if (tls_check_open(self) < 0) {
        /* Fall through to the shared cleanup below */
    } else if (nbytes < 0) {
        PyErr_SetString(PyExc_ValueError, "negative nbytes");
    } else if (nbytes > buffer.len) {
        PyErr_SetString(PyExc_ValueError, "nbytes is greater than the length of the buffer");
    } else if ((nbytes ? nbytes : buffer.len) <= 0) {
        PyErr_SetString(PyExc_ValueError, "No space left in buffer");
    } else {
        length = tls_read(self, (char *)buffer.buf, nbytes ? nbytes : buffer.len);
    }

    PyBuffer_Release(&buffer);
    return length < 0 ? NULL : PyLong_FromSsize_t(length);
}

static PyObject *TLSSocket_recv(TLSSocketObject *self, PyObject *args)
{
    Py_ssize_t bufsize;

    if (!PyArg_ParseTuple(args, "n:recv", &bufsize)) {
        return NULL;
    }
    if (tls_check_open(self) < 0) {
        return NULL;
    }
    if (bufsize < 0) {
        PyErr_SetString(PyExc_ValueError, "negative buffersize in recv");
        return NULL;
    }

    PyObject *result = PyBytes_FromStringAndSize(NULL, bufsize);
    if (!result || bufsize == 0) {
        return result;
    }

    Py_ssize_t length = tls_read(self, PyBytes_AS_STRING(result), bufsize);
    if (length < 0) {
        Py_DECREF(result);
        return NULL;
    }
    if (length != bufsize && _PyBytes_Resize(&result, length) < 0) {
        return NULL;
    }
    return result;
}

/* Shared implementation for send() and sendall(). When all_of_it is set the loop
   only stops once every byte is written. */
static PyObject *TLSSocket_write_impl(TLSSocketObject *self, PyObject *args, int all_of_it, const char *action)
{
    Py_buffer data;

    if (!PyArg_ParseTuple(args, "y*", &data)) {
        return NULL;
    }

    if (tls_check_open(self) < 0) {
        PyBuffer_Release(&data);
        return NULL;
    }

    const char *mem = (const char *)data.buf;
    Py_ssize_t remaining = data.len;
    size_t count = 0;
    auto deadline = tls_deadline(self->timeout);

    if (remaining == 0) {
        PyBuffer_Release(&data);
        return all_of_it ? Py_NewRef(Py_None) : PyLong_FromLong(0);
    }

    for (;;) {
        int result = 0;
        int error = SSL_ERROR_NONE;

        ERR_clear_error();
        Py_BEGIN_ALLOW_THREADS;
        do {
            result = SSL_write(self->ssl, mem + count, (int)(remaining > INT_MAX ? INT_MAX : remaining));
            if (result <= 0) {
                break;
            }
            count += (size_t)result;
            remaining -= result;
        } while (remaining > 0);

        if (result <= 0) {
            error = SSL_get_error(self->ssl, result);
        }
        Py_END_ALLOW_THREADS;

        if (remaining == 0) {
            PyBuffer_Release(&data);
            return all_of_it ? Py_NewRef(Py_None) : PyLong_FromSize_t(count);
        }

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            /* send() reports a short write and lets the caller buffer the rest,
               sendall() has to keep going */
            if (!all_of_it && count > 0) {
                PyBuffer_Release(&data);
                return PyLong_FromSize_t(count);
            }
            if (!tls_handle_would_block(self, error, deadline)) {
                PyBuffer_Release(&data);
                return NULL;
            }
            continue;
        }

        if (error == SSL_ERROR_ZERO_RETURN) {
            ERR_clear_error();
            PyErr_SetString(PyExc_ConnectionResetError, "Server closed connection");
            PyBuffer_Release(&data);
            return NULL;
        }

        tls_set_error(self, error, result, action);
        PyBuffer_Release(&data);
        return NULL;
    }
}

static PyObject *TLSSocket_send(TLSSocketObject *self, PyObject *args)
{
    return TLSSocket_write_impl(self, args, 0, "write");
}

static PyObject *TLSSocket_sendall(TLSSocketObject *self, PyObject *args)
{
    return TLSSocket_write_impl(self, args, 1, "write");
}

static PyObject *TLSSocket_pending(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->ssl) {
        return PyLong_FromLong(0);
    }
    return PyLong_FromLong(SSL_pending(self->ssl));
}

static PyObject *TLSSocket_fileno(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyObject_CallMethod(self->socket, "fileno", NULL);
}

static PyObject *TLSSocket_setblocking(TLSSocketObject *self, PyObject *arg)
{
    int blocking = PyObject_IsTrue(arg);
    if (blocking < 0) {
        return NULL;
    }

    PyObject *result = PyObject_CallMethod(self->socket, "setblocking", "O", arg);
    if (!result) {
        return NULL;
    }
    Py_DECREF(result);

    self->timeout = blocking ? TLS_TIMEOUT_BLOCKING : 0.0;
    tls_apply_read_ahead(self);
    Py_RETURN_NONE;
}

static PyObject *TLSSocket_settimeout(TLSSocketObject *self, PyObject *arg)
{
    PyObject *result = PyObject_CallMethod(self->socket, "settimeout", "O", arg);
    if (!result) {
        return NULL;
    }
    Py_DECREF(result);

    if (arg == Py_None) {
        self->timeout = TLS_TIMEOUT_BLOCKING;
    } else {
        double timeout = PyFloat_AsDouble(arg);
        if (timeout == -1.0 && PyErr_Occurred()) {
            return NULL;
        }
        self->timeout = timeout;
    }
    tls_apply_read_ahead(self);
    Py_RETURN_NONE;
}

static PyObject *TLSSocket_gettimeout(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    if (self->timeout < 0) {
        Py_RETURN_NONE;
    }
    return PyFloat_FromDouble(self->timeout);
}

static PyObject *TLSSocket_getblocking(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyBool_FromLong(self->timeout != 0.0);
}

static PyObject *TLSSocket_session_reused(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->ssl) {
        return PyBool_FromLong(0);
    }
    return PyBool_FromLong(SSL_session_reused(self->ssl));
}

static PyObject *TLSSocket_version(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->ssl) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(SSL_get_version(self->ssl));
}

static PyObject *TLSSocket_cipher(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->ssl) {
        Py_RETURN_NONE;
    }

    const SSL_CIPHER *cipher = SSL_get_current_cipher(self->ssl);
    if (!cipher) {
        Py_RETURN_NONE;
    }

    /* Same shape as ssl.SSLSocket.cipher(): (name, protocol, secret bits) */
    return Py_BuildValue(
        "ssi",
        SSL_CIPHER_get_name(cipher),
        SSL_get_version(self->ssl),
        SSL_CIPHER_get_bits(cipher, NULL)
    );
}

static PyObject *TLSSocket_getpeercert(TLSSocketObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"binary_form", NULL};
    int binary_form = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p:getpeercert", (char **)kwlist, &binary_form)) {
        return NULL;
    }
    if (!binary_form) {
        /* The decoded dict form of ssl.SSLSocket.getpeercert() is not reimplemented */
        PyErr_SetString(PyExc_NotImplementedError, "only getpeercert(binary_form=True) is supported");
        return NULL;
    }
    if (tls_check_open(self) < 0) {
        return NULL;
    }

    X509 *cert = SSL_get_peer_certificate(self->ssl);
    if (!cert) {
        Py_RETURN_NONE;
    }

    unsigned char *der = NULL;
    int length = i2d_X509(cert, &der);
    X509_free(cert);
    if (length < 0) {
        ERR_clear_error();
        PyErr_SetString(SSLError, "could not encode the peer certificate");
        return NULL;
    }

    PyObject *result = PyBytes_FromStringAndSize((const char *)der, length);
    OPENSSL_free(der);
    return result;
}

static PyObject *TLSSocket_close(TLSSocketObject *self, PyObject *Py_UNUSED(ignored))
{
    self->closed = 1;
    /* The SSL object stays alive until deallocation, see TLSSocket_dealloc */
    return PyObject_CallMethod(self->socket, "close", NULL);
}

static PyMethodDef TLSSocket_methods[] = {
    {
        "recv_into",
        (PyCFunction)(void (*)(void))TLSSocket_recv_into,
        METH_VARARGS | METH_KEYWORDS,
        "recv_into(buffer, nbytes=0) -> int"
    },
    {"recv", (PyCFunction)TLSSocket_recv, METH_VARARGS, "recv(bufsize) -> bytes"},
    {"send", (PyCFunction)TLSSocket_send, METH_VARARGS, "send(data) -> int"},
    {"sendall", (PyCFunction)TLSSocket_sendall, METH_VARARGS, "sendall(data)"},
    {"pending", (PyCFunction)TLSSocket_pending, METH_NOARGS, "pending() -> int"},
    {"fileno", (PyCFunction)TLSSocket_fileno, METH_NOARGS, "fileno() -> int"},
    {"setblocking", (PyCFunction)TLSSocket_setblocking, METH_O, "setblocking(flag)"},
    {"settimeout", (PyCFunction)TLSSocket_settimeout, METH_O, "settimeout(value)"},
    {"gettimeout", (PyCFunction)TLSSocket_gettimeout, METH_NOARGS, "gettimeout() -> float | None"},
    {"getblocking", (PyCFunction)TLSSocket_getblocking, METH_NOARGS, "getblocking() -> bool"},
    {"session_reused", (PyCFunction)TLSSocket_session_reused, METH_NOARGS, "session_reused() -> bool"},
    {"version", (PyCFunction)TLSSocket_version, METH_NOARGS, "version() -> str"},
    {"cipher", (PyCFunction)TLSSocket_cipher, METH_NOARGS, "cipher() -> tuple"},
    {
        "getpeercert",
        (PyCFunction)(void (*)(void))TLSSocket_getpeercert,
        METH_VARARGS | METH_KEYWORDS,
        "getpeercert(binary_form=False) -> bytes"
    },
    {"close", (PyCFunction)TLSSocket_close, METH_NOARGS, "close()"},
    {NULL, NULL, 0, NULL}
};

static PyObject *TLSSocket_get_socket(TLSSocketObject *self, void *closure)
{
    return Py_NewRef(self->socket);
}

static PyObject *TLSSocket_get_context(TLSSocketObject *self, void *closure)
{
    return Py_NewRef(self->context);
}

static PyGetSetDef TLSSocket_getset[] = {
    {(char *)"socket", (getter)TLSSocket_get_socket, NULL, (char *)"the wrapped socket", NULL},
    {(char *)"context", (getter)TLSSocket_get_context, NULL, (char *)"the TLSContext used", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

/* ------------------------------------------------------------------------- */
/* Module setup                                                               */
/* ------------------------------------------------------------------------- */

/* Fetches an exception type from the ssl module. */
static PyObject *tls_get_exception(PyObject *ssl_module, const char *name)
{
    PyObject *exception = PyObject_GetAttrString(ssl_module, name);
    if (exception && !PyExceptionClass_Check(exception)) {
        Py_CLEAR(exception);
        PyErr_Format(PyExc_TypeError, "ssl.%s is not an exception type", name);
    }
    return exception;
}

bool tls_init(PyObject *module)
{
    PyObject *ssl_module = PyImport_ImportModule("ssl");
    if (!ssl_module) {
        return false;
    }

    SSLError = tls_get_exception(ssl_module, "SSLError");
    SSLWantReadError = tls_get_exception(ssl_module, "SSLWantReadError");
    SSLWantWriteError = tls_get_exception(ssl_module, "SSLWantWriteError");
    SSLCertVerificationError = tls_get_exception(ssl_module, "SSLCertVerificationError");
    Py_DECREF(ssl_module);

    if (!SSLError || !SSLWantReadError || !SSLWantWriteError || !SSLCertVerificationError) {
        return false;
    }

    TLSContextType.tp_name = "sabctools.TLSContext";
    TLSContextType.tp_basicsize = sizeof(TLSContextObject);
    TLSContextType.tp_dealloc = (destructor)TLSContext_dealloc;
    TLSContextType.tp_flags = Py_TPFLAGS_DEFAULT;
    TLSContextType.tp_doc = "TLSContext(ca_certs, verify_mode, check_hostname, ciphers, "
                            "minimum_version, maximum_version, session_cache)";
    TLSContextType.tp_methods = TLSContext_methods;
    TLSContextType.tp_getset = TLSContext_getset;
    TLSContextType.tp_init = (initproc)TLSContext_init;
    TLSContextType.tp_new = TLSContext_new;

    TLSSocketType.tp_name = "sabctools.TLSSocket";
    TLSSocketType.tp_basicsize = sizeof(TLSSocketObject);
    TLSSocketType.tp_dealloc = (destructor)TLSSocket_dealloc;
    TLSSocketType.tp_flags = Py_TPFLAGS_DEFAULT;
    TLSSocketType.tp_doc = "A TLS connection layered on a connected socket";
    TLSSocketType.tp_methods = TLSSocket_methods;
    TLSSocketType.tp_getset = TLSSocket_getset;

    if (PyType_Ready(&TLSContextType) < 0 || PyType_Ready(&TLSSocketType) < 0) {
        return false;
    }

    if (PyModule_AddObjectRef(module, "TLSContext", (PyObject *)&TLSContextType) < 0 ||
        PyModule_AddObjectRef(module, "TLSSocket", (PyObject *)&TLSSocketType) < 0 ||
        PyModule_AddStringConstant(module, "aws_lc_version", awslc_version_string()) < 0) {
        return false;
    }

    return true;
}

#endif /* SABCTOOLS_AWS_LC */
