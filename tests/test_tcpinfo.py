import socket
import threading

import pytest

from tests.testsupport import *

TCP_FIELDS = (
    "state",
    "mss",
    "rtt",
    "rtt_var",
    "min_rtt",
    "rcv_rtt",
    "rcv_space",
    "rcv_wnd",
    "rcv_buf",
    "bytes_received",
    "bytes_reordered",
    "packets_reordered",
    "bytes_retrans_out",
)


@pytest.fixture
def connection():
    """A connected TCP socket pair on loopback, with data already through it"""
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    accepted = []

    def serve():
        conn, _ = listener.accept()
        accepted.append(conn)
        try:
            conn.sendall(b"x" * (256 * 1024))
        except OSError:
            pass

    server = threading.Thread(target=serve)
    server.start()

    client = socket.create_connection(listener.getsockname())
    received = 0
    while received < 256 * 1024:
        chunk = client.recv(65536)
        if not chunk:
            break
        received += len(chunk)

    yield client

    client.close()
    server.join()
    for conn in accepted:
        conn.close()
    listener.close()


@pytest.mark.skipif(not sabctools.tcp_info_available, reason="no TCP statistics on this platform")
def test_reports_a_connected_socket(connection):
    info = sabctools.tcp_info(connection)

    assert info is not None
    assert info["source"] in ("linux", "macos", "windows")


@pytest.mark.skipif(not sabctools.tcp_info_available, reason="no TCP statistics on this platform")
def test_every_field_is_an_int_or_none(connection):
    info = sabctools.tcp_info(connection)

    for field in TCP_FIELDS:
        assert info[field] is None or isinstance(info[field], int), field


@pytest.mark.skipif(not sabctools.tcp_info_available, reason="no TCP statistics on this platform")
def test_counts_the_bytes_that_arrived(connection):
    info = sabctools.tcp_info(connection)

    assert info["bytes_received"] is None or info["bytes_received"] >= 256 * 1024


@pytest.mark.skipif(not sabctools.tcp_info_available, reason="no TCP statistics on this platform")
def test_accepts_a_raw_file_descriptor(connection):
    assert sabctools.tcp_info(connection.fileno()) is not None


def test_closed_socket_reports_nothing(connection):
    connection.close()

    assert sabctools.tcp_info(connection) is None


def test_udp_socket_reports_nothing():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        assert sabctools.tcp_info(udp) is None


@pytest.mark.parametrize("argument", ["not a socket", None, 3.5, object()])
def test_rejects_anything_that_is_not_a_socket(argument):
    with pytest.raises(TypeError):
        sabctools.tcp_info(argument)
