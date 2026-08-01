import select
import socket
import ssl
import sys
import time

import pytest

from tests.testsupport import *
from tests.test_unlocked_ssl import HOST, EchoServer

pytestmark = pytest.mark.skipif(not sabctools.aws_lc_linked, reason="built without aws-lc")

# Self-signed, valid until 2126, with subjectAltName DNS:localhost and IP:127.0.0.1 so
# that hostname verification can actually be exercised against the echo server.
san_cert = """-----BEGIN CERTIFICATE-----
MIIDuTCCAqGgAwIBAgIUEwWFdQdxBYxkGK0ad5p6T9RTvc8wDQYJKoZIhvcNAQEL
BQAwXTELMAkGA1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxJTAjBgNVBAoM
HFNBQm56YmQgc2FiY3Rvb2xzIHRlc3Qgc3VpdGUxEjAQBgNVBAMMCWxvY2FsaG9z
dDAgFw0yNjA4MDExNjAxMzFaGA8yMTI2MDcwODE2MDEzMVowXTELMAkGA1UEBhMC
QVUxEzARBgNVBAgMClNvbWUtU3RhdGUxJTAjBgNVBAoMHFNBQm56YmQgc2FiY3Rv
b2xzIHRlc3Qgc3VpdGUxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAKZesdnLouZoqcSSB4S9xiBtzanxTHntyAPhO+oQ
cqKH2v0RXpr2xNJzTRgKeiF+IlieTFx21SEecSv7gcCzGuVqT1gsGwMC1s8IIDv4
4thZYULQYQyBH5ROOptRahpQw4E60jsl79s66pQhQVk75Q6un8FdR8pUUG1uja0p
+s88wks25jRJDZmhvOx/Nfp+tkMEMth3P/jIppviek3vafznPKv9AiKzd0hDdjOK
wkxDxjY632aLPiv5iWTi7MkCCWbWOp7OaiKFTz26j9x3dLvQUYakx7qVsIhbWt4i
GpSByNnPw1dWKl+dKvkrs34IeYS9wF2gUIO9E/SBPPKZiKcCAwEAAaNvMG0wHQYD
VR0OBBYEFDTArVV0faq8quHWmtb5iHz6bX60MB8GA1UdIwQYMBaAFDTArVV0faq8
quHWmtb5iHz6bX60MA8GA1UdEwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxo
b3N0hwR/AAABMA0GCSqGSIb3DQEBCwUAA4IBAQA5126qkqMT64+DWdxgLILRYi5D
cWmHsS6OKxeFxTJ42CFB7s2vyh/Pz0+QL4qVDYK6APw68wIhGW7u8dRP82GCF43l
DrqlXYeWOoJR6Cs6h/ln/3mBb2u5Y+2wDPt3bqmEAhcWOmwsIEQY81QbBrbtud0C
ZvSwPH9ayO20fNI45024pmiaHDJw1ro3CubSzQvRll6OOBoz3E8KR76EpuZuqKbL
8X7uURczmJif6YxTk9fWpIj/Ag81qOTdRBASZzt964hNfC406kUHq7NNMpbYHbcH
RE9EXPvAt6qyppUNG7psb80Mm7PItbLpJTk0lRTCCVb61ql7aiz4j44NvYr+
-----END CERTIFICATE-----
"""

