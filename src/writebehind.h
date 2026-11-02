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

#ifndef SABCTOOLS_WRITEBEHIND_H
#define SABCTOOLS_WRITEBEHIND_H

#include <Python.h>

#include "filewriter.h"

/*
 * A bounded pool of staging buffers and the threads that write them out.
 *
 * A decoder streaming to a sink takes a buffer from the pool, fills it, and submits
 * it; a writer thread performs the positional write and returns the buffer. Buffers
 * are allocated on first use up to the limit. When all of them are in use, taking a
 * buffer blocks until a write completes.
 *
 * None of these touch the Python API. writebehind_acquire and writebehind_drain may
 * block, so callers release the GIL around them.
 */

bool writebehind_enabled();

/* Size of every pooled buffer */
Py_ssize_t writebehind_buffer_size();

/* A free buffer, or NULL if out of memory, or if none is free and wait is false */
char *writebehind_acquire(bool wait);

void writebehind_release(char *buffer);

/* Queue a write of length bytes of buffer at offset. The pool takes the buffer back afterwards. */
void writebehind_submit(FileWriter *writer, char *buffer, Py_ssize_t length, long long offset);

/* Wait until every write submitted for this writer has completed */
void writebehind_drain(FileWriter *writer);

PyObject *writebehind_configure(PyObject *, PyObject *, PyObject *);

PyObject *writebehind_stats(PyObject *, PyObject *, PyObject *);

/* Free every idle buffer, and each in-use buffer as it is returned, until the next acquire */
PyObject *writebehind_trim(PyObject *, PyObject *);

#endif // SABCTOOLS_WRITEBEHIND_H
