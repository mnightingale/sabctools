import socket
from enum import IntEnum
from typing import Tuple, Optional, IO, List, Iterator, Union
from ssl import SSLSocket
from _typeshed import ReadableBuffer, WriteableBuffer

__version__: str
openssl_linked: bool
aws_lc_linked: bool
aws_lc_version: Optional[str]
simd: str

def yenc_encode(input_string: bytes) -> Tuple[bytes, int]: ...
def unlocked_ssl_recv_into(ssl_socket: SSLSocket, buffer: WriteableBuffer) -> int: ...
def collect_ca_certs() -> bytes: ...
def crc32_combine(crc1: int, crc2: int, length: int) -> int: ...
def crc32_multiply(crc1: int, crc2: int) -> int: ...
def crc32_xpow8n(n: int) -> int: ...
def crc32_xpown(n: int) -> int: ...
def crc32_zero_unpad(crc1: int, length: int) -> int: ...
def sparse(file: Union[IO, int], length: int) -> None: ...
def bytearray_malloc(size: int) -> bytearray: ...
def rarfile_rar3_s2k(pwd, salt) -> tuple[bytes, bytes]: ...

class TLSSocket:
    """A TLS connection layered on an already connected socket.

    Implements the subset of ssl.SSLSocket that the download path uses, but reads
    and writes as many TLS records per call as it can without holding the GIL.
    """

    socket: socket.socket
    context: "TLSContext"

    def recv_into(self, buffer: WriteableBuffer, nbytes: int = 0) -> int: ...
    def recv(self, bufsize: int) -> bytes: ...
    def send(self, data: ReadableBuffer) -> int: ...
    def sendall(self, data: ReadableBuffer) -> None: ...
    def pending(self) -> int: ...
    def fileno(self) -> int: ...
    def setblocking(self, flag: bool) -> None: ...
    def settimeout(self, value: Optional[float]) -> None: ...
    def gettimeout(self) -> Optional[float]: ...
    def getblocking(self) -> bool: ...
    def session_reused(self) -> bool: ...
    def version(self) -> Optional[str]: ...
    def cipher(self) -> Optional[Tuple[str, str, int]]: ...
    def getpeercert(self, binary_form: bool = False) -> Optional[bytes]: ...
    def close(self) -> None: ...

class TLSContext:
    def __init__(
        self,
        ca_certs: bytes = b"",
        verify_mode: int = 1,
        check_hostname: bool = True,
        ciphers: Optional[str] = None,
        minimum_version: int = 0,
        maximum_version: int = 0,
        session_cache: bool = True,
    ):
        """Configure a client TLS context.

        `ca_certs` is a concatenated PEM blob, see collect_ca_certs().
        `minimum_version`/`maximum_version` take ssl.TLSVersion values, where any
        value <= 0 means "whatever the library supports".
        """
    has_session: bool
    """Whether a session is cached for resumption"""

    def wrap_socket(self, sock: socket.socket, server_hostname: Optional[str]) -> TLSSocket: ...

class EncodingFormat(IntEnum):
    YENC = 1
    UU = 2

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
