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

#include "tcpinfo.h"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// Declared by the SDK only when the build targets Windows 10 1703 or later
#if defined(SIO_TCP_INFO)
#define SABCTOOLS_TCP_INFO 1
#endif
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifndef TCP_INFO
#define TCP_INFO 11
#endif
#define SABCTOOLS_TCP_INFO 1
#elif defined(__APPLE__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define SABCTOOLS_TCP_INFO 1
#endif

bool tcp_info_supported() {
#ifdef SABCTOOLS_TCP_INFO
    return true;
#else
    return false;
#endif
}

#ifdef SABCTOOLS_TCP_INFO

static int set_int(PyObject* dict, const char* key, uint64_t value) {
    PyObject* number = PyLong_FromUnsignedLongLong(value);
    if (!number) return -1;
    int result = PyDict_SetItemString(dict, key, number);
    Py_DECREF(number);
    return result;
}

static int set_none(PyObject* dict, const char* key) {
    return PyDict_SetItemString(dict, key, Py_None);
}

#endif // SABCTOOLS_TCP_INFO

#if defined(__linux__) && defined(SABCTOOLS_TCP_INFO)

/*
 * Declared here rather than taken from <linux/tcp.h> because the kernel headers a wheel
 * is built against are older than the kernels it runs on: manylinux2014 predates
 * tcpi_bytes_received, tcpi_min_rtt, tcpi_bytes_retrans and tcpi_rcv_ooopack, so
 * compiling against the system definition would drop the most useful fields on exactly
 * the builds users install.
 *
 * The struct only ever grows, so the length getsockopt reports back says which fields
 * this kernel actually filled in. Offsets are asserted below.
 */
struct sabctools_tcp_info {
    uint8_t  tcpi_state;
    uint8_t  tcpi_ca_state;
    uint8_t  tcpi_retransmits;
    uint8_t  tcpi_probes;
    uint8_t  tcpi_backoff;
    uint8_t  tcpi_options;
    uint8_t  tcpi_wscale;      // snd_wscale:4, rcv_wscale:4
    uint8_t  tcpi_delivery_rate_app_limited; // plus fastopen_client_fail:2

    uint32_t tcpi_rto;
    uint32_t tcpi_ato;
    uint32_t tcpi_snd_mss;
    uint32_t tcpi_rcv_mss;

    uint32_t tcpi_unacked;
    uint32_t tcpi_sacked;
    uint32_t tcpi_lost;
    uint32_t tcpi_retrans;
    uint32_t tcpi_fackets;

    uint32_t tcpi_last_data_sent;
    uint32_t tcpi_last_ack_sent;
    uint32_t tcpi_last_data_recv;
    uint32_t tcpi_last_ack_recv;

    uint32_t tcpi_pmtu;
    uint32_t tcpi_rcv_ssthresh;
    uint32_t tcpi_rtt;
    uint32_t tcpi_rttvar;
    uint32_t tcpi_snd_ssthresh;
    uint32_t tcpi_snd_cwnd;
    uint32_t tcpi_advmss;
    uint32_t tcpi_reordering;

    uint32_t tcpi_rcv_rtt;
    uint32_t tcpi_rcv_space;

    uint32_t tcpi_total_retrans;

    uint64_t tcpi_pacing_rate;
    uint64_t tcpi_max_pacing_rate;
    uint64_t tcpi_bytes_acked;
    uint64_t tcpi_bytes_received;
    uint32_t tcpi_segs_out;
    uint32_t tcpi_segs_in;

    uint32_t tcpi_notsent_bytes;
    uint32_t tcpi_min_rtt;
    uint32_t tcpi_data_segs_in;
    uint32_t tcpi_data_segs_out;

    uint64_t tcpi_delivery_rate;

    uint64_t tcpi_busy_time;
    uint64_t tcpi_rwnd_limited;
    uint64_t tcpi_sndbuf_limited;

    uint32_t tcpi_delivered;
    uint32_t tcpi_delivered_ce;

