/*
 * Copyright 2007-2023 The SABnzbd-Team (sabnzbd.org)
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

#ifndef SABCTOOLS_UNLOCKED_SSL_H
#define SABCTOOLS_UNLOCKED_SSL_H

#include <Python.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>

/* OpenSSL link */
#if defined(_WIN32) || defined(__CYGWIN__)
# define WIN32_LEAN_AND_MEAN
# include <Windows.h>
# include <winsock2.h>
#else
# include <dlfcn.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ssl module state
 */
typedef struct {
    /* Types */
    PyTypeObject *PySSLContext_Type;
    PyTypeObject *PySSLSocket_Type;
    PyTypeObject *PySSLMemoryBIO_Type;
    PyTypeObject *PySSLSession_Type;
    PyTypeObject *PySSLCertificate_Type;
    /* SSL error object */
    PyObject *PySSLErrorObject;
    PyObject *PySSLCertVerificationErrorObject;
    PyObject *PySSLZeroReturnErrorObject;
    PyObject *PySSLWantReadErrorObject;
    PyObject *PySSLWantWriteErrorObject;
    PyObject *PySSLSyscallErrorObject;
    PyObject *PySSLEOFErrorObject;
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 12
    /* Error mappings */
    PyObject *err_codes_to_names;
    PyObject *lib_codes_to_names;
    /* socket type from module CAPI */
    PyTypeObject *Sock_Type;
    /* Interned strings */
    PyObject *str_library;
    PyObject *str_reason;
    PyObject *str_verify_code;
    PyObject *str_verify_message;
    /* keylog lock */
    PyThread_type_lock keylog_lock;
#elif  PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 11
    /* Error mappings */
    PyObject *err_codes_to_names;
    PyObject *err_names_to_codes;
    PyObject *lib_codes_to_names;
    /* socket type from module CAPI */
    PyTypeObject *Sock_Type;
    /* Interned strings */
    PyObject *str_library;
    PyObject *str_reason;
    PyObject *str_verify_code;
    PyObject *str_verify_message;
#elif  PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
    /* Error mappings */
    PyObject *err_codes_to_names;
    PyObject *err_names_to_codes;
    PyObject *lib_codes_to_names;
    /* socket type from module CAPI */
    PyTypeObject *Sock_Type;
#endif
} _sslmodulestate;

/* Have to manually define this OpenSSL constant and hope it never changes */
# define SSL_ERROR_SSL 1
# define SSL_RECEIVED_SHUTDOWN 2
# define SSL_ERROR_WANT_READ 2
# define SSL_ERROR_WANT_WRITE 3
# define SSL_ERROR_WANT_X509_LOOKUP 4
# define SSL_ERROR_SYSCALL 5
# define SSL_ERROR_ZERO_RETURN 6
# define SSL_ERROR_WANT_CONNECT 7
# define SSL_ERROR_WANT_ACCEPT 8
# define SSL_R_CERTIFICATE_VERIFY_FAILED 134
# define SSL_R_UNEXPECTED_EOF_WHILE_READING 294
void openssl_init();
bool openssl_linked();
PyObject *unlocked_ssl_recv_into(PyObject *, PyObject*);

#ifdef __cplusplus
}
#endif
#endif