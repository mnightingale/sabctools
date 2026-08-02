from enum import IntEnum
from os import PathLike
from types import TracebackType
from typing import Tuple, Optional, IO, List, Iterator, Union, Type, Sequence, TypedDict, Callable, Dict
from ssl import SSLSocket
from _typeshed import ReadableBuffer, WriteableBuffer

__version__: str
openssl_linked: bool
simd: str
crc_simd: str

def yenc_encode(input_string: bytes) -> Tuple[bytes, int]: ...
def unlocked_ssl_recv_into(ssl_socket: SSLSocket, buffer: WriteableBuffer) -> int: ...
def crc32_combine(crc1: int, crc2: int, length: int) -> int: ...
def crc32_multiply(crc1: int, crc2: int) -> int: ...
def crc32_xpow8n(n: int) -> int: ...
def crc32_xpown(n: int) -> int: ...
def crc32_zero_unpad(crc1: int, length: int) -> int: ...
def sparse(file: Union[IO, int], length: int) -> None:
    """Deprecated in favour of FileWriter.preallocate, kept for existing callers."""

class SparseUnsupported(OSError):
    """The filesystem cannot store the file with holes in it."""

class WriteStats(TypedDict):
    count: int
    bytes: int
    nanos: int
    """Nanoseconds spent inside the write itself"""
    max_nanos: int
    """Nanoseconds the slowest single write took"""

def write_stats(reset: bool = False) -> WriteStats:
    """Totals for every write through every FileWriter.

    Only write() is counted, and closing a file does not subtract what it wrote. With
    reset, each total is taken and zeroed in one step, so consecutive calls carve the
    writes into intervals with none lost or counted twice.
    """

def bytearray_malloc(size: int) -> bytearray: ...
def rarfile_rar3_s2k(pwd, salt) -> tuple[bytes, bytes]: ...

class EncodingFormat(IntEnum):
    YENC = 1
    UU = 2

class Par2Result(IntEnum):
    """Return codes from Par2Repairer's methods, mirroring par2cmdline's own."""

    SUCCESS = 0
    REPAIR_POSSIBLE = 1
    REPAIR_NOT_POSSIBLE = 2
    INVALID_COMMAND_LINE_ARGUMENTS = 3
    INSUFFICIENT_CRITICAL_DATA = 4
    REPAIR_FAILED = 5
    FILE_IO_ERROR = 6
    LOGIC_ERROR = 7
    MEMORY_ERROR = 8

class Par2Error(Exception):
    """Raised when par2 fails in a way it has no return code for."""

class Par2File(TypedDict):
    """One file of a par2 set, as reported by Par2Repairer.files."""

    name: str
    """Name recorded in the par2 set"""
    target: str
    """Path the file should occupy"""
    found: str
    """Where a complete copy actually is, empty if there is not one"""
    exists: bool
    """Whether something occupies the target path.

    Only filled in once verify() has scanned the source files; false for every entry
    straight after load().
    """
    complete: bool
    """Whether a verified-intact copy was found, under any name"""
    blocks: int
    """Number of par2 blocks this file spans"""

class Par2Repairer:
    def __init__(
        self,
        parfile: str,
        extrafiles: Sequence[str] = (),
        basepath: str = "",
        memory_limit: int = 0,
        threads: int = 0,
        file_threads: int = 0,
        skip_data: bool = True,
        skip_leaway: int = 0,
        purge_files: bool = False,
        rename_only: bool = False,
    ) -> None:
        """Verify and repair a par2 set in-process.

        Raises ValueError if par2 rejects the arguments, which includes `parfile`
        not existing. `memory_limit` of 0 lets par2 derive one from physical memory.
        """

    def load(self) -> Par2Result:
        """Read the par2 packets and work out the file set."""

    def load_more(self, parfiles: Sequence[str]) -> int:
        """Add recovery blocks from further par2 files; returns recovery_block_count.

        Does not re-verify. A following repair() reuses the existing verification and
        only re-checks whether there are now enough blocks, which is what makes
        "fetch more blocks and retry" cheap. Requires load() first, and raises
        Par2Error if a named file does not exist.
        """

    def set_known_blocks(self, mapping: Mapping[str, Sequence[object]]) -> None:
        """Take {filename: per-block truth values} as already verified.

        Listed files are neither read nor hashed during verify(); the blocks marked
        true are trusted as-is and the rest are scanned normally, so a partial map is
        fine and an empty one restores the default behaviour. Filenames are as recorded
        in the par2 set, and each sequence runs from block 0.

        Call after load(), which is when block_size becomes known, and before verify().
        """

    def verify(self) -> Par2Result:
        """Scan the source files. Requires load() first."""

    def repair(self) -> Par2Result:
        """Repair the set, reusing an earlier verify() if there was one."""

    def cancel(self) -> None:
        """Ask an in-progress verify() or repair() to stop.

        Safe from another thread, including from progress_callback. The interrupted
        call returns FILE_IO_ERROR, so check `cancelled` to tell a cancellation apart
        from a genuine I/O failure.
        """
    progress_callback: Optional[Callable[[str, str, int], None]]
    """Called as (stage, filename, percent).

    `stage` is one of "loading", "verifying", "repairing", "verifying_repair".
    `filename` is set when a new file is opened and empty on percentage updates;
    `percent` is 0-100 and always 0 during "loading", where par2 does not report a
    usable fraction. Invoked from par2's worker threads, so it must be quick and must
    not call back into the repairer other than cancel(). Exceptions raised here cannot
    unwind through par2 and are reported via sys.unraisablehook.
    """

    file_done_callback: Optional[Callable[[str, int, int], None]]
    """Called as (filename, blocks_found, blocks_total) once per scanned file.

    blocks_found > 0 means that file contributed data to the repair, which is the only
    way to tell that joinable .001/.002 parts were consumed - par2 never reports those
    as source files. Same threading rules as progress_callback.
    """

    missing_block_count: int
    """Blocks that need reconstructing"""
    available_block_count: int
    """Undamaged source blocks found"""
    source_block_count: int
    """Blocks in the complete set"""
    recovery_block_count: int
    """Recovery blocks loaded from the par2 files"""
    recoverable_file_count: int
    """Files the set can recover"""
    complete_file_count: int
    """Files verified intact"""
    damaged_file_count: int
    """Files present but damaged"""
    missing_file_count: int
    """Files not found"""
    renamed_file_count: int
    """Files found under a different name"""
    block_size: int
    """Block size of the set, in bytes"""
    setid: str
    """The par2 set id"""
    repair_possible: bool
    """Whether enough recovery blocks are available to repair"""
    cancelled: bool
    """Whether cancel() was called"""
    quick_verified_files: int
    """How many files verify() took from set_known_blocks() instead of reading"""
    renames: Dict[str, str]
    """{path_on_disk: path_it_will_get} for files matched under another name.

    Only meaningful between verify() and repair(); repair() applies the renames.
    """
    files: List[Par2File]
    """Per-file state, in the order par2 records them"""

