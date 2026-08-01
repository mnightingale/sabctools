#!/usr/bin/python3
"""Compare the native sabctools.pwrite/pwritev against the pure-Python approaches.

Run directly, it is not collected by pytest:

    python tests/benchmark_pwrite.py
    python tests/benchmark_pwrite.py --sizes 4096,786432 --iterations 20000

On Windows this compares the ctypes WriteFile/OVERLAPPED wrapper used by SABnzbd
against the native extension. On other platforms sabctools.pwrite is unavailable,
so only os.pwrite is measured, as a syscall-cost reference.
"""

import argparse
import os
import sys
import tempfile
import time
from typing import Callable, Optional

import sabctools

WINDOWS = sys.platform == "win32"

if WINDOWS:
    import ctypes
    import msvcrt
    from ctypes import wintypes

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [
            ("Internal", ctypes.c_void_p),
            ("InternalHigh", ctypes.c_void_p),
            ("Offset", wintypes.DWORD),
            ("OffsetHigh", wintypes.DWORD),
            ("hEvent", wintypes.HANDLE),
        ]

    _WriteFile = ctypes.windll.kernel32.WriteFile
    _WriteFile.argtypes = (
        wintypes.HANDLE,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(OVERLAPPED),
    )
    _WriteFile.restype = wintypes.BOOL

    def ctypes_pwrite(fd: int, data: bytearray, offset: int) -> int:
        """The implementation currently used by SABnzbd's assembler"""
        overlapped = OVERLAPPED()
        overlapped.Offset = offset & 0xFFFFFFFF
        overlapped.OffsetHigh = offset >> 32
        written = wintypes.DWORD(0)
        length = len(data)
        buffer = (ctypes.c_char * length).from_buffer(data)
        if not _WriteFile(
            msvcrt.get_osfhandle(fd), ctypes.byref(buffer), length, ctypes.byref(written), ctypes.byref(overlapped)
        ):
            raise ctypes.WinError()
        return written.value

    def ctypes_pwrite_cached_handle(fd: int, data: bytearray, offset: int, handle) -> int:
        """As above but with the handle lookup hoisted out, isolating the ctypes call cost"""
        overlapped = OVERLAPPED()
        overlapped.Offset = offset & 0xFFFFFFFF
        overlapped.OffsetHigh = offset >> 32
        written = wintypes.DWORD(0)
        length = len(data)
        buffer = (ctypes.c_char * length).from_buffer(data)
        if not _WriteFile(handle, ctypes.byref(buffer), length, ctypes.byref(written), ctypes.byref(overlapped)):
            raise ctypes.WinError()
        return written.value


def time_call(call: Callable[[int], None], iterations: int, repeats: int) -> float:
    """Return the best nanoseconds-per-call over `repeats` runs of `iterations` calls"""
    best = float("inf")
    for _ in range(repeats):
        start = time.perf_counter_ns()
        for i in range(iterations):
            call(i)
        elapsed = time.perf_counter_ns() - start
        best = min(best, elapsed / iterations)
    return best


def format_row(name: str, ns: float, baseline: Optional[float], size: int) -> str:
    throughput = size / (ns / 1_000_000_000) / (1024 * 1024)
    relative = "baseline" if baseline is None else "%.2fx" % (baseline / ns)
    return "  %-34s %10.0f ns %12.0f MB/s   %s" % (name, ns, throughput, relative)


def run_size(fd: int, size: int, iterations: int, repeats: int, span: int) -> None:
    data = bytearray(os.urandom(size))

    # Cycle over a window of offsets so the same page is not written every time,
    # while staying small enough that the file stays in the page cache
    offsets = [(i % span) * size for i in range(iterations)]

    print("\n%s per call, %d iterations x %d repeats" % (human(size), iterations, repeats))

    baseline = None
    if WINDOWS:
        handle = msvcrt.get_osfhandle(fd)
        baseline = time_call(lambda i: ctypes_pwrite(fd, data, offsets[i]), iterations, repeats)
        print(format_row("ctypes WriteFile", baseline, None, size))

        cached = time_call(lambda i: ctypes_pwrite_cached_handle(fd, data, offsets[i], handle), iterations, repeats)
        print(format_row("ctypes WriteFile (cached handle)", cached, baseline, size))

        native = time_call(lambda i: sabctools.pwrite(fd, data, offsets[i]), iterations, repeats)
        print(format_row("sabctools.pwrite", native, baseline, size))
    else:
        native = time_call(lambda i: os.pwrite(fd, data, offsets[i]), iterations, repeats)
        print(format_row("os.pwrite", native, None, size))
        print("  sabctools.pwrite is Windows-only, nothing to compare against here")


def run_vectored(fd: int, size: int, count: int, iterations: int, repeats: int) -> None:
    buffers = [bytearray(os.urandom(size)) for _ in range(count)]
    total = size * count

    print("\n%d x %s vectored, %d iterations x %d repeats" % (count, human(size), iterations, repeats))

    if WINDOWS:
        def loop_pwrite(_i: int) -> None:
            offset = 0
            for buffer in buffers:
                offset += sabctools.pwrite(fd, buffer, offset)

        baseline = time_call(loop_pwrite, iterations, repeats)
        print(format_row("loop of sabctools.pwrite", baseline, None, total))

        native = time_call(lambda i: sabctools.pwritev(fd, buffers, 0), iterations, repeats)
        print(format_row("sabctools.pwritev", native, baseline, total))
    else:
        def loop_pwrite(_i: int) -> None:
            offset = 0
            for buffer in buffers:
                offset += os.pwrite(fd, buffer, offset)

        baseline = time_call(loop_pwrite, iterations, repeats)
        print(format_row("loop of os.pwrite", baseline, None, total))

        if hasattr(os, "pwritev"):
            native = time_call(lambda i: os.pwritev(fd, buffers, 0), iterations, repeats)
            print(format_row("os.pwritev", native, baseline, total))


def human(size: int) -> str:
    for unit in ("B", "KB", "MB"):
        if size < 1024 or unit == "MB":
            return "%g %s" % (size, unit)
        size /= 1024


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sizes", default="1024,65536,786432", help="comma separated write sizes in bytes")
    parser.add_argument("--iterations", type=int, default=5000, help="calls per timed run")
    parser.add_argument("--repeats", type=int, default=5, help="timed runs, the best is reported")
    parser.add_argument("--span", type=int, default=64, help="number of distinct offsets to cycle over")
    parser.add_argument("--vectored", type=int, default=8, help="buffers per pwritev call")
    parser.add_argument("--keep", action="store_true", help="do not delete the scratch file")
    args = parser.parse_args()

    sizes = [int(size) for size in args.sizes.split(",")]

    print("python %s on %s, sabctools %s" % (sys.version.split()[0], sys.platform, sabctools.version))

    path = os.path.join(tempfile.gettempdir(), "sabctools_pwrite_benchmark.bin")
    fd = os.open(path, os.O_CREAT | os.O_RDWR | getattr(os, "O_BINARY", 0))
    try:
        # Pre-size the file so the writes never extend it, which would dominate the timings
        sabctools.sparse(fd, max(sizes) * max(args.span, args.vectored))
        for size in sizes:
            run_size(fd, size, args.iterations, args.repeats, args.span)
        run_vectored(fd, sizes[0], args.vectored, args.iterations, args.repeats)
    finally:
        os.close(fd)
        if not args.keep:
            os.unlink(path)


if __name__ == "__main__":
    main()