    uint64_t tcpi_bytes_sent;
    uint64_t tcpi_bytes_retrans;
    uint32_t tcpi_dsack_dups;
    uint32_t tcpi_reord_seen;

    uint32_t tcpi_rcv_ooopack;

    uint32_t tcpi_snd_wnd;
    uint32_t tcpi_rcv_wnd;

    uint32_t tcpi_rehash;

    uint16_t tcpi_total_rto;
    uint16_t tcpi_total_rto_recoveries;
    uint32_t tcpi_total_rto_time;
};

#define ASSERT_OFFSET(field, expected) \
    static_assert(offsetof(struct sabctools_tcp_info, field) == (expected), #field " moved")

ASSERT_OFFSET(tcpi_rto, 8);
ASSERT_OFFSET(tcpi_snd_mss, 16);
ASSERT_OFFSET(tcpi_rtt, 68);
ASSERT_OFFSET(tcpi_rttvar, 72);
ASSERT_OFFSET(tcpi_rcv_rtt, 92);
ASSERT_OFFSET(tcpi_rcv_space, 96);
ASSERT_OFFSET(tcpi_total_retrans, 100);
ASSERT_OFFSET(tcpi_bytes_received, 128);
ASSERT_OFFSET(tcpi_min_rtt, 148);
ASSERT_OFFSET(tcpi_bytes_retrans, 208);
ASSERT_OFFSET(tcpi_reord_seen, 220);
ASSERT_OFFSET(tcpi_rcv_ooopack, 224);
ASSERT_OFFSET(tcpi_snd_wnd, 228);
ASSERT_OFFSET(tcpi_rcv_wnd, 232);
static_assert(sizeof(struct sabctools_tcp_info) == 248, "tcp_info is not the expected size");

#define FILLED(len, field) \
    ((size_t)(len) >= offsetof(struct sabctools_tcp_info, field) + \
                      sizeof(((struct sabctools_tcp_info*)nullptr)->field))

// A field the kernel filled in, or None when this kernel is older than the field
#define SET_FIELD(dict, key, len, field) \
    (FILLED(len, field) ? set_int(dict, key, info.field) : set_none(dict, key))

static PyObject* read_tcp_info(int fd) {
    struct sabctools_tcp_info info;
    memset(&info, 0, sizeof(info));
    socklen_t length = sizeof(info);

    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &length) != 0) {
        Py_RETURN_NONE;
    }

    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    int failed = 0;
    failed |= PyDict_SetItemString(dict, "source", PyUnicode_InternFromString("linux")) < 0;
    failed |= SET_FIELD(dict, "state", length, tcpi_state) < 0;
    failed |= SET_FIELD(dict, "mss", length, tcpi_snd_mss) < 0;
    failed |= SET_FIELD(dict, "rtt", length, tcpi_rtt) < 0;
    failed |= SET_FIELD(dict, "rtt_var", length, tcpi_rttvar) < 0;
    failed |= SET_FIELD(dict, "min_rtt", length, tcpi_min_rtt) < 0;
    failed |= SET_FIELD(dict, "rcv_rtt", length, tcpi_rcv_rtt) < 0;
    failed |= SET_FIELD(dict, "rcv_space", length, tcpi_rcv_space) < 0;
    failed |= SET_FIELD(dict, "rcv_wnd", length, tcpi_rcv_wnd) < 0;
    failed |= set_none(dict, "rcv_buf") < 0;
    failed |= SET_FIELD(dict, "bytes_received", length, tcpi_bytes_received) < 0;
    failed |= set_none(dict, "bytes_reordered") < 0;
    failed |= SET_FIELD(dict, "packets_reordered", length, tcpi_rcv_ooopack) < 0;
    failed |= SET_FIELD(dict, "bytes_retrans_out", length, tcpi_bytes_retrans) < 0;

    if (failed) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

#elif defined(__APPLE__) && defined(SABCTOOLS_TCP_INFO)

