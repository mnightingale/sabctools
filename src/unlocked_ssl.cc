/*
 * This code was largely copied from cpython's ssl.c:
 * Copyright 2001-2022 Python Software Foundation
 * Licensed under the PSF LICENSE AGREEMENT FOR PYTHON 3.11.1,
 * see https://docs.python.org/3/license.html
 *
 * With modifications:
 * Copyright 2023 The SABnzbd-Team (sabnzbd.org)
 * Licensed under the GNU GPL version 2 or later.
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

#include "unlocked_ssl.h"
#include <openssl/err.h>

static long openssl_version_number = 0;
static int (*SSL_read_ex)(void*, void*, size_t, size_t*) = NULL;
static int (*SSL_get_error)(void*, int) = NULL;
static int (*SSL_get_shutdown)(void*) = NULL;
static int (*UnlockedSSL_ERR_peek_last_error)() = NULL;
static int (*UnlockedSSL_ERR_clear_error)() = NULL;
static char* (*UnlockedSSL_ERR_reason_error_string)(unsigned long e) = NULL;
static PyObject *SSLSocketType = NULL;
static PyObject *PySSLErrorObject = NULL;
static PyObject *PySSLCertVerificationErrorObject = NULL;
static PyObject *PySSLZeroReturnErrorObject = NULL;
static PyObject *PySSLWantReadErrorObject = NULL;
static PyObject *PySSLWantWriteErrorObject = NULL;
static PyObject *PySSLSyscallErrorObject = NULL;
static PyObject *PySSLEOFErrorObject = NULL;

typedef struct {
    int ssl; /* last seen error from SSL */
    int c; /* last seen error from libc */
#ifdef MS_WINDOWS
    int ws; /* last seen error from winsock */
#endif
} _PySSLError;

typedef struct {
    PyObject_HEAD
    PyObject *Socket; /* weakref to socket on which we're layered */
    void *ssl;
    PyObject *ctx; /* weakref to SSL context */
    char shutdown_seen_zero;
    int socket_type;
    PyObject *owner; /* Python level "owner" passed to servername callback */
    PyObject *server_hostname;
    _PySSLError err; /* last seen error from various sources */
    /* Some SSL callbacks don't have error reporting. Callback wrappers
     * store exception information on the socket. The handshake, read, write,
     * and shutdown methods check for chained exceptions.
     */
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 12
    PyObject *exc;
#else
    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_tb;
#endif
} PySSLSocket;

static inline _PySSLError _PySSL_errno(int failed, void *ssl, int retcode)
{
    _PySSLError err = { 0 };
    if (failed) {
#ifdef MS_WINDOWS
        err.ws = WSAGetLastError();
#endif
        err.c = errno;
        err.ssl = SSL_get_error(ssl, retcode);
    }
    return err;
}

enum py_ssl_error {
    /* these mirror ssl.h */
    PY_SSL_ERROR_NONE,
    PY_SSL_ERROR_SSL,
    PY_SSL_ERROR_WANT_READ,
    PY_SSL_ERROR_WANT_WRITE,
    PY_SSL_ERROR_WANT_X509_LOOKUP,
    PY_SSL_ERROR_SYSCALL,     /* look at error stack/return value/errno */
    PY_SSL_ERROR_ZERO_RETURN,
    PY_SSL_ERROR_WANT_CONNECT,
    /* start of non ssl.h errorcodes */
    PY_SSL_ERROR_EOF,         /* special case of SSL_ERROR_SYSCALL */
    PY_SSL_ERROR_NO_SOCKET,   /* socket has been GC'd */
    PY_SSL_ERROR_INVALID_ERROR_CODE
};

typedef enum {
    SOCKET_IS_NONBLOCKING,
    SOCKET_IS_BLOCKING,
    SOCKET_HAS_TIMED_OUT,
    SOCKET_HAS_BEEN_CLOSED,
    SOCKET_TOO_LARGE_FOR_SELECT,
    SOCKET_OPERATION_OK
} timeout_state;

/* Get the socket from a PySSLSocket, if it has one */
#define GET_SOCKET(obj) ((obj)->Socket ? \
    (PyObject *) PyWeakref_GetObject((obj)->Socket) : NULL)

