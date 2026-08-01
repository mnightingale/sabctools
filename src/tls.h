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

#ifndef SABCTOOLS_TLS_H
#define SABCTOOLS_TLS_H

#include <Python.h>

#ifdef SABCTOOLS_AWS_LC

/* Registers the TLSContext and TLSSocket types plus the aws_lc_version constant
   on the module. Returns false with an exception set on failure. */
bool tls_init(PyObject *module);

#endif /* SABCTOOLS_AWS_LC */
#endif /* SABCTOOLS_TLS_H */