static PyObject* read_tcp_info(int fd) {
    struct tcp_connection_info info;
    memset(&info, 0, sizeof(info));
    socklen_t length = sizeof(info);

    if (getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &length) != 0) {
        Py_RETURN_NONE;
    }
    if (length < sizeof(info)) {
        Py_RETURN_NONE;
    }

    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    int failed = 0;
    failed |= PyDict_SetItemString(dict, "source", PyUnicode_InternFromString("macos")) < 0;
    failed |= set_int(dict, "state", info.tcpi_state) < 0;
    failed |= set_int(dict, "mss", info.tcpi_maxseg) < 0;
    // Reported in milliseconds, so the value is coarse even though the unit is not
    failed |= set_int(dict, "rtt", (uint64_t)info.tcpi_srtt * 1000) < 0;
    failed |= set_int(dict, "rtt_var", (uint64_t)info.tcpi_rttvar * 1000) < 0;
    failed |= set_none(dict, "min_rtt") < 0;
    failed |= set_none(dict, "rcv_rtt") < 0;
    failed |= set_none(dict, "rcv_space") < 0;
    failed |= set_int(dict, "rcv_wnd", info.tcpi_rcv_wnd) < 0;
    failed |= set_none(dict, "rcv_buf") < 0;
    failed |= set_int(dict, "bytes_received", info.tcpi_rxbytes) < 0;
    failed |= set_int(dict, "bytes_reordered", info.tcpi_rxoutoforderbytes) < 0;
    failed |= set_none(dict, "packets_reordered") < 0;
    failed |= set_int(dict, "bytes_retrans_out", info.tcpi_txretransmitbytes) < 0;

    if (failed) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

#elif defined(_WIN32) && defined(SABCTOOLS_TCP_INFO)

static PyObject* read_tcp_info(int fd) {
    TCP_INFO_v0 info;
    memset(&info, 0, sizeof(info));
    DWORD version = 0;
    DWORD returned = 0;

    if (WSAIoctl((SOCKET)fd, SIO_TCP_INFO, &version, sizeof(version),
                 &info, sizeof(info), &returned, nullptr, nullptr) != 0) {
        Py_RETURN_NONE;
    }
    if (returned < sizeof(info)) {
        Py_RETURN_NONE;
    }

    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    int failed = 0;
    failed |= PyDict_SetItemString(dict, "source", PyUnicode_InternFromString("windows")) < 0;
    failed |= set_int(dict, "state", info.State) < 0;
    failed |= set_int(dict, "mss", info.Mss) < 0;
    failed |= set_int(dict, "rtt", info.RttUs) < 0;
    failed |= set_none(dict, "rtt_var") < 0;
    failed |= set_int(dict, "min_rtt", info.MinRttUs) < 0;
    failed |= set_none(dict, "rcv_rtt") < 0;
    failed |= set_none(dict, "rcv_space") < 0;
    failed |= set_int(dict, "rcv_wnd", info.RcvWnd) < 0;
    failed |= set_int(dict, "rcv_buf", info.RcvBuf) < 0;
    failed |= set_int(dict, "bytes_received", info.BytesIn) < 0;
    failed |= set_int(dict, "bytes_reordered", info.BytesReordered) < 0;
    failed |= set_none(dict, "packets_reordered") < 0;
    failed |= set_int(dict, "bytes_retrans_out", info.BytesRetrans) < 0;

    if (failed) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

#endif

PyObject* tcp_info(PyObject* self, PyObject* socket) {
    int fd = PyObject_AsFileDescriptor(socket);
    if (fd < 0) {
        // A closed socket is an ordinary outcome for a caller sampling connections that
        // come and go, so only a genuinely wrong argument is worth raising over
        if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            return nullptr;
        }
        PyErr_Clear();
        Py_RETURN_NONE;
    }

#ifdef SABCTOOLS_TCP_INFO
    return read_tcp_info(fd);
#else
    Py_RETURN_NONE;
#endif
}
