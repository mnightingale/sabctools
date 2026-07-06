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

#include "sabctools.h"
#include "yenc.h"
#include "unlocked_ssl.h"
#include "crc32.h"
#include "sparse.h"
#include "utils.h"

/* Function and exception declarations */
PyMODINIT_FUNC PyInit_sabctools(void);

/* Python API requirements */
static PyMethodDef sabctools_methods[] = {
    {
        "yenc_encode",
        yenc_encode,
        METH_O,
        "yenc_encode(input_string)"
    },
    {
        "unlocked_ssl_recv_into",
        unlocked_ssl_recv_into,
        METH_VARARGS,
        "unlocked_ssl_recv_into(ssl_socket, buffer)"
    },
    {
        "crc32_combine",
        crc32_combine,
        METH_VARARGS,
        "crc32_combine(crc1, crc2, length)"
    },
    {
        "crc32_multiply",
        crc32_multiply,
        METH_VARARGS,
        "crc32_multiply(crc1, crc2)"
    },
    {
        "crc32_zero_unpad",
        crc32_zero_unpad,
        METH_VARARGS,
        "crc32_zero_unpad(crc1, length)"
    },
    {
        "crc32_xpown",
        crc32_xpown,
        METH_O,
        "crc32_xpown(n)"
    },
    {
        "crc32_xpow8n",
        crc32_xpow8n,
        METH_O,
        "crc32_xpow8n(n)"
    },
    {
        "sparse",
        sparse,
        METH_VARARGS,
        "sparse(handle, length)"
    },
    {
        "bytearray_malloc",
        bytearray_malloc,
        METH_O,
        "bytearray_malloc(size)"
    },
    {
        "rarfile_rar3_loop",
        rarfile_rar3_loop,
        METH_VARARGS,
        "rarfile_rar3_loop(sha1, seed, base)"
    },
    {NULL, NULL, 0, NULL}
};

static const char* simd_detected(void) {
    int level = RapidYenc::decode_isa_level();
#ifdef PLATFORM_X86
    if(level >= ISA_LEVEL_VBMI2)
        return "AVX512VL+VBMI2";
    if(level >= ISA_LEVEL_AVX3)
        return "AVX512VL";
    if(level >= ISA_LEVEL_AVX2)
        return "AVX2";
    if(level >= ISA_LEVEL_AVX)
        return "AVX";
    if(level >= ISA_LEVEL_SSE4_POPCNT)
        return "SSE4.1+POPCNT";
    if(level >= ISA_LEVEL_SSE41)
        return "SSE4.1";
    if(level >= ISA_LEVEL_SSSE3)
        return "SSSE3";
    if(level >= (ISA_LEVEL_SSE2 | ISA_FEATURE_POPCNT | ISA_FEATURE_LZCNT))
        return "SSE2+ABM";
    return "SSE2";
#endif
#ifdef PLATFORM_ARM
    if(level >= ISA_LEVEL_NEON) {
        return "NEON";
    }
#endif
#ifdef __riscv
    if(level >= ISA_LEVEL_RVV) {
        return "RVV";
    }
#endif
    return "";
}

static int sabctools_exec(PyObject *m)
{
    if (yenc_init(m) < 0)
        return -1;

    if (openssl_init(m) < 0)
        return -1;

    if (sparse_init(m) < 0)
        return -1;

    // Initialize and add version / SIMD information
    if (PyModule_AddStringConstant(m, "version", SABCTOOLS_VERSION) < 0)
        return -1;

    if (PyModule_AddStringConstant(m, "simd", simd_detected()) < 0)
        return -1;

    PyObject *linked = PyBool_FromLong(openssl_linked());
    if (linked == nullptr)
        return -1;

    if (PyModule_AddObject(m, "openssl_linked", linked) < 0) {
        Py_DECREF(linked);
        return -1;
    }

    return 0;
}

static int sabctools_traverse(PyObject *module, visitproc visit, void *arg)
{
    const auto *state = static_cast<sabctools_state *>(
        PyModule_GetState(module)
    );

    Py_VISIT(state->DecoderType);
    Py_VISIT(state->NNTPResponseType);
    Py_VISIT(state->EncodingFormat);
    Py_VISIT(state->ENCODING_FORMAT_YENC);
    Py_VISIT(state->ENCODING_FORMAT_UU);

    return 0;
}

static int sabctools_clear(PyObject *module)
{
    auto *state = static_cast<sabctools_state *>(
        PyModule_GetState(module)
    );

    Py_CLEAR(state->DecoderType);
    Py_CLEAR(state->NNTPResponseType);
    Py_CLEAR(state->EncodingFormat);
    Py_CLEAR(state->ENCODING_FORMAT_YENC);
    Py_CLEAR(state->ENCODING_FORMAT_UU);

    return 0;
}

static PyModuleDef_Slot sabctools_slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(sabctools_exec)},
    {0, NULL}
};

static PyModuleDef sabctools_definition = {
    PyModuleDef_HEAD_INIT,
    "sabctools",
    "Utils written in C for use within SABnzbd.",
    sizeof(sabctools_state),
    sabctools_methods,
    sabctools_slots,
    sabctools_traverse,
    sabctools_clear,
};


PyMODINIT_FUNC PyInit_sabctools(void) {
    return PyModuleDef_Init(&sabctools_definition);
}