/* Linking to OpenSSL function used by Python */
void openssl_init() {
    // TODO: consider adding an extra version check to avoid possible future changes to SSL_read_ex

    PyObject *ssl_module = NULL;
    PyObject *py_openssl_version_number = NULL;
    PyObject *_ssl_module = NULL;
    PyObject *_ssl_module_path = NULL;
    #if defined(_WIN32) || defined(__CYGWIN__)
    HMODULE openssl_handle = NULL;
    HMODULE crypto_handle = NULL;
    #else
    void* openssl_handle = NULL;
    #endif

    ssl_module = PyImport_ImportModule("ssl");
    if(!ssl_module) goto cleanup;

    py_openssl_version_number = PyObject_GetAttrString(ssl_module, "OPENSSL_VERSION_NUMBER");
    if(!py_openssl_version_number) goto cleanup;
    openssl_version_number = PyLong_AsLong(py_openssl_version_number);

    _ssl_module = PyImport_ImportModule("_ssl");
    if(!_ssl_module) goto cleanup;

    SSLSocketType = PyObject_GetAttrString(ssl_module, "SSLSocket");
    if(!SSLSocketType) goto cleanup;

    PySSLErrorObject = PyObject_GetAttrString(ssl_module, "SSLError");
    if(!PySSLErrorObject) goto cleanup;

    PySSLCertVerificationErrorObject = PyObject_GetAttrString(ssl_module, "SSLCertVerificationError");
    if(!PySSLCertVerificationErrorObject) goto cleanup;

    PySSLZeroReturnErrorObject = PyObject_GetAttrString(ssl_module, "SSLZeroReturnError");
    if(!PySSLZeroReturnErrorObject) goto cleanup;

    PySSLWantReadErrorObject = PyObject_GetAttrString(ssl_module, "SSLWantReadError");
    if(!PySSLWantReadErrorObject) goto cleanup;

    PySSLWantWriteErrorObject = PyObject_GetAttrString(ssl_module, "SSLWantWriteError");
    if(!PySSLWantWriteErrorObject) goto cleanup;

    PySSLSyscallErrorObject = PyObject_GetAttrString(ssl_module, "SSLSyscallError");
    if(!PySSLSyscallErrorObject) goto cleanup;

    PySSLEOFErrorObject = PyObject_GetAttrString(ssl_module, "SSLEOFError");
    if(!PySSLEOFErrorObject) goto cleanup;

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef _M_ARM64
    openssl_handle = GetModuleHandle(TEXT("libssl-3-arm64.dll"));
    crypto_handle =  GetModuleHandle(TEXT("libcrypto-3-arm64.dll"));
#else
    openssl_handle = GetModuleHandle(TEXT(openssl_version_number >= 0x30000000L ? "libssl-3.dll" : "libssl-1_1.dll"));
    crypto_handle =  GetModuleHandle(TEXT(openssl_version_number >= 0x30000000L ? "libcrypto-3.dll" : "libcrypto-1_1.dll"));
#endif
    if(!openssl_handle || !crypto_handle) goto cleanup;

    *(void**)&SSL_read_ex = GetProcAddress(openssl_handle, "SSL_read_ex");
    *(void**)&SSL_get_error = GetProcAddress(openssl_handle, "SSL_get_error");
    *(void**)&SSL_get_shutdown = GetProcAddress(openssl_handle, "SSL_get_shutdown");
    *(void**)&UnlockedSSL_ERR_peek_last_error = GetProcAddress(crypto_handle, "ERR_peek_last_error");
    *(void**)&UnlockedSSL_ERR_clear_error = GetProcAddress(crypto_handle, "ERR_clear_error");
    *(void**)&UnlockedSSL_ERR_reason_error_string = GetProcAddress(crypto_handle, "ERR_reason_error_string");
#else
    // Find library at "import ssl; print(ssl._ssl.__file__)"

    if (PyObject_HasAttrString(_ssl_module, "__file__")) {
        _ssl_module_path = PyObject_GetAttrString(_ssl_module, "__file__");
        if(!_ssl_module_path) goto error;
        openssl_handle = dlopen(PyUnicode_AsUTF8(_ssl_module_path), RTLD_LAZY | RTLD_NOLOAD);
    }

    if(!openssl_handle) goto error;

    *(void**)&SSL_read_ex = dlsym(openssl_handle, "SSL_read_ex");
    *(void**)&SSL_get_error = dlsym(openssl_handle, "SSL_get_error");
    *(void**)&SSL_get_shutdown = dlsym(openssl_handle, "SSL_get_shutdown");
    *(void**)&UnlockedSSL_ERR_peek_last_error = dlsym(openssl_handle, "ERR_peek_last_error");
    *(void**)&UnlockedSSL_ERR_clear_error = dlsym(openssl_handle, "ERR_clear_error");
    *(void**)&UnlockedSSL_ERR_reason_error_string = dlsym(openssl_handle, "ERR_reason_error_string");

error:
    if (!openssl_linked() && openssl_handle) dlclose(openssl_handle);
    Py_XDECREF(_ssl_module_path);
#endif

cleanup:
    Py_XDECREF(ssl_module);
    Py_XDECREF(_ssl_module);
    Py_XDECREF(py_openssl_version_number);
    if (!openssl_linked()) {
        Py_XDECREF(PySSLErrorObject);
        Py_XDECREF(PySSLZeroReturnErrorObject);
        Py_XDECREF(PySSLWantReadErrorObject);
        Py_XDECREF(PySSLWantWriteErrorObject);
        Py_XDECREF(PySSLEOFErrorObject);
        Py_XDECREF(PySSLCertVerificationErrorObject);
        Py_XDECREF(SSLSocketType);
    }
}

