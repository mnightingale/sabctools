import hashlib
import os
import ssl
import sys
from functools import lru_cache
from struct import pack, unpack

# C-extension is placed as submodule to allow typing
from sabctools.sabctools import *

__version__ = version

# id-kp-serverAuth, the only purpose we care about when picking Windows roots
_SERVER_AUTH_OID = "1.3.6.1.5.5.7.3.1"
_PEM_HEADER = b"-----BEGIN CERTIFICATE-----"


def _pem_from_verify_paths() -> list:
    """Read the certificates OpenSSL would have used for its default paths.

    Most Linux distributions point Python at a capath directory, which the ssl
    module only loads lazily, so get_ca_certs() reports nothing for them.
    """
    certs = []
    paths = ssl.get_default_verify_paths()

    if paths.cafile and os.path.isfile(paths.cafile):
        with open(paths.cafile, "rb") as cafile:
            certs.append(cafile.read())

    if paths.capath and os.path.isdir(paths.capath):
        for entry in sorted(os.listdir(paths.capath)):
            full_path = os.path.join(paths.capath, entry)
            try:
                with open(full_path, "rb") as cert:
                    contents = cert.read()
            except OSError:
                continue
            # The directory also holds CRLs and other non-certificate files
            if _PEM_HEADER in contents:
                certs.append(contents)

    return certs


def _pem_from_certifi() -> list:
    """Last resort for platforms where Python has no usable trust store"""
    try:
        import certifi
    except ImportError:
        return []

    try:
        with open(certifi.where(), "rb") as bundle:
            return [bundle.read()]
    except OSError:
        return []


@lru_cache(maxsize=1)
def collect_ca_certs() -> bytes:
    """Return the platform's trusted root certificates as a concatenated PEM blob.

    TLSContext has no access to the system trust store, so the roots have to be
    handed to it explicitly. The sources are the same ones the ssl module uses,
    which keeps locally installed roots (corporate proxies, virus scanners)
    working just as they do today.
    """
    certs = []

    if sys.platform == "win32":
        for store in ("ROOT", "CA"):
            for cert, encoding, trust in ssl.enum_certificates(store):
                # trust is True for "all purposes", or a set of enhanced key usage OIDs
                if encoding == "x509_asn" and (trust is True or _SERVER_AUTH_OID in trust):
                    certs.append(ssl.DER_cert_to_PEM_cert(cert).encode())
    else:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_default_certs(ssl.Purpose.SERVER_AUTH)
        certs = [ssl.DER_cert_to_PEM_cert(cert).encode() for cert in context.get_ca_certs(binary_form=True)]

        if not certs:
            certs = _pem_from_verify_paths()

    if not certs:
        certs = _pem_from_certifi()

    return b"".join(certs)


def rarfile_rar3_s2k(pwd, salt):
    """String-to-key hash for RAR3."""
    rar_max_password = 127
    if not isinstance(pwd, str):
        pwd = pwd.decode("utf8")
    wstr = pwd.encode("utf-16le")[: rar_max_password * 2]
    seed = bytearray(wstr + salt)
    h = hashlib.sha1()
    iv = bytearray(16)
    for i in range(16):
        iv[i] = rarfile_rar3_loop(h, seed, i << 14)
    key_be = h.digest()[:16]
    key_le = pack("<LLLL", *unpack(">LLLL", key_be))
    return key_le, bytes(iv)
