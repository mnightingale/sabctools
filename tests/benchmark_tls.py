"""CPU comparison of the three TLS read paths this module offers.

    stdlib   ssl.SSLSocket.recv_into           - one TLS record per GIL acquisition
    openssl  sabctools.unlocked_ssl_recv_into  - drains records, hooks CPython's OpenSSL
    awslc    sabctools.TLSSocket.recv_into     - drains records, own statically linked aws-lc

Run it from the repository root:

    python tests/benchmark_tls.py
    python tests/benchmark_tls.py --repeats 5 --connections 1 4 16 32

The server runs in its own process and every client implementation runs in its own
process, so the reported CPU belongs to that implementation alone. Throughput can be
capped by the server, but CPU per GiB cannot, which is why that is the column to
compare. This is loopback, so it runs orders of magnitude faster than a real feed:
it exaggerates syscall and scheduling overhead and hides nothing about the per-byte
cost of the crypto itself, which is the same for both libraries.

This exists mainly to keep one tradeoff honest. aws-lc reads a record header and then
its body unless read-ahead is enabled, and read-ahead needs the flag set on both the
SSL_CTX (which sizes the buffer) and the SSL (which decides whether to fill it). With
it enabled sabctools uses ~21% less CPU per GiB at 16 connections and ~5% more at 4,
so it is on by default. If src/tls.cc ever changes there, this is how to re-check.
"""

import argparse
import json
import os
import resource
import select
import socket
import ssl
import statistics
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import sabctools

# Reuse the certificate the test suite already ships, so this needs no key generation
from tests.test_tls import san_cert, san_key

BUFFER_SIZE = 256 * 1024  # NNTP_BUFFER_SIZE in SABnzbd
CHUNK = b"\xa5" * (64 * 1024)
IMPLEMENTATIONS = ["stdlib", "openssl", "awslc"]

_certificate_dir = None


def certificate_paths() -> tuple:
    """Write the embedded certificate to disk, where the ssl module can load it"""
    global _certificate_dir
    if _certificate_dir is None:
        _certificate_dir = tempfile.mkdtemp(prefix="sabctools_benchmark_")
        with open(os.path.join(_certificate_dir, "cert.pem"), "w") as f:
            f.write(san_cert)
        with open(os.path.join(_certificate_dir, "key.pem"), "w") as f:
            f.write(san_key)
    return os.path.join(_certificate_dir, "cert.pem"), os.path.join(_certificate_dir, "key.pem")


# --------------------------------------------------------------------------- server


def serve(total_bytes: int):
    """Accept connections and blast total_bytes down each one"""
    certificate, key = certificate_paths()
    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.load_cert_chain(certificate, key)

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(128)
    # The driver reads this to learn where to connect
    print(json.dumps({"port": listener.getsockname()[1]}), flush=True)

    def handle(conn):
        try:
            with context.wrap_socket(conn, server_side=True) as tls:
                sent = 0
                while sent < total_bytes:
                    piece = CHUNK[: min(len(CHUNK), total_bytes - sent)]
                    tls.sendall(piece)
                    sent += len(piece)
        except OSError:
            pass

    while True:
        try:
            conn, _ = listener.accept()
        except OSError:
            return
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


# --------------------------------------------------------------------------- client


def connect(implementation: str, port: int):
    certificate, _ = certificate_paths()
    sock = socket.create_connection(("127.0.0.1", port), timeout=30)

    if implementation == "awslc":
        tls = sabctools.TLSContext(ca_certs=san_cert.encode()).wrap_socket(sock, server_hostname="localhost")
    else:
        context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=certificate)
        tls = context.wrap_socket(sock, server_hostname="localhost")

    tls.setblocking(False)
    return tls


def drain(tls, implementation: str, total_bytes: int, buffer: memoryview) -> tuple:
    """Read total_bytes, counting the read calls and the waits in between"""
    received = calls = waits = 0
    fileno = [tls.fileno()]

    while received < total_bytes:
        try:
            if implementation == "openssl":
                count = sabctools.unlocked_ssl_recv_into(tls, buffer)
            else:
                count = tls.recv_into(buffer)
        except ssl.SSLWantReadError:
            select.select(fileno, [], [], 5)
            waits += 1
            continue
        if count == 0:
            break
        received += count
        calls += 1

    return received, calls, waits


def run_client(implementation: str, port: int, connections: int, total_bytes: int) -> dict:
    # Handshake outside the measured window, this measures the bulk transfer
    sockets = [connect(implementation, port) for _ in range(connections)]
    negotiated = (sockets[0].version(), sockets[0].cipher()[0])
    results = [None] * connections

    def worker(index):
        buffer = memoryview(sabctools.bytearray_malloc(BUFFER_SIZE))
        results[index] = drain(sockets[index], implementation, total_bytes, buffer)

    before = resource.getrusage(resource.RUSAGE_SELF)
    start = time.perf_counter()

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(connections)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    elapsed = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_SELF)

    for tls in sockets:
        tls.close()

    received = sum(result[0] for result in results)
    calls = sum(result[1] for result in results)
    user = after.ru_utime - before.ru_utime
    system = after.ru_stime - before.ru_stime
    gib = received / 1024**3

    return {
        "gib": gib,
        "mib_per_second": received / 1024**2 / elapsed,
        "cpu_per_gib": (user + system) / gib,
        "system_per_gib": system / gib,
        "kib_per_call": received / calls / 1024 if calls else 0,
        "selects": sum(result[2] for result in results),
        "version": negotiated[0],
        "cipher": negotiated[1],
    }