bool openssl_linked() {
    return SSL_read_ex &&
        SSL_get_error &&
        SSL_get_shutdown &&
        UnlockedSSL_ERR_peek_last_error &&
        UnlockedSSL_ERR_clear_error &&
        UnlockedSSL_ERR_reason_error_string &&
        SSLSocketType &&
        PySSLErrorObject &&
        PySSLCertVerificationErrorObject &&
        PySSLZeroReturnErrorObject &&
        PySSLWantReadErrorObject &&
        PySSLWantWriteErrorObject &&
        PySSLSyscallErrorObject &&
        PySSLEOFErrorObject;
}

static void
fill_and_set_sslerror(
                      PySSLSocket *sslsock, PyObject *type, int ssl_errno,
                      const char *errstr, int lineno, unsigned long errcode) {
    PyObject *err_value = NULL, *reason_obj = NULL, *lib_obj = NULL;
    PyObject *verify_obj = NULL, *verify_code_obj = NULL;
    PyObject *init_value, *msg, *key;

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
    if (errcode != 0) {
        int lib, reason;

        lib = ERR_GET_LIB(errcode);
        reason = ERR_GET_REASON(errcode);
        key = Py_BuildValue("ii", lib, reason);
        if (key == NULL)
            goto fail;
        if (errstr == NULL) {
            errstr = UnlockedSSL_ERR_reason_error_string(errcode);
        }
    }
#endif
    if (errstr == NULL)
        errstr = "unknown error";

    msg = PyUnicode_FromFormat("%s (unlocked_ssl.c:%d)", errstr, lineno);

    init_value = Py_BuildValue("iN", ERR_GET_REASON(ssl_errno), msg);
    if (init_value == NULL)
        goto fail;

    err_value = PyObject_CallObject(type, init_value);
    Py_DECREF(init_value);
    if (err_value == NULL)
        goto fail;

    if (reason_obj == NULL)
        reason_obj = Py_None;

    PyErr_SetObject(type, err_value);
    fail:
        Py_XDECREF(err_value);
        Py_XDECREF(verify_code_obj);
        Py_XDECREF(verify_obj);
}

static int
PySSL_ChainExceptions(PySSLSocket *sslsock) {
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 12
    if (sslsock->exc == NULL)
        return 0;

    _PyErr_ChainExceptions1(sslsock->exc);
    sslsock->exc = NULL;
#else
    if (sslsock->exc_type == NULL)
        return 0;

    _PyErr_ChainExceptions(sslsock->exc_type, sslsock->exc_value, sslsock->exc_tb);
    sslsock->exc_type = NULL;
    sslsock->exc_value = NULL;
    sslsock->exc_tb = NULL;
#endif
    return -1;
}