class NNTPResponse:
    context: Optional[object]
    """Object handed to Decoder.expect() for the request this answers"""
    status_code: int
    """Code extracted from the first 3 characters of the response"""
    message: Optional[str]
    """The first line of the response"""
    bytes_read: int
    """Bytes consumed, including status line and yEnc headers"""
    bytes_decoded: int
    """Bytes produced"""
    file_name: Optional[str]
    file_size: int
    part_begin: int
    part_end: int
    part_size: int
    end_size: int
    data: Optional[bytearray]
    """Decoded data, or None when it was streamed to a sink instead"""
    crc: Optional[int]
    """CRC of decoded data, None if does not match crc_expected"""
    crc_expected: Optional[int]
    """CRC is yEnc headers, None if not found"""
    lines: Optional[List[str]]
    """NNTP lines from multi-line responses which are not yEnc headers/data e.g. ARTICLE/HEAD/CAPABILITIES"""
    format: Optional[EncodingFormat]
    """Decoding process used"""
    baddata: bool
    """Invalid UU lines were encountered, some data was lost"""
    sink_failed: bool
    """A write to the sink failed, so the decoded body was discarded.

    The response is still completed and the connection is left usable - abandoning it
    mid-stream would desynchronise the byte stream - but nothing was kept, so the
    article has to be fetched again."""
    sink_error: Optional[BaseException]
    """The exception that failed write produced, held rather than raised.

    An OSError for a real disk error, carrying its errno and the file it was writing -
    a full disk arrives as ENOSPC. A ValueError when the file had simply been closed,
    which is what a deleted job looks like. None when no write failed."""

class Decoder:
    def __init__(self, size: int):
        """Initialise a decoder with the given internal buffer size."""

    def __bool__(self) -> bool: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[NNTPResponse]: ...
    def __next__(self) -> NNTPResponse: ...
    def __buffer__(self, __flags: int) -> memoryview: ...
    def __release_buffer__(self, __buffer: memoryview) -> None: ...
    expected: int
    """Requests recorded with expect() whose responses have not been decoded yet"""
    pending: Tuple[object, ...]
    """Contexts of the requests still awaiting a response, oldest first"""

    def expect(self, context: object, sink: Optional["FileWriter"] = None) -> None:
        """Record that a request has been sent, so its response can be paired with it.

        `context` is returned untouched as NNTPResponse.context. Calls must be in the
        order the requests were sent.

        When `sink` is given, the decoded body is written into it at the offset the
        yEnc headers declare rather than collected into a bytearray, and the response's
        `data` is left as None. Bodies larger than the internal staging buffer are
        written in pieces. uu-encoded articles carry no offsets, so a sink is ignored
        for those and `data` is populated as usual.
        """

    def clear_expected(self) -> None:
        """Forget every pending request, for a connection being reset."""

    def process(self, length: int) -> None:
        """Process `length` additional bytes of the internal buffer.

        The decoder maintains an internal buffer that is re-used across calls.
        Incoming data is consumed in fixed-size chunks to avoid repeatedly
        allocating large temporary buffers.

        Callers are expected to feed data from sockets or files incrementally.
        This pattern minimizes copying and wasted allocations while allowing
        streaming decode of multiple NNTP responses.
        """

class FileWriter:
    """A file opened for positional writes.

    Owns its descriptor so nothing outside can close it while a write is in flight,
    and writes at absolute offsets so several threads may write to one file at once
    without a lock. On Windows this uses WriteFile with an OVERLAPPED offset, which
    Python itself has no equivalent for: os.pwrite is Unix only.
    """

    def __init__(self, path: Union[str, bytes, PathLike]) -> None:
        """Open path for writing, creating it if it does not exist."""
    closed: bool
    path: Optional[str]
    size: int
    """Current length of the file, read from the owned handle"""

    def write(self, data: ReadableBuffer, offset: int) -> int:
        """Write all of data at an absolute offset, returning the bytes written.

        Short writes are retried internally, so the return value always equals
        len(data) unless an error was raised.
        """

    def preallocate(self, length: int) -> None:
        """Set the file length, marking it sparse first where the filesystem requires it.

        Raises SparseUnsupported if the filesystem cannot. Windows knows before it acts
        and changes nothing; elsewhere it is only visible afterwards, so the length is
        left set and allocated in full.
        """

    def close(self) -> None:
        """Close the file. Idempotent, and waits for any writes still in flight."""

    def __enter__(self) -> "FileWriter": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc: Optional[BaseException],
        tb: Optional[TracebackType],
    ) -> None: ...