san_key = """-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCmXrHZy6LmaKnE
kgeEvcYgbc2p8Ux57cgD4TvqEHKih9r9EV6a9sTSc00YCnohfiJYnkxcdtUhHnEr
+4HAsxrlak9YLBsDAtbPCCA7+OLYWWFC0GEMgR+UTjqbUWoaUMOBOtI7Je/bOuqU
IUFZO+UOrp/BXUfKVFBtbo2tKfrPPMJLNuY0SQ2ZobzsfzX6frZDBDLYdz/4yKab
4npN72n85zyr/QIis3dIQ3YzisJMQ8Y2Ot9miz4r+Ylk4uzJAglm1jqezmoihU89
uo/cd3S70FGGpMe6lbCIW1reIhqUgcjZz8NXVipfnSr5K7N+CHmEvcBdoFCDvRP0
gTzymYinAgMBAAECggEALFb/in7Rzxuk511OAKw9UiZBkmHbkoFzdclBGRxCGRwh
GqJW9vD+uuH88YPVUfjWeYzS6C/JlLaSzfiNd8ikSfFf7S1wE4jdrDbLtIAcITIN
EGwN+XGuc224A+4aW6IbwOTm49m0B9c3brxAOOKUJSoYLMZKHFwFRW0Z+EVAcZue
++EqgSj+n2+wh75DGEIO75mr0ON3GljXryHn3fx5atyvHIldpCudmXo6djlR8SZ+
kxOZGW5U/gSIhljLNevuPQXmvF5tJfm7vtM+1qCZjK/rgD+lB1YpVvfrHrQjuSm3
cFCg04oXISlqsSgUrxIPLUIM/JRL41uxTphMk5AwKQKBgQDgqQcZBt2BxxyM9f5l
8EMurA3UZVR93Hhj11mm2/c28QYgVfyR3FNGO4dUrOLSeEverX0SGMNK6jvT0pY9
ZVgZqSHpKPKFyFZSTaXLcBl2Y3qbm7fPI71qj/K7bgac70GHwawQMv3zRUlfPJGR
Ml4V+apYEVRVnkAFRk/2Ex+wnwKBgQC9lAa2m9fY4W3GHRydcArlZ4oOxKMdIYoW
jI1a2D7tJr5S1VieGSvCyVxjLlpfG9/uIstx2JZw9intBO6D84Q8v2UpSjlKhmtC
m3QxtlrPee2hkmIB6x/+MechVJuVXsLFIql0P3fIfE5jwvOk8ypQyvQRZaBq2Uzi
y1XsWSqC+QKBgQDQsxTd4evKEH7sT+UJK54tcDXUtmE8HqBUF4y4HiVUi6jmRxq/
IU7WspwwQ/7eCFRqwv2p3wkkwd3cFAfvdwLVq7HN2HUbZJUUFf/LshJlUVpnzct+
CLSAlsKl7TsFdJmKlJbT4ZrZ7+aOK0UK/iA7B9h/wXF3q+/LNps3fGJ2/wKBgDOz
d/U7IS2LpRVfgRtKoB4aE6OdauKZ7//gSviYeujQJR2QA3/yW2Xe5mxCCvFfN73J
DXS53aVm7N4v9yBTPCAZDmypSmCRshTTMmgQVEm69dyXgFUHm40GbQNBAMFGu5Vp
s475dCBgDjzUwP+eNU8dWlyYO5yIMJi1XXR8iMR5AoGAS7jfmuaCIYlYlEz1cWiS
6dvPEPsTU3L5wuE6VTr7jxBeU78wxE67nKLKBUfwjppMoLqNjkhxZStTjH8C93xh
Q14owYB24as1smytU/jqC4jiQDkcxOR7RC73uSLVLRA2Un9emOgk+0yr2//I2m6s
ep1Hv8UVFa7OplrV2qul8S8=
-----END PRIVATE KEY-----
"""


@pytest.fixture()
def server():
    server = EchoServer(certificate=san_cert, private_key=san_key)
    with server:
        yield server


@pytest.fixture()
def context():
    """Verifies the echo server's certificate against itself as the only trusted root"""
    yield sabctools.TLSContext(ca_certs=san_cert.encode())


def connect(server, context, hostname="localhost", blocking=False, timeout=10.0):
    """Connect to the echo server and hand the socket to the TLS layer"""
    sock = socket.create_connection((server.host, server.port), timeout=timeout)
    tls = context.wrap_socket(sock, server_hostname=hostname)
    if not blocking:
        tls.setblocking(False)
    return tls


@pytest.fixture()
def client(server, context):
    tls = connect(server, context)
    yield tls
    tls.close()


@pytest.fixture()
def buffer():
    yield memoryview(bytearray(1024))


def echo(tls, data: bytes, into: memoryview) -> int:
    """Send data and spin until the echoed response arrives"""
    tls.sendall(data)
    received = 0
    deadline = time.time() + 10
    while received == 0 and time.time() < deadline:
        try:
            received += tls.recv_into(into)
        except ssl.SSLWantReadError:
            select.select([tls.fileno()], [], [], 1)
    return received


def test_aws_lc_linked():
    assert sabctools.aws_lc_linked is True
    assert sabctools.aws_lc_version.startswith("AWS-LC")


def test_handshake_and_verification(client):
    assert client.version().startswith("TLS")
    assert client.cipher()[0]
    assert client.getpeercert(binary_form=True)


