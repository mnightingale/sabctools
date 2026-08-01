import os
import sys

import pytest

from tests.testsupport import *

WINDOWS = sys.platform == "win32"
windows_only = pytest.mark.skipif(not WINDOWS, reason="sabctools.pwrite is Windows-only")


@pytest.fixture
def fd(tmp_path):
    fd = os.open(tmp_path / "pwrite.bin", os.O_CREAT | os.O_RDWR | getattr(os, "O_BINARY", 0))
    try:
        yield fd
    finally:
        os.close(fd)


def read_at(fd: int, length: int, offset: int) -> bytes:
    """os.pread is POSIX-only, so read via the file pointer and restore it"""
    pos = os.lseek(fd, 0, os.SEEK_CUR)
    try:
        os.lseek(fd, offset, os.SEEK_SET)
        return os.read(fd, length)
    finally:
        os.lseek(fd, pos, os.SEEK_SET)


def read_all(fd: int) -> bytes:
    return read_at(fd, os.fstat(fd).st_size, 0)


@windows_only
def test_pwrite(fd):
    assert sabctools.pwrite(fd, b"Hello World!", 0) == 12
    assert read_all(fd) == b"Hello World!"


@windows_only
@pytest.mark.parametrize("data", [b"bytes", bytearray(b"bytearray"), memoryview(b"memoryview")])
def test_pwrite_buffer_types(fd, data):
    assert sabctools.pwrite(fd, data, 0) == len(data)
    assert read_all(fd)[: len(data)] == bytes(data)


@windows_only
def test_pwrite_offset_creates_hole(fd):
    assert sabctools.pwrite(fd, b"end", 10) == 3
    assert read_all(fd) == b"\0" * 10 + b"end"


@windows_only
def test_pwrite_moves_file_pointer(fd):
    """Unlike os.pwrite, a synchronous handle has its file pointer moved to the end of the write.
    The write position itself is still taken from the offset, never from the pointer."""
    os.lseek(fd, 5, os.SEEK_SET)
    sabctools.pwrite(fd, b"data", 100)
    assert os.lseek(fd, 0, os.SEEK_CUR) == 104


@windows_only
def test_pwrite_ignores_file_pointer(fd):
    """The write must land at the given offset regardless of where the pointer is"""
    sabctools.pwrite(fd, b"x" * 32, 0)
    os.lseek(fd, 20, os.SEEK_SET)
    sabctools.pwrite(fd, b"here", 4)
    assert read_at(fd, 4, 4) == b"here"


@windows_only
def test_pwrite_empty(fd):
    assert sabctools.pwrite(fd, b"", 0) == 0
    assert os.fstat(fd).st_size == 0


@windows_only
def test_pwrite_out_of_order(fd):
    expected = bytearray(37)
    for offset, chunk in ((32, b"third"), (0, b"first"), (16, b"second")):
        assert sabctools.pwrite(fd, chunk, offset) == len(chunk)
        expected[offset : offset + len(chunk)] = chunk
    assert read_all(fd) == bytes(expected)


@windows_only
def test_pwrite_overwrite(fd):
    sabctools.pwrite(fd, b"aaaaaaaa", 0)
    sabctools.pwrite(fd, b"bb", 3)
    assert read_all(fd) == b"aaabbaaa"


@windows_only
def test_pwrite_large_buffer(fd):
    data = os.urandom(4 * 1024 * 1024)
    assert sabctools.pwrite(fd, data, 0) == len(data)
    assert read_all(fd) == data


@windows_only
def test_pwrite_high_offset(fd):
    """The offset is split over Offset/OffsetHigh, so check the high word is used"""
    offset = 5 * 1024 * 1024 * 1024
    sabctools.sparse(fd, offset + 4)
    if os.fstat(fd).st_size != offset + 4:
        pytest.skip("filesystem does not support sparse files")
    assert sabctools.pwrite(fd, b"high", offset) == 4
    assert read_at(fd, 4, offset) == b"high"


@windows_only
def test_pwrite_negative_offset(fd):
    with pytest.raises(OSError) as exc:
        sabctools.pwrite(fd, b"data", -1)
    assert exc.value.errno == 22


@windows_only
def test_pwrite_bad_fd():
    with pytest.raises(OSError):
        sabctools.pwrite(99999, b"data", 0)


