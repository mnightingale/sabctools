# C-extension is placed as submodule to allow typing
from ssl import SSLError, SSL_ERROR_EOF, SSLSocket

from sabctools.sabctools import *
from sabctools.sabctools import unlocked_ssl_recv_into as _unlocked_ssl_recv_into

__version__ = version


def unlocked_ssl_recv_into(sock: SSLSocket, buffer: memoryview) -> int:
    try:
        return _unlocked_ssl_recv_into(sock, buffer)
    except SSLError as x:
        if x.args[0] == SSL_ERROR_EOF and sock.suppress_ragged_eofs:
            return 0
        else:
            raise