def test_hostname_mismatch_is_rejected(server, context):
    with pytest.raises(ssl.SSLCertVerificationError):
        connect(server, context, hostname="not-the-right-host")


def test_untrusted_certificate_is_rejected(server):
    # An unrelated root, so the server's certificate cannot chain to anything trusted
    from tests.test_unlocked_ssl import cert as unrelated_cert

    context = sabctools.TLSContext(ca_certs=unrelated_cert.encode())
    with pytest.raises(ssl.SSLCertVerificationError):
        connect(server, context)


def test_verification_can_be_disabled(server):
    context = sabctools.TLSContext(verify_mode=0, check_hostname=False)
    tls = connect(server, context, hostname=None)
    assert tls.version().startswith("TLS")
    tls.close()


def test_verify_mode_requires_ca_certs():
    with pytest.raises(ssl.SSLError, match="ca_certs is required"):
        sabctools.TLSContext()


def test_check_hostname_requires_a_hostname(server):
    context = sabctools.TLSContext(ca_certs=san_cert.encode())
    sock = socket.create_connection((server.host, server.port), timeout=10)
    with pytest.raises(ValueError, match="server_hostname is required"):
        context.wrap_socket(sock, server_hostname=None)
    sock.close()


def test_echo(client, buffer):
    received = echo(client, b"TEST", buffer)
    assert buffer[:received].tobytes() == b"TEST"


def test_bulk_response_exceeds_one_tls_record(client):
    # 131072 bytes divide up into 8 TLS records (16 KB each). A single call has to
    # be able to drain more than one of them, that is the whole point of this module.
    size = 131072
    buffer = bytearray(size)

    client.sendall(b"\xff" * size)
    select.select([client.fileno()], [], [])

    remaining = size
    while remaining > 0:
        try:
            count = client.recv_into(buffer)
        except ssl.SSLWantReadError:
            select.select([client.fileno()], [], [])
            # Give the sender some more time to complete sending
            time.sleep(0.1)
        else:
            if count > 16384:
                return
            remaining -= count

    pytest.fail("All TLS reads were smaller than 16KB")


def test_recv_returns_bytes(server, context):
    tls = connect(server, context, blocking=True)
    tls.sendall(b"TEST")
    assert tls.recv(1024) == b"TEST"
    tls.close()


def test_recv_zero_bytes(client):
    assert client.recv(0) == b""


def test_recv_rejects_a_negative_size(client):
    with pytest.raises(ValueError, match="negative buffersize"):
        client.recv(-1)


def test_recv_into_honours_nbytes(client, buffer):
    client.sendall(b"0123456789")
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            count = client.recv_into(buffer, nbytes=4)
        except ssl.SSLWantReadError:
            select.select([client.fileno()], [], [], 1)
        else:
            assert count == 4
            assert buffer[:4].tobytes() == b"0123"
            return
    pytest.fail("No data received")


def test_recv_into_rejects_nbytes_larger_than_the_buffer(client, buffer):
    with pytest.raises(ValueError, match="nbytes is greater than the length of the buffer"):
        client.recv_into(buffer, nbytes=len(buffer) + 1)


def test_recv_into_rejects_a_full_buffer(client, buffer):
    with pytest.raises(ValueError, match="No space left in buffer"):
        client.recv_into(buffer[len(buffer) :])


def test_recv_into_rejects_a_read_only_buffer(client):
    with pytest.raises(TypeError):
        client.recv_into(b"read only")


def test_recv_into_on_an_idle_socket_wants_read(client, buffer):
    with pytest.raises(ssl.SSLWantReadError):
        client.recv_into(buffer)


def test_send_returns_the_number_of_bytes_written(client):
    assert client.send(b"hello") == 5


def test_send_accepts_a_memoryview(client, buffer):
    payload = memoryview(b"from a memoryview")
    assert client.send(payload) == len(payload)
    assert echo_result(client, buffer, len(payload)) == bytes(payload)


def echo_result(tls, into: memoryview, expected: int) -> bytes:
    """Collect exactly `expected` bytes of echoed data"""
    received = 0
    deadline = time.time() + 10
    while received < expected and time.time() < deadline:
        try:
            received += tls.recv_into(into[received:])
        except ssl.SSLWantReadError:
            select.select([tls.fileno()], [], [], 1)
    return into[:received].tobytes()