@windows_only
def test_pwrite_closed_fd(tmp_path):
    fd = os.open(tmp_path / "closed.bin", os.O_CREAT | os.O_RDWR | os.O_BINARY)
    os.close(fd)
    with pytest.raises(OSError):
        sabctools.pwrite(fd, b"data", 0)


@windows_only
def test_pwrite_read_only_fd(tmp_path):
    path = tmp_path / "readonly.bin"
    path.write_bytes(b"data")
    fd = os.open(path, os.O_RDONLY | os.O_BINARY)
    try:
        with pytest.raises(OSError):
            sabctools.pwrite(fd, b"nope", 0)
    finally:
        os.close(fd)


@windows_only
def test_pwrite_wrong_types(fd):
    with pytest.raises(TypeError):
        sabctools.pwrite(fd, "string", 0)
    with pytest.raises(TypeError):
        sabctools.pwrite(fd, b"data")


@windows_only
def test_pwritev(fd):
    assert sabctools.pwritev(fd, [b"Hello", b" ", b"World!"], 0) == 12
    assert read_all(fd) == b"Hello World!"


@windows_only
def test_pwritev_offset(fd):
    assert sabctools.pwritev(fd, [b"aa", b"bb"], 8) == 4
    assert read_all(fd) == b"\0" * 8 + b"aabb"


@windows_only
def test_pwritev_mixed_buffer_types(fd):
    assert sabctools.pwritev(fd, [b"one", bytearray(b"two"), memoryview(b"three")], 0) == 11
    assert read_all(fd) == b"onetwothree"


@windows_only
def test_pwritev_accepts_tuple_and_iterator(fd):
    assert sabctools.pwritev(fd, (b"a", b"b"), 0) == 2
    assert sabctools.pwritev(fd, iter([b"c", b"d"]), 2) == 2
    assert read_all(fd) == b"abcd"


@windows_only
def test_pwritev_empty(fd):
    assert sabctools.pwritev(fd, [], 0) == 0
    assert sabctools.pwritev(fd, [b"", b""], 0) == 0
    assert os.fstat(fd).st_size == 0


@windows_only
def test_pwritev_skips_empty_buffers(fd):
    assert sabctools.pwritev(fd, [b"a", b"", b"b"], 0) == 2
    assert read_all(fd) == b"ab"


@windows_only
def test_pwritev_matches_sequential_pwrite(fd, tmp_path):
    chunks = [os.urandom(1024) for _ in range(8)]
    assert sabctools.pwritev(fd, chunks, 64) == sum(len(chunk) for chunk in chunks)
    assert read_all(fd) == b"\0" * 64 + b"".join(chunks)


@windows_only
def test_pwritev_moves_file_pointer(fd):
    """As with pwrite, the pointer ends up after the last buffer written"""
    os.lseek(fd, 5, os.SEEK_SET)
    sabctools.pwritev(fd, [b"data", b"more"], 100)
    assert os.lseek(fd, 0, os.SEEK_CUR) == 108


@windows_only
def test_pwritev_negative_offset(fd):
    with pytest.raises(OSError) as exc:
        sabctools.pwritev(fd, [b"data"], -1)
    assert exc.value.errno == 22


@windows_only
def test_pwritev_invalid_entry_writes_nothing(fd):
    """Buffers are acquired up front, so a bad entry must not write anything"""
    with pytest.raises(TypeError):
        sabctools.pwritev(fd, [b"good", "bad"], 0)
    assert os.fstat(fd).st_size == 0


@windows_only
def test_pwritev_not_a_sequence(fd):
    with pytest.raises(TypeError):
        sabctools.pwritev(fd, 42, 0)


@windows_only
def test_pwritev_too_many_buffers(fd):
    with pytest.raises(OSError) as exc:
        sabctools.pwritev(fd, [b"x"] * 1025, 0)
    assert exc.value.errno == 22


@pytest.mark.skipif(WINDOWS, reason="tests the non-Windows fallback")
def test_unsupported_platform():
    with pytest.raises(NotImplementedError):
        sabctools.pwrite(0, b"data", 0)
    with pytest.raises(NotImplementedError):
        sabctools.pwritev(0, [b"data"], 0)
