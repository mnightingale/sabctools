from enum import IntEnum
from typing import Callable, Dict, Mapping, Tuple, Optional, IO, List, Iterator, Sequence, TypedDict, Union
from ssl import SSLSocket
from _typeshed import WriteableBuffer

__version__: str
openssl_linked: bool
simd: str

def yenc_encode(input_string: bytes) -> Tuple[bytes, int]: ...
def unlocked_ssl_recv_into(ssl_socket: SSLSocket, buffer: WriteableBuffer) -> int: ...
def crc32_combine(crc1: int, crc2: int, length: int) -> int: ...
def crc32_multiply(crc1: int, crc2: int) -> int: ...
def crc32_xpow8n(n: int) -> int: ...
def crc32_xpown(n: int) -> int: ...
def crc32_zero_unpad(crc1: int, length: int) -> int: ...
def sparse(file: Union[IO, int], length: int) -> None: ...
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
    renames: Dict[str, str]
    """{path_on_disk: path_it_will_get} for files matched under another name.

    Only meaningful between verify() and repair(); repair() applies the renames.
    """
    files: List[Par2File]
    """Per-file state, in the order par2 records them"""

class NNTPResponse:
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
    """Decoded data"""
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

class Decoder:
    def __init__(self, size: int):
        """Initialise a decoder with the given internal buffer size."""

    def __bool__(self) -> bool: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[NNTPResponse]: ...
    def __next__(self) -> NNTPResponse: ...
    def __buffer__(self, __flags: int) -> memoryview: ...
    def __release_buffer__(self, __buffer: memoryview) -> None: ...
    def process(self, length: int) -> None:
        """Process `length` additional bytes of the internal buffer.

        The decoder maintains an internal buffer that is re-used across calls.
        Incoming data is consumed in fixed-size chunks to avoid repeatedly
        allocating large temporary buffers.

        Callers are expected to feed data from sockets or files incrementally.
        This pattern minimizes copying and wasted allocations while allowing
        streaming decode of multiple NNTP responses.
        """
