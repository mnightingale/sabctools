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

#include "crc32.h"
#include "yencode/crc.h"

PyObject* crc32_combine(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 3) {
        PyErr_Format(PyExc_TypeError, "crc32_combine() takes exactly 3 arguments (%zd given)", nargs);
        return NULL;
    }

    unsigned long crc1 = PyLong_AsUnsignedLong(args[0]);
    if (PyErr_Occurred())
        return NULL;

    unsigned long crc2 = PyLong_AsUnsignedLong(args[1]);
    if (PyErr_Occurred())
        return NULL;

    unsigned long long length = PyLong_AsUnsignedLongLong(args[2]);
    if (PyErr_Occurred())
        return NULL;

    return PyLong_FromUnsignedLong(RapidYenc::crc32_combine(crc1, crc2, length));
}

PyObject* crc32_multiply(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 2) {
        PyErr_Format(PyExc_TypeError, "crc32_multiply() takes exactly 2 arguments (%zd given)", nargs);
        return NULL;
    }

    unsigned long crc1 = PyLong_AsUnsignedLong(args[0]);
    if (PyErr_Occurred())
        return NULL;

    unsigned long crc2 = PyLong_AsUnsignedLong(args[1]);
    if (PyErr_Occurred())
        return NULL;

    return PyLong_FromUnsignedLong(RapidYenc::crc32_multiply(crc1, crc2));
}

PyObject* crc32_zero_unpad(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 2) {
        PyErr_Format(PyExc_TypeError, "crc32_zero_unpad() takes exactly 2 arguments (%zd given)", nargs);
        return NULL;
    }

    unsigned long crc1 = PyLong_AsUnsignedLong(args[0]);
    if (PyErr_Occurred())
        return NULL;

    unsigned long long length = PyLong_AsUnsignedLongLong(args[1]);
    if (PyErr_Occurred())
        return NULL;

    return PyLong_FromUnsignedLong(RapidYenc::crc32_unzero(crc1, length));
}

PyObject* crc32_xpown(PyObject* self, PyObject* arg) {
    long long n = PyLong_AsLongLong(arg);

    if (PyErr_Occurred()) {
        return NULL;
    }

    unsigned long result = RapidYenc::crc32_2pow(n);

    return PyLong_FromUnsignedLong(result);
}

PyObject* crc32_xpow8n(PyObject* self, PyObject* arg) {
    unsigned long long n = PyLong_AsUnsignedLongLong(arg);

    if (PyErr_Occurred()) {
        return NULL;
    }

    unsigned long result = RapidYenc::crc32_256pow(n);

    return PyLong_FromUnsignedLong(result);
}
