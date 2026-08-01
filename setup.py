#!/usr/bin/python3 -OO
# Copyright 2007-2023 The SABnzbd-Team (sabnzbd.org)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

import os
import sys
import platform
import re
import subprocess
import tempfile
import shutil
import logging
from typing import Type, Optional
from contextlib import closing, ExitStack

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

# distutils was removed from the stdlib in Python 3.12; use the setuptools-vendored copy.
from setuptools._distutils.ccompiler import CCompiler

# Depending on SETUPTOOLS_USE_DISTUTILS, the compiler may raise either the vendored or
# the stdlib CompileError (distinct classes), so catch both in the probes below.
from setuptools._distutils.errors import CompileError as VendoredCompileError
from distutils.errors import CompileError as StdlibCompileError

# Plain INFO-level logger
log = logging.getLogger("sabctools.setup")
log.setLevel(logging.INFO)


def autoconf_check(
    compiler: Type[CCompiler],
    include_check: str = None,
    define_check: str = None,
    flag_check: str = None,
):
    """A makeshift Python version of the autoconf checks"""
    with ExitStack() as stack:
        tmpdir = tempfile.mkdtemp()
        stack.callback(shutil.rmtree, tmpdir)
        tmp_fd, tmp_path = tempfile.mkstemp(suffix=".cc", prefix="sabctools_", dir=tmpdir)

        with closing(os.fdopen(tmp_fd, "w")) as f:
            if include_check:
                log.info("==> Checking support for include: %s", include_check)
                f.write(f"#include <{include_check}>\n")

            if define_check:
                log.info("==> Checking support for define: %s", define_check)
                # Just let it crash
                f.write(f"#ifndef {define_check}\n")
                f.write(f"#error {define_check} not available!\n")
                f.write(f"#endif\n")

            f.write("int main (int argc, char **argv) { return 0; }")

        extra_postargs = []
        if flag_check:
            log.info("==> Checking support for flag: %s", flag_check)
            extra_postargs.append(flag_check)

        try:
            log.info("==> Please ignore any errors shown below!")
            compiler.compile([tmp_path], output_dir=tmpdir, extra_postargs=extra_postargs)
            log.info("==> Success!")
        except (VendoredCompileError, StdlibCompileError):
            log.info("==> Not available!")
            return False

    return True


ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
AWSLC_SOURCE_DIR = os.path.join(ROOT_DIR, "third_party", "aws-lc")


def aws_lc_enabled() -> bool:
    """aws-lc is opt-out, so packagers that cannot ship vendored crypto can disable it"""
    return os.environ.get("SABCTOOLS_AWSLC", "1").lower() not in ("0", "false", "no", "off")


def macos_target_archs() -> list:
    """cibuildwheel signals the wanted architectures through ARCHFLAGS, as does a manual
    "ARCHFLAGS=-arch arm64 -arch x86_64 pip wheel ." invocation"""
    archs = re.findall(r"-arch\s+(\S+)", os.environ.get("ARCHFLAGS", ""))
    if not archs:
        archs = [platform.machine()]
    # Preserve order but drop duplicates
    return list(dict.fromkeys(archs))


def run_command(args: list):
    log.info("==> %s", " ".join(args))
    subprocess.run(args, check=True)


def build_aws_lc_single(build_dir: str, arch: Optional[str] = None) -> tuple:
    """Configure and build a static libssl/libcrypto for a single architecture.
    Returns the (libssl, libcrypto) paths."""
    configure = [
        "cmake",
        "-S",
        AWSLC_SOURCE_DIR,
        "-B",
        build_dir,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_LIBSSL=ON",
        # We only need the libraries, not the bssl tool or the (Go-based) test suite
        "-DBUILD_TESTING=OFF",
        "-DBUILD_TOOL=OFF",
        # Fall back to the checked-in generated-src/, so no Go or Perl is needed to build
        "-DDISABLE_GO=ON",
        "-DDISABLE_PERL=ON",
        # Required, we link the result into a shared object
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    ]

    if sys.platform == "darwin":
        # aws-lc only supports one architecture per build, the caller lipo's them together
        configure.append("-DCMAKE_OSX_ARCHITECTURES=%s" % arch)
        if deployment_target := os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
            configure.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=%s" % deployment_target)
    elif sys.platform == "win32":
        # Match the /MD that CPython extensions are built with
        configure.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL")
        # Without an assembler aws-lc still builds, it just uses the slower C code
        assembler = "armasm64" if platform.machine().lower() in ("arm64", "aarch64") else "nasm"
        if not shutil.which(assembler):
            log.info("==> %s not found, building aws-lc without assembly", assembler)
            configure.append("-DOPENSSL_NO_ASM=ON")

    run_command(configure)
    run_command(["cmake", "--build", build_dir, "--config", "Release", "--target", "ssl", "crypto", "--parallel"])

    if sys.platform == "win32":
        # Single-config generators drop the libraries directly in the target directory,
        # multi-config generators (Visual Studio) add a per-configuration subdirectory
        candidates = [
            (os.path.join(build_dir, "ssl", "ssl.lib"), os.path.join(build_dir, "crypto", "crypto.lib")),
            (
                os.path.join(build_dir, "ssl", "Release", "ssl.lib"),
                os.path.join(build_dir, "crypto", "Release", "crypto.lib"),
            ),
        ]
    else:
        candidates = [(os.path.join(build_dir, "ssl", "libssl.a"), os.path.join(build_dir, "crypto", "libcrypto.a"))]

    for libssl, libcrypto in candidates:
        if os.path.exists(libssl) and os.path.exists(libcrypto):
            return libssl, libcrypto

    raise RuntimeError("aws-lc built without error, but no static libraries were produced in %s" % build_dir)