def run_handshakes(implementation: str, port: int, count: int) -> dict:
    """Handshakes are pure CPU too, and a downloader opens a lot of connections"""
    before = resource.getrusage(resource.RUSAGE_SELF)
    start = time.perf_counter()
    for _ in range(count):
        connect(implementation, port).close()
    elapsed = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_SELF)

    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    return {"per_second": count / elapsed, "cpu_ms_each": cpu / count * 1000}


# --------------------------------------------------------------------------- driver


def start_server(total_bytes: int) -> tuple:
    server = subprocess.Popen(
        [sys.executable, __file__, "--role", "server", "--bytes", str(total_bytes)],
        stdout=subprocess.PIPE,
        text=True,
    )
    return server, json.loads(server.stdout.readline())["port"]


def measure(role: str, implementation: str, total_bytes: int, **arguments) -> dict:
    """One measurement, in a subprocess of its own so the CPU accounting is clean"""
    server, port = start_server(total_bytes)
    try:
        command = [sys.executable, __file__, "--role", role, "--implementation", implementation, "--port", str(port)]
        command += ["--bytes", str(total_bytes)]
        for name, value in arguments.items():
            command += ["--" + name, str(value)]
        completed = subprocess.run(command, capture_output=True, text=True)
        if completed.returncode != 0:
            sys.stderr.write(completed.stderr)
            raise SystemExit(f"{implementation} {role} failed")
        return json.loads(completed.stdout)
    finally:
        server.kill()
        server.wait()


def drive(connection_counts: list, repeats: int, total_bytes: int):
    implementations = [i for i in IMPLEMENTATIONS if i != "awslc" or sabctools.aws_lc_linked]

    print(f"python      {sys.version.split()[0]}")
    print(f"OpenSSL     {ssl.OPENSSL_VERSION}")
    print(f"aws-lc      {sabctools.aws_lc_version or 'not built in'}")
    print(f"payload     {total_bytes / 1024**2:.0f} MiB per connection, {repeats} repeats, median reported")

    baselines = {}
    for connections in connection_counts:
        print(f"\n{connections} connection(s)")
        print("-" * 88)
        print(
            f"{'impl':<9}{'CPU s/GiB':>11}{'min':>8}{'max':>8}{'sys s/GiB':>11}{'MiB/s':>9}{'KiB/read':>10}{'vs stdlib':>11}"
        )
        for implementation in implementations:
            runs = [measure("client", implementation, total_bytes, connections=connections) for _ in range(repeats)]
            cpu = sorted(run["cpu_per_gib"] for run in runs)
            median = statistics.median(cpu)
            if implementation == "stdlib":
                baselines[connections] = median
            relative = median / baselines.get(connections, median)
            print(
                f"{implementation:<9}{median:>11.3f}{cpu[0]:>8.3f}{cpu[-1]:>8.3f}"
                f"{statistics.median(r['system_per_gib'] for r in runs):>11.3f}"
                f"{statistics.median(r['mib_per_second'] for r in runs):>9.0f}"
                f"{statistics.median(r['kib_per_call'] for r in runs):>10.1f}"
                f"{relative:>11.2f}"
            )

    print("\nHandshakes (the openssl path shares the stdlib handshake, so it is not repeated)")
    print("-" * 88)
    print(f"{'impl':<9}{'per second':>13}{'CPU ms each':>14}")
    for implementation in [i for i in implementations if i != "openssl"]:
        result = measure("handshake", implementation, 1024, count=200)
        print(f"{implementation:<9}{result['per_second']:>13.0f}{result['cpu_ms_each']:>14.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--role", choices=["driver", "server", "client", "handshake"], default="driver")
    parser.add_argument("--implementation", choices=IMPLEMENTATIONS)
    parser.add_argument("--port", type=int)
    parser.add_argument("--connections", type=int, nargs="+", default=[1, 4, 16, 32])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--bytes", type=int, default=256 * 1024 * 1024)
    parser.add_argument("--count", type=int, default=200)
    arguments = parser.parse_args()

    if arguments.role == "server":
        serve(arguments.bytes)
    elif arguments.role == "client":
        print(
            json.dumps(run_client(arguments.implementation, arguments.port, arguments.connections[0], arguments.bytes))
        )
    elif arguments.role == "handshake":
        print(json.dumps(run_handshakes(arguments.implementation, arguments.port, arguments.count)))
    else:
        drive(arguments.connections, arguments.repeats, arguments.bytes)


if __name__ == "__main__":
    main()