def test_sendall_large_payload(server, context):
    """sendall has to keep going past a short write, which a 1 MB payload forces"""
    tls = connect(server, context, blocking=True)
    payload = b"x" * (1024 * 1024)
    tls.sendall(payload)

    received = 0
    while received < len(payload):
        chunk = bytearray(65536)
        count = tls.recv_into(chunk)
        assert count > 0
        received += count
    assert received == len(payload)
    tls.close()


def test_blocking_read(server, context, buffer):
    tls = connect(server, context, blocking=True)
    tls.sendall(b"blocking")
    assert tls.recv_into(buffer) == 8
    assert buffer[:8].tobytes() == b"blocking"
    tls.close()


def test_read_times_out(server, context, buffer):
    tls = connect(server, context, timeout=1.0)
    tls.settimeout(0.2)
    with pytest.raises(TimeoutError):
        tls.recv_into(buffer)
    tls.close()


def test_clean_shutdown_returns_zero(server, context, buffer):
    tls = connect(server, context, blocking=True)
    # The echo server unwraps and closes the connection on a whitespace-only line
    tls.sendall(b"\n")
    assert tls.recv_into(buffer) == 0
    tls.close()


def test_operations_on_a_closed_socket_fail(client, buffer):
    client.close()
    with pytest.raises(ValueError, match="closed TLSSocket"):
        client.recv_into(buffer)
    with pytest.raises(ValueError, match="closed TLSSocket"):
        client.send(b"nope")
    # These stay callable so that teardown paths do not need to special-case them
    assert client.pending() == 0
    assert client.version() is None


def test_pending_reports_buffered_plaintext(server, context):
    tls = connect(server, context, blocking=True)
    tls.sendall(b"y" * 65536)

    small = bytearray(1024)
    tls.recv_into(small)
    # A whole TLS record was decrypted, so the remainder is buffered inside aws-lc
    # and select() will never report it
    assert tls.pending() > 0
    tls.close()


def test_timeout_accessors(client):
    assert client.getblocking() is False
    assert client.gettimeout() == 0.0

    client.settimeout(5.0)
    assert client.gettimeout() == 5.0
    assert client.getblocking() is True

    client.settimeout(None)
    assert client.gettimeout() is None

    client.setblocking(False)
    assert client.gettimeout() == 0.0


def test_socket_and_context_are_exposed(client, context):
    assert client.context is context
    assert client.fileno() == client.socket.fileno()


def test_getpeercert_dict_form_is_not_supported(client):
    with pytest.raises(NotImplementedError):
        client.getpeercert()


def test_bad_cipher_list_is_reported():
    with pytest.raises(ssl.SSLError, match="could not set ciphers"):
        sabctools.TLSContext(ca_certs=san_cert.encode(), ciphers="THIS-IS-NOT-A-CIPHER")


def test_protocol_version_bounds(server):
    context = sabctools.TLSContext(
        ca_certs=san_cert.encode(),
        minimum_version=ssl.TLSVersion.TLSv1_2,
        maximum_version=ssl.TLSVersion.TLSv1_2,
    )
    tls = connect(server, context)
    assert tls.version() == "TLSv1.2"
    tls.close()


def test_session_is_cached_and_resumed(server, context):
    assert context.has_session is False

    first = connect(server, context, blocking=True)
    assert first.session_reused() is False
    # TLS 1.3 sends the ticket after the handshake, so read once to pick it up
    first.sendall(b"TEST")
    first.recv_into(bytearray(64))
    first.close()
    assert context.has_session is True

    second = connect(server, context, blocking=True)
    assert second.session_reused() is True
    second.close()


def test_session_cache_can_be_disabled(server):
    context = sabctools.TLSContext(ca_certs=san_cert.encode(), session_cache=False)
    tls = connect(server, context, blocking=True)
    tls.sendall(b"TEST")
    tls.recv_into(bytearray(64))
    tls.close()
    assert context.has_session is False


def test_ref_counts_unchanged(client, buffer):
    objects = [client, client.socket, client.context, buffer]
    before = [sys.getrefcount(x) for x in objects]

    received = echo(client, b"Hello World", buffer)
    assert buffer[:received].tobytes() == b"Hello World"

    after = [sys.getrefcount(x) for x in objects]
    assert after == before


def test_collect_ca_certs_returns_pem():
    ca_certs = sabctools.collect_ca_certs()
    assert b"-----BEGIN CERTIFICATE-----" in ca_certs
    # Has to be usable as a trust store
    assert sabctools.TLSContext(ca_certs=ca_certs)