static PyObject *
UnlockedSSL_SetError(PySSLSocket *sslsock, const char *filename, int lineno)
{
    PyObject *type;
    const char *errstr = NULL;
    _PySSLError err;
    enum py_ssl_error p = PY_SSL_ERROR_NONE;
    unsigned long e = 0;

    assert(sslsock != NULL);

    type = PySSLErrorObject;

    // ERR functions are thread local, no need to lock them.
    e = UnlockedSSL_ERR_peek_last_error();

    if (sslsock->ssl != NULL) {
        err = sslsock->err;

        switch (err.ssl) {
        case SSL_ERROR_ZERO_RETURN:
            errstr = "TLS/SSL connection has been closed (EOF)";
            type = PySSLZeroReturnErrorObject;
            p = PY_SSL_ERROR_ZERO_RETURN;
            break;
        case SSL_ERROR_WANT_READ:
            errstr = "The operation did not complete (read)";
            type = PySSLWantReadErrorObject;
            p = PY_SSL_ERROR_WANT_READ;
            break;
        case SSL_ERROR_WANT_WRITE:
            p = PY_SSL_ERROR_WANT_WRITE;
            type = PySSLWantWriteErrorObject;
            errstr = "The operation did not complete (write)";
            break;
        case SSL_ERROR_WANT_X509_LOOKUP:
            p = PY_SSL_ERROR_WANT_X509_LOOKUP;
            errstr = "The operation did not complete (X509 lookup)";
            break;
        case SSL_ERROR_WANT_CONNECT:
            p = PY_SSL_ERROR_WANT_CONNECT;
            errstr = "The operation did not complete (connect)";
            break;
        case SSL_ERROR_SYSCALL:
        {
            if (e == 0) {
                /* underlying BIO reported an I/O error */
                UnlockedSSL_ERR_clear_error();
#ifdef MS_WINDOWS
                if (err.ws) {
                    return PyErr_SetFromWindowsErr(err.ws);
                }
#endif
                if (err.c) {
                    errno = err.c;
                    return PyErr_SetFromErrno(PyExc_OSError);
                }
                else {
                    p = PY_SSL_ERROR_EOF;
                    type = PySSLEOFErrorObject;
                    errstr = "EOF occurred in violation of protocol";
                }
            } else {
                if (ERR_GET_LIB(e) == ERR_LIB_SSL &&
                        ERR_GET_REASON(e) == SSL_R_CERTIFICATE_VERIFY_FAILED) {
                    type = PySSLSyscallErrorObject;
                }
                if (ERR_GET_LIB(e) == ERR_LIB_SYS) {
                    // A system error is being reported; reason is set to errno
                    errno = ERR_GET_REASON(e);
                    return PyErr_SetFromErrno(PyExc_OSError);
                }
                p = PY_SSL_ERROR_SYSCALL;
            }
            break;
        }
        case SSL_ERROR_SSL:
        {
            p = PY_SSL_ERROR_SSL;
            if (e == 0) {
                /* possible? */
                errstr = "A failure in the SSL library occurred";
            }
            if (ERR_GET_LIB(e) == ERR_LIB_SSL &&
                    ERR_GET_REASON(e) == SSL_R_CERTIFICATE_VERIFY_FAILED) {
                type = PySSLCertVerificationErrorObject;
            }
            if (openssl_version_number >= 0x30000000L) {
                /* OpenSSL 3.0 changed transport EOF from SSL_ERROR_SYSCALL with
                 * zero return value to SSL_ERROR_SSL with a special error code. */
                if (ERR_GET_LIB(e) == ERR_LIB_SSL &&
                        ERR_GET_REASON(e) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                    p = PY_SSL_ERROR_EOF;
                    type = PySSLEOFErrorObject;
                    errstr = "EOF occurred in violation of protocol";
                }
            }
            if (ERR_GET_LIB(e) == ERR_LIB_SYS) {
                // A system error is being reported; reason is set to errno
                errno = ERR_GET_REASON(e);
                return PyErr_SetFromErrno(PyExc_OSError);
            }
            break;
        }
        default:
            p = PY_SSL_ERROR_INVALID_ERROR_CODE;
            errstr = "Invalid error code";
        }
    }
    fill_and_set_sslerror(sslsock, type, p, errstr, lineno, e);
    UnlockedSSL_ERR_clear_error();
    PySSL_ChainExceptions(sslsock);
    return NULL;
}