def build_aws_lc(build_temp: str) -> Optional[list]:
    """Build the vendored aws-lc and return the static libraries to link against,
    or None when it is unavailable. Never raises: without aws-lc the module still
    builds, it just falls back to unlocked_ssl_recv_into."""
    if not aws_lc_enabled():
        log.info("==> SABCTOOLS_AWSLC is disabled, skipping aws-lc")
        return None

    if not os.path.exists(os.path.join(AWSLC_SOURCE_DIR, "CMakeLists.txt")):
        log.warning(
            "==> %s is empty, run 'git submodule update --init --recursive' to enable the aws-lc TLS support",
            AWSLC_SOURCE_DIR,
        )
        return None

    if not shutil.which("cmake"):
        log.warning("==> cmake not found, building without aws-lc TLS support")
        return None

    try:
        awslc_root = os.path.join(os.path.abspath(build_temp), "aws-lc")
        if sys.platform == "darwin":
            archs = macos_target_archs()
        else:
            archs = [None]

        libraries = [build_aws_lc_single(os.path.join(awslc_root, arch or "native"), arch) for arch in archs]

        if len(libraries) == 1:
            return list(libraries[0])

        # Fuse the per-architecture builds into universal static libraries
        universal = []
        for index, name in enumerate(("libssl.a", "libcrypto.a")):
            output = os.path.join(awslc_root, name)
            run_command(["lipo", "-create", "-output", output] + [library[index] for library in libraries])
            universal.append(output)
        return universal
    except Exception as error:
        log.warning("==> Failed to build aws-lc (%s), building without aws-lc TLS support", error)
        return None


