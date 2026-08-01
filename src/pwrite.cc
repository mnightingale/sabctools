/*
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

#include "pwrite.h"

#if defined(_WIN32) || defined(__CYGWIN__)

/* Maximum number of buffers accepted by pwritev, mirroring the IOV_MAX most
   POSIX platforms expose, so behaviour stays comparable across platforms. */
#define SABCTOOLS_IOV_MAX 1024

/* _get_osfhandle invokes the CRT invalid parameter handler for an out of range
   descriptor, which terminates the process. CPython has _Py_BEGIN_SUPPRESS_IPH for
   this, but it is only available to core builds, so keep a local equivalent. */
#if defined(_MSC_VER) && _MSC_VER >= 1900
static void silent_invalid_parameter_handler(
    const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
{
}
#define SABCTOOLS_BEGIN_SUPPRESS_IPH { \
    _invalid_parameter_handler previous_handler = \
        _set_thread_local_invalid_parameter_handler(silent_invalid_parameter_handler);
#define SABCTOOLS_END_SUPPRESS_IPH _set_thread_local_invalid_parameter_handler(previous_handler); }
#else
#define SABCTOOLS_BEGIN_SUPPRESS_IPH
#define SABCTOOLS_END_SUPPRESS_IPH
#endif

static HANDLE handle_from_fd(int fd)
{
    HANDLE handle;
    SABCTOOLS_BEGIN_SUPPRESS_IPH
    handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    SABCTOOLS_END_SUPPRESS_IPH
    if (handle == INVALID_HANDLE_VALUE) {
        PyErr_SetFromErrno(PyExc_OSError);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

/**
 * Write a single buffer at an absolute offset.
 *
 * WriteFile with the offset supplied in an OVERLAPPED structure is a single
 * syscall that ignores the shared file pointer, so concurrent writers to the
 * same descriptor do not need to be serialized by a lock.
 *
 * @return true on success, with `written` set; false with the Windows error raised
 */
static bool write_at(HANDLE handle, const void *buf, Py_ssize_t len, unsigned long long offset, DWORD *written)
{
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);

    // A single WriteFile cannot exceed a DWORD, callers deal with the short write
    DWORD chunk = static_cast<DWORD>(Py_MIN(len, static_cast<Py_ssize_t>(INT_MAX)));

    BOOL success;
    DWORD error = 0;
    // os.pwrite also drops the GIL, a slow disk should not stall other threads
    Py_BEGIN_ALLOW_THREADS
    success = WriteFile(handle, buf, chunk, written, &overlapped);
    if (!success) {
        // Capture before reacquiring the GIL, which can clobber the thread error
        error = GetLastError();
    }
    Py_END_ALLOW_THREADS

    if (!success) {
        // Maps the Windows error onto errno, so callers can keep checking for ENOSPC
        PyErr_SetFromWindowsErr(static_cast<int>(error));
        return false;
    }
    return true;
}

PyObject *sabctools_pwrite(PyObject *self, PyObject *args)
{
    int fd;
    Py_buffer buffer;
    long long offset;

    if (!PyArg_ParseTuple(args, "iy*L:pwrite", &fd, &buffer, &offset)) {
        return NULL;
    }

    if (offset < 0) {
        PyBuffer_Release(&buffer);
        errno = EINVAL;
        return PyErr_SetFromErrno(PyExc_OSError);
    }

    HANDLE handle = handle_from_fd(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        PyBuffer_Release(&buffer);
        return NULL;
    }

    DWORD written = 0;
    bool success = write_at(handle, buffer.buf, buffer.len, static_cast<unsigned long long>(offset), &written);
    PyBuffer_Release(&buffer);

    if (!success) {
        return NULL;
    }
    return PyLong_FromUnsignedLong(written);
}

PyObject *sabctools_pwritev(PyObject *self, PyObject *args)
{
    int fd;
    PyObject *Py_buffers;
    long long offset;

    if (!PyArg_ParseTuple(args, "iOL:pwritev", &fd, &Py_buffers, &offset)) {
        return NULL;
    }

    if (offset < 0) {
        errno = EINVAL;
        return PyErr_SetFromErrno(PyExc_OSError);
    }

    PyObject *sequence = PySequence_Fast(Py_buffers, "pwritev() arg 2 must be a sequence");
    if (sequence == NULL) {
        return NULL;
    }

    Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
    if (count > SABCTOOLS_IOV_MAX) {
        Py_DECREF(sequence);
        errno = EINVAL;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    if (count == 0) {
        Py_DECREF(sequence);
        return PyLong_FromUnsignedLongLong(0);
    }

    Py_buffer *buffers = static_cast<Py_buffer *>(PyMem_Malloc(sizeof(Py_buffer) * count));
    if (buffers == NULL) {
        Py_DECREF(sequence);
        return PyErr_NoMemory();
    }

    // Acquire every buffer up front so a bad entry fails before anything is written
    Py_ssize_t acquired = 0;
    for (; acquired < count; acquired++) {
        if (PyObject_GetBuffer(PySequence_Fast_GET_ITEM(sequence, acquired), &buffers[acquired], PyBUF_SIMPLE) < 0) {
            break;
        }
    }
    Py_DECREF(sequence);

    HANDLE handle = INVALID_HANDLE_VALUE;
    unsigned long long total = 0;
    bool success = acquired == count;

    if (success) {
        handle = handle_from_fd(fd);
        success = handle != INVALID_HANDLE_VALUE;
    }

    if (success) {
        // Windows has no buffered scatter/gather write, WriteFileGather requires page-aligned
        // unbuffered handles, so emulate by writing each buffer at an advancing offset
        for (Py_ssize_t i = 0; i < count; i++) {
            if (buffers[i].len == 0) {
                continue;
            }
            DWORD written = 0;
            if (!write_at(handle, buffers[i].buf, buffers[i].len, offset + total, &written)) {
                success = false;
                break;
            }
            total += written;
            if (static_cast<Py_ssize_t>(written) < buffers[i].len) {
                // Short write, report what was written and let the caller retry
                break;
            }
        }
    }

    for (Py_ssize_t i = 0; i < acquired; i++) {
        PyBuffer_Release(&buffers[i]);
    }
    PyMem_Free(buffers);

    if (!success) {
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(total);
}

#else

static PyObject *unsupported_platform(const char *name)
{
    PyErr_Format(
        PyExc_NotImplementedError,
        "sabctools.%s is only available on Windows, use os.%s instead",
        name,
        name
    );
    return NULL;
}

PyObject *sabctools_pwrite(PyObject *self, PyObject *args)
{
    return unsupported_platform("pwrite");
}

PyObject *sabctools_pwritev(PyObject *self, PyObject *args)
{
    return unsupported_platform("pwritev");
}

#endif
