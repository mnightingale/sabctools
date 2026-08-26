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

#ifndef SABCTOOLS_TCPINFO_H
#define SABCTOOLS_TCPINFO_H

#include <Python.h>

/* What the kernel knows about one TCP connection, or None. Requires the GIL. */
PyObject* tcp_info(PyObject *, PyObject *);

/* Whether this build has a way to ask at all */
bool tcp_info_supported();

#endif // SABCTOOLS_TCPINFO_H