class SABCToolsBuild(build_ext):
    def build_extension(self, ext: Extension):
        # Try to determine the architecture to build for
        machine = platform.machine().lower()
        IS_X86 = machine in ["i386", "i686", "x86", "i86pc", "x86_64", "x64", "amd64"]
        IS_MACOS = sys.platform == "darwin"
        IS_ARM = machine.startswith("arm") or machine.startswith("aarch64")
        IS_AARCH64 = True

        log.info("==> Baseline detection: ARM=%s, x86=%s, macOS=%s", IS_ARM, IS_X86, IS_MACOS)

        # Build the vendored aws-lc that backs the TLSContext/TLSSocket types.
        # Returns None when unavailable, in which case only the OpenSSL-hooking
        # unlocked_ssl_recv_into is compiled in.
        aws_lc_libraries = build_aws_lc(self.build_temp)

        # Determine compiler flags
        gcc_arm_neon_flags = []
        gcc_arm_crc_flags = []
        gcc_arm_crc_pmull_flags = []
        gcc_vpclmulqdq_flags = []
        gcc_vbmi2_flags = []
        gcc_avx10_flags = []
        gcc_rvv_flags = []
        gcc_rvzbkc_flags = []
        gcc_macros = []
        if self.compiler.compiler_type == "msvc":
            # LTCG not enabled due to issues seen with code generation where
            # different ISA extensions are selected for specific files
            ldflags = ["/OPT:REF", "/OPT:ICF"]
            cflags = ["/O2", "/GS-", "/Gy", "/sdl-", "/Oy", "/Oi"]
            if autoconf_check(self.compiler, flag_check="/std:c++20"):
                cflags.append("/std:c++20")
                ext.extra_compile_args.append("/std:c++20")
            elif autoconf_check(self.compiler, flag_check="/std:c++17"):
                cflags.append("/std:c++17")
                ext.extra_compile_args.append("/std:c++17")
            else:
                log.info("==> C++17 flag not available")
        else:
            # TODO: consider -flto - may require some extra testing
            ldflags = ["-ldl"]  # for dlopen
            cflags = [
                "-Wall",
                "-Wextra",
                "-Wno-unused-function",
                "-fomit-frame-pointer",
                "-fno-rtti",
                "-fno-exceptions",
                "-O3",
                "-fPIC",
                "-fwrapv",
            ]
            if autoconf_check(self.compiler, flag_check="-std=c++20"):
                cflags.append("-std=c++20")
                ext.extra_compile_args.append("-std=c++20")
            elif autoconf_check(self.compiler, flag_check="-std=c++17"):
                cflags.append("-std=c++17")
                ext.extra_compile_args.append("-std=c++17")
            else:
                log.info("==> C++17 flag not available")

            # Verify specific flags for ARM chips
            # macOS ARM does not need any flags, they support everything
            if IS_ARM and not IS_MACOS:
                if not autoconf_check(self.compiler, define_check="__aarch64__"):
                    log.info("==> __aarch64__ not available, disabling 64bit extensions")
                    IS_AARCH64 = False
                if autoconf_check(self.compiler, flag_check="-march=armv8-a+crc+crypto"):
                    gcc_arm_crc_pmull_flags.append("-march=armv8-a+crc+crypto")
                if autoconf_check(self.compiler, flag_check="-march=armv8-a+crc"):
                    gcc_arm_crc_flags.append("-march=armv8-a+crc")
                    # Resolve problems on armv7, see issue #56
                    if not IS_AARCH64:
                        gcc_arm_crc_flags.append("-fno-lto")
                        gcc_arm_crc_pmull_flags.append("-fno-lto")
                if not IS_AARCH64 and autoconf_check(self.compiler, flag_check="-mfpu=neon"):
                    gcc_arm_neon_flags.append("-mfpu=neon")
                    # Resolve problems on armv7, see issue #56
                    gcc_arm_neon_flags.append("-fno-lto")

            # Check for special x32 case
            if (
                IS_X86
                and not IS_MACOS
                and autoconf_check(self.compiler, define_check="__ILP32__")
                and autoconf_check(self.compiler, define_check="__x86_64__")
            ):
                log.info("==> Detected x32 platform, setting CRCUTIL_USE_ASM=0")
                ext.define_macros.append(("CRCUTIL_USE_ASM", "0"))
                gcc_macros.append(("CRCUTIL_USE_ASM", "0"))

            if IS_X86 and autoconf_check(self.compiler, flag_check="-mvpclmulqdq"):
                gcc_vpclmulqdq_flags = ["-mavx2", "-mvpclmulqdq", "-mpclmul"]

            if IS_X86 and autoconf_check(self.compiler, flag_check="-mavx512vbmi2"):
                gcc_vbmi2_flags = [
                    "-mavx512vbmi2",
                    "-mavx512vl",
                    "-mavx512bw",
                    "-mpopcnt",
                    "-mbmi",
                    "-mbmi2",
                    "-mlzcnt",
                ]

            if IS_X86 and autoconf_check(self.compiler, flag_check="-mno-evex512"):
                gcc_avx10_flags = ["-mno-evex512"]

            if machine.startswith("riscv"):
                arch_flag = "-march=rv" + ("32" if machine.startswith("riscv32") else "64") + "gc"
                if autoconf_check(self.compiler, flag_check=arch_flag + "v"):
                    gcc_rvv_flags = [arch_flag + "v"]
                if autoconf_check(self.compiler, flag_check=arch_flag + "_zbkc"):
                    gcc_rvzbkc_flags = [arch_flag + "_zbkc"]

        srcdeps_crc_common = ["src/yencode/common.h", "src/yencode/crc_common.h", "src/yencode/crc.h"]
        srcdeps_dec_common = ["src/yencode/common.h", "src/yencode/decoder_common.h", "src/yencode/decoder.h"]
        srcdeps_enc_common = ["src/yencode/common.h", "src/yencode/encoder_common.h", "src/yencode/encoder.h"]

        # build yencode/crcutil
        output_dir = os.path.dirname(self.build_lib)
        compiled_objects = []
        source_groups = [
            {
                "sources": [
                    "src/yencode/platform.cc",
                    "src/yencode/encoder.cc",
                    "src/yencode/decoder.cc",
                    "src/yencode/crc.cc",
                ],
                "include_dirs": ["src/crcutil-1.0/code", "src/crcutil-1.0/examples"],
            },
            {
                "sources": ["src/yencode/encoder_sse2.cc"],
                "depends": srcdeps_enc_common + ["encoder_sse_base.h"],
                "gcc_x86_flags": ["-msse2"],
            },
            {
                "sources": ["src/yencode/decoder_sse2.cc"],
                "depends": srcdeps_dec_common + ["decoder_sse_base.h"],
                "gcc_x86_flags": ["-msse2"],
            },
            {
                "sources": ["src/yencode/encoder_ssse3.cc"],
                "depends": srcdeps_enc_common + ["encoder_sse_base.h"],
                "gcc_x86_flags": ["-mssse3"],
            },
            {
                "sources": ["src/yencode/decoder_ssse3.cc"],
                "depends": srcdeps_dec_common + ["decoder_sse_base.h"],
                "gcc_x86_flags": ["-mssse3"],
            },
            {
                "sources": ["src/yencode/crc_folding.cc"],
                "depends": srcdeps_crc_common,
                "gcc_x86_flags": ["-mssse3", "-msse4.1", "-mpclmul"],
            },
            {
                "sources": ["src/yencode/crc_folding_256.cc"],
                "depends": srcdeps_crc_common,
                "gcc_x86_flags": gcc_vpclmulqdq_flags,
                "msvc_x86_flags": ["/arch:AVX2"],
            },
            {
                "sources": ["src/yencode/encoder_avx.cc"],
                "depends": srcdeps_enc_common + ["encoder_sse_base.h"],
                "gcc_x86_flags": ["-mavx", "-mpopcnt"],
                "msvc_x86_flags": ["/arch:AVX"],
            },
            {
                "sources": ["src/yencode/decoder_avx.cc"],
                "depends": srcdeps_dec_common + ["decoder_sse_base.h"],
                "gcc_x86_flags": ["-mavx", "-mpopcnt"],
                "msvc_x86_flags": ["/arch:AVX"],
            },
            {
                "sources": ["src/yencode/encoder_avx2.cc"],
                "depends": srcdeps_enc_common + ["encoder_avx_base.h"],
                "gcc_x86_flags": ["-mavx2", "-mpopcnt", "-mbmi", "-mbmi2", "-mlzcnt"],
                "msvc_x86_flags": ["/arch:AVX2"],
            },
            {
                "sources": ["src/yencode/decoder_avx2.cc"],
                "depends": srcdeps_dec_common + ["decoder_avx2_base.h"],
                "gcc_x86_flags": ["-mavx2", "-mpopcnt", "-mbmi", "-mbmi2", "-mlzcnt"],
                "msvc_x86_flags": ["/arch:AVX2"],
            },
            {
                "sources": ["src/yencode/encoder_vbmi2.cc"],
                "depends": srcdeps_enc_common + ["encoder_avx_base.h"],
                "gcc_x86_flags": gcc_vbmi2_flags + gcc_avx10_flags,
                "msvc_x86_flags": ["/arch:AVX512"],
            },
            {
                "sources": ["src/yencode/decoder_vbmi2.cc"],
                "depends": srcdeps_dec_common + ["decoder_avx2_base.h"],
                "gcc_x86_flags": gcc_vbmi2_flags + gcc_avx10_flags,
                "msvc_x86_flags": ["/arch:AVX512"],
            },
            {
                "sources": ["src/yencode/encoder_neon.cc"],
                "depends": srcdeps_enc_common,
                "gcc_arm_flags": gcc_arm_neon_flags,
            },
            {
                "sources": ["src/yencode/decoder_neon64.cc" if IS_AARCH64 else "src/yencode/decoder_neon.cc"],
                "depends": srcdeps_dec_common,
                "gcc_arm_flags": gcc_arm_neon_flags,
            },
            {
                "sources": ["src/yencode/crc_arm.cc"],
                "depends": srcdeps_crc_common,
                "gcc_arm_flags": gcc_arm_crc_flags,
            },
            {
                "sources": ["src/yencode/crc_arm_pmull.cc"],
                "depends": srcdeps_crc_common,
                "gcc_arm_flags": gcc_arm_crc_pmull_flags,
            },
            {
                "sources": ["src/yencode/encoder_rvv.cc", "src/yencode/decoder_rvv.cc"],
                "depends": srcdeps_enc_common + srcdeps_dec_common,
                "gcc_flags": gcc_rvv_flags,
            },
            {
                "sources": ["src/yencode/crc_riscv.cc"],
                "depends": srcdeps_crc_common,
                "gcc_flags": gcc_rvzbkc_flags,
            },
            {
                "sources": [
                    "src/crcutil-1.0/code/crc32c_sse4.cc",
                    "src/crcutil-1.0/code/multiword_64_64_cl_i386_mmx.cc",
                    "src/crcutil-1.0/code/multiword_64_64_gcc_amd64_asm.cc",
                    "src/crcutil-1.0/code/multiword_64_64_gcc_i386_mmx.cc",
                    "src/crcutil-1.0/code/multiword_64_64_intrinsic_i386_mmx.cc",
                    "src/crcutil-1.0/code/multiword_128_64_gcc_amd64_sse2.cc",
                    "src/crcutil-1.0/examples/interface.cc",
                ],
                "gcc_flags": ["-Wno-expansion-to-defined", "-Wno-unused-parameter"],
                "include_dirs": ["src/crcutil-1.0/code", "src/crcutil-1.0/tests"],
                "macros": [("CRCUTIL_USE_MM_CRC32", "0")],
            },
            {
                "sources": [
                    "src/yenc.cc",
                ],
                "gcc_flags": ["-Wno-unused-parameter"],
            },
            {
                "sources": [
                    "src/unlocked_ssl.cc",
                ],
                "gcc_flags": ["-Wno-unused-parameter", "-Wno-missing-field-initializers"],
                "msvc_libraries": ["ws2_32"],
            },
            {
                "sources": [
                    "src/crc32.cc",
                ],
                "gcc_flags": ["-Wno-unused-parameter"],
                "include_dirs": ["src/crcutil-1.0/code", "src/crcutil-1.0/examples"],
            },
            {
                "sources": [
                    "src/sparse.cc",
                ],
                "gcc_flags": ["-Wno-unused-parameter"],
            },
            {
                "sources": [
                    "src/utils.cc",
                ],
                "gcc_flags": ["-Wno-unused-parameter"],
            },
        ]

        if aws_lc_libraries:
            ext.define_macros.append(("SABCTOOLS_AWS_LC", "1"))
            gcc_macros.append(("SABCTOOLS_AWS_LC", "1"))
            source_groups.append(
                {
                    "sources": [
                        "src/tls.cc",
                    ],
                    "gcc_flags": ["-Wno-unused-parameter", "-Wno-missing-field-initializers"],
                    "include_dirs": [os.path.join(AWSLC_SOURCE_DIR, "include")],
                    "msvc_libraries": ["ws2_32", "advapi32", "crypt32", "user32", "bcrypt"],
                }
            )

        for source_files in source_groups:
            args = {
                "sources": source_files["sources"],
                "output_dir": output_dir,
                "extra_postargs": cflags[:],
                "macros": gcc_macros[:],
            }
            if self.compiler.compiler_type == "msvc":
                if IS_X86 and "msvc_x86_flags" in source_files:
                    args["extra_postargs"] += source_files["msvc_x86_flags"]
                if "msvc_libraries" in source_files:
                    ext.libraries += source_files["msvc_libraries"]
            else:
                if "gcc_flags" in source_files:
                    args["extra_postargs"] += source_files["gcc_flags"]
                if IS_X86 and "gcc_x86_flags" in source_files:
                    args["extra_postargs"] += source_files["gcc_x86_flags"]
                if IS_ARM and "gcc_arm_flags" in source_files:
                    args["extra_postargs"] += source_files["gcc_arm_flags"]

            if "include_dirs" in source_files:
                args["include_dirs"] = source_files["include_dirs"]
            if "macros" in source_files:
                args["macros"].extend(source_files["macros"])

            self.compiler.compile(**args)
            compiled_objects += self.compiler.object_filenames(source_files["sources"], output_dir=output_dir)

        # attach to Extension
        if aws_lc_libraries and sys.platform.startswith("linux"):
            # Keep the aws-lc symbols out of the dynamic symbol table, so they can never collide
            # with the OpenSSL that CPython's own _ssl module is linked against. macOS does not
            # need this as it uses two-level namespaces, and static libraries do not export on Windows.
            ldflags.append("-Wl,--exclude-libs,ALL")

        # The static libraries have to follow the objects that reference them
        ext.extra_link_args = ldflags + compiled_objects + (aws_lc_libraries or [])
        ext.depends = ["src/sabctools.h"] + compiled_objects

        # proceed with regular Extension build
        super(SABCToolsBuild, self).build_extension(ext)


def get_version():
    """Parse the version from the C sources"""
    with open("src/sabctools.h", "r") as sabctools_h:
        version = re.findall('#define SABCTOOLS_VERSION +"([0-9xA-Z_.]+)"?', sabctools_h.read())[0]
        return version


setup(
    version=get_version(),
    ext_modules=[
        Extension(
            "sabctools.sabctools",
            ["src/sabctools.cc"],
        )
    ],
    cmdclass={"build_ext": SABCToolsBuild},
)
