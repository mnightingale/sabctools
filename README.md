
SABCTools - C implementations of functions for use within SABnzbd
===============================

This module implements three main sets of C implementations that are used within SABnzbd: 
* yEnc decoding and encoding using SIMD routines
* CRC32 calculations
* TLS connections that do not hold the GIL
* Non-blocking SSL-socket reading
* Marking files as sparse

Of course, they can also be used in any other application.

## yEnc decoding and encoding using SIMD routines
yEnc decoding and encoding performed by using [yencode](https://github.com/animetosho/node-yencode) from animetosho, 
which utilizes x86/ARM SIMD optimised routines if such CPU features are available.

## CRC32 calculations
We used the `crcutil` library for very fast CRC calculations.

## TLS connections that do not hold the GIL
`sabctools.TLSContext` and `sabctools.TLSSocket` implement a TLS client on top of a statically linked
[aws-lc](https://github.com/aws/aws-lc), covering the part of `ssl.SSLSocket` that a download path needs.
Because the `SSL` object belongs to us, a single `recv_into()` drains as many TLS records as fit in the
buffer without ever reacquiring the GIL, `send()` does the same in the other direction, and sessions are
resumed across reconnects. It depends on no CPython internals.

```python
import socket, sabctools

context = sabctools.TLSContext(ca_certs=sabctools.collect_ca_certs())
sock = socket.create_connection(("news.example.org", 563), timeout=60)
tls = context.wrap_socket(sock, server_hostname="news.example.org")
tls.setblocking(False)
```

`collect_ca_certs()` gathers the platform's trust store as a PEM blob, using the same sources the `ssl`
module does, so locally installed roots keep working.

## Non-blocking SSL-socket reading
When Python reads data from a non-blocking SSL socket, it is limited to receiving 16K data at once. This module implements a patched version that can read as much data is available at once.
For more details, see the [cpython pull request](https://github.com/python/cpython/pull/31492).

This predates the `TLSSocket` above and remains as the fallback for builds without aws-lc.

## Marking files as sparse
Uses Windows specific system calls to mark files as sparse and set the desired size.
On other platforms the same is achieved by calling `truncate`.

## Utility functions
Use `sabctools.bytearray_malloc(size)` to get an `bytearray` that is uninitialized (not set to `0`'s). 
This is much faster than the built-in `bytearray(size)` because the data inside the new `bytearray` will be whatever is present in the memory block.

Use `sabctools.rarfile_rar3_s2k` as a native replacement for `rarfile` via `rarfile.rar3_s2k = sabctools.rarfile_rar3_s2k`.   
It provides a significant speed increase for decrypting RAR4 headers when the password length exceeds 28 characters.

# Installing

As simple as running:
```
pip install sabctools --upgrade
```
When you want to compile from sources, you can run in the `sabctools` directory:
```
git submodule update --init --recursive
pip install .
```

> [!NOTE]
> You need a compiler that supports at least C++17 to compile the extension.

The `third_party/aws-lc` submodule backs `TLSContext`/`TLSSocket` and is built with CMake as part of
`pip install`. It needs neither Go nor Perl, and NASM only for the Windows x86_64 assembly. It is not
part of the source distribution because of its size, so an sdist install builds without it. Set
`SABCTOOLS_AWSLC=0` to skip it deliberately. Either way the module still builds and
`unlocked_ssl_recv_into` remains available, only `sabctools.aws_lc_linked` becomes `False`.

## SIMD detection

To see which SIMD set was detected on your system, run:
```
python -c "import sabctools; print(sabctools.simd);"
```

## OpenSSL detection

To see if we could link to OpenSSL library on your system, run:
```
python -c "import sabctools; print(sabctools.openssl_linked);"
```

## aws-lc detection

To see whether the TLS support was built in, run:
```
python -c "import sabctools; print(sabctools.aws_lc_linked, sabctools.aws_lc_version);"
```

# Testing

For testing we use `pytest` (install via `pip install --group test`) and test can simply be executed by browsing to the `sabctools` directory and running:
```
pytest
```
Note that tests can fail if `git` modified the line endings of data files when checking out the repository!