static PyObject* unlocked_ssl_recv_into_impl(PySSLSocket *self, Py_ssize_t len, Py_buffer *buffer) {
    char *mem;
    size_t count = 0;
    size_t readbytes = 0;
    int retval;
    int sockstate;
    _PySSLError err;
    PyObject *sock = GET_SOCKET(self);

    mem = (char *)buffer->buf;
    if (len <= 0 || len > buffer->len) {
        len = (int) buffer->len;
        if (buffer->len != len) {
            PyErr_SetString(PyExc_OverflowError,
                            "maximum length can't fit in a C 'int'");
            goto error;
        }
        if (len == 0) {
            count = 0;
            goto done;
        }
    }

    if (sock != NULL) {
        if (((PyObject*)sock) == Py_None) {
            PyErr_SetString(PyExc_ValueError, "Underlying socket connection gone");
            return NULL;
        }
        Py_INCREF(sock);
    }

    do {
        Py_BEGIN_ALLOW_THREADS;
        do {
            retval = SSL_read_ex(self->ssl, mem + count, len, &readbytes);
            if (retval <= 0) {
                break;
            }
            count += readbytes;
            len -= readbytes;
        } while (len > 0);
        err = _PySSL_errno(retval == 0, self->ssl, retval);
        Py_END_ALLOW_THREADS;
        self->err = err;

        if (count > 0) {
            break;
        }

        if (PyErr_CheckSignals())
            goto error;

        if (err.ssl == SSL_ERROR_WANT_READ) {
            sockstate = SOCKET_IS_NONBLOCKING;
        } else if (err.ssl == SSL_ERROR_WANT_WRITE) {
            sockstate = SOCKET_IS_NONBLOCKING;
        } else if (err.ssl == SSL_ERROR_ZERO_RETURN &&
                SSL_get_shutdown(self->ssl) == SSL_RECEIVED_SHUTDOWN)
        {
            count = 0;
            goto done;
        }
        else
            sockstate = SOCKET_OPERATION_OK;

        if (sockstate == SOCKET_IS_NONBLOCKING) {
            break;
        }
    } while (err.ssl == SSL_ERROR_WANT_READ ||
             err.ssl == SSL_ERROR_WANT_WRITE);

    if (count == 0 && retval == 0) {
        UnlockedSSL_SetError(self, __FILE__, __LINE__);
        goto error;
    }
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 12
    if (self->exc != NULL)
        goto error;
#else
    if (self->exc_type != NULL)
        goto error;
#endif

done:
    Py_XDECREF(sock);
    return PyLong_FromSize_t(count);

error:
    PySSL_ChainExceptions(self);
    Py_XDECREF(sock);
    return NULL;
}

PyObject* unlocked_ssl_recv_into(PyObject* self, PyObject* args) {
    PyObject *ssl_socket;
    PyObject *Py_ssl_socket;
    Py_ssize_t len;
    Py_buffer Py_buffer;
    PyObject *retval = NULL;
    PyObject *blocking = NULL;

    if(!openssl_linked()) {
        PyErr_SetString(PyExc_OSError, "Failed to link with OpenSSL");
        return NULL;
    }

    // Parse input
    if (!PyArg_ParseTuple(args, "O!w*:unlocked_ssl_recv_into", SSLSocketType, &ssl_socket, &Py_buffer)) {
        return NULL;
    }

    Py_ssl_socket = PyObject_GetAttrString(ssl_socket, "_sslobj");
    if (!Py_ssl_socket) {
        PyErr_SetString(PyExc_ValueError, "Could not find _sslobj attribute");
        goto error;
    }

    blocking = PyObject_CallMethod(ssl_socket, "getblocking", NULL);
    if (blocking == Py_True) {
        PyErr_SetString(PyExc_ValueError, "Only non-blocking sockets are supported");
        goto error;
    }

    // Basic sanity check
    len = (Py_ssize_t)Py_buffer.len;
    if (len <= 0) {
        PyErr_SetString(PyExc_ValueError, "No space left in buffer");
        goto error;
    }

    retval = unlocked_ssl_recv_into_impl((PySSLSocket*)Py_ssl_socket, len, &Py_buffer);

error:
    PyBuffer_Release(&Py_buffer);
    Py_XDECREF(Py_ssl_socket);
    Py_XDECREF(blocking);
    return retval;
}
