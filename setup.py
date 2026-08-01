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
import tempfile
import shutil
import subprocess
import logging
from typing import Type
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

# Vendored par2cmdline-turbo; see src/par2/VENDOR.md for the exact commit.
PAR2_DIR = "src/par2"


def autoconf_check(
    compiler: Type[CCompiler],
    include_check: str = None,
    define_check: str = None,
    flag_check=None,
):
    """A makeshift Python version of the autoconf checks

    flag_check accepts a single flag or a list of flags that must be applied together
    (some ISA extensions only compile when their prerequisites are also enabled).
    """
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
            if isinstance(flag_check, str):
                flag_check = [flag_check]
            log.info("==> Checking support for flag(s): %s", " ".join(flag_check))
            extra_postargs.extend(flag_check)

        try:
            log.info("==> Please ignore any errors shown below!")
            compiler.compile([tmp_path], output_dir=tmpdir, extra_postargs=extra_postargs)
            log.info("==> Success!")
        except (VendoredCompileError, StdlibCompileError):
            log.info("==> Not available!")
            return False

    return True


class SABCToolsBuild(build_ext):
    # Static libraries produced by the vendored par2 CMake build, in link order:
    # archives only satisfy symbols demanded to their left.
    PAR2_LIBRARIES = ("par2-turbo", "gf16", "hasher")

    def build_par2(self) -> tuple:
        """Build the vendored par2cmdline-turbo with its own CMake build.

        Driving upstream's CMake rather than reimplementing it in setup.py keeps the
        ~100-file per-ISA SIMD flag matrix, the MASM stub for MSVC's XOR-JIT and the
        generated config.h in upstream's hands, and builds them in parallel instead of
        serially through distutils.

        Returns (include_dirs, libraries) for linking into the extension.
        """
        build_dir = os.path.abspath(os.path.join(self.build_temp, "par2"))
        source_dir = os.path.abspath(PAR2_DIR)

        arches = sorted(set(re.findall(r"-arch\s+(\S+)", os.environ.get("ARCHFLAGS", ""))))

        if sys.platform == "darwin" and len(arches) > 1:
            build_dir, libraries = self.build_par2_universal(source_dir, build_dir, arches)
        else:
            extra = []
            if sys.platform == "darwin" and arches:
                extra.append("-DCMAKE_OSX_ARCHITECTURES=" + arches[0])
            self.configure_and_build_par2(source_dir, build_dir, extra)
            libraries = [self.find_par2_library(build_dir, name) for name in self.PAR2_LIBRARIES]

        for library in libraries:
            log.info("==> Built %s", library)

        return ([os.path.join(source_dir, "include"), build_dir], libraries)

    def build_par2_universal(self, source_dir: str, build_dir: str, arches: list) -> tuple:
        """Build one slice per architecture, then lipo them together.

        A single configure with several -arch flags does not work: CMake reports one
        CMAKE_SYSTEM_PROCESSOR, so cmake/common.cmake picks exactly one of IS_X86 /
        IS_ARM, and parpar's per-file ISA flags are applied for that one only. Worse,
        CHECK_CXX_COMPILER_FLAG then probes with every -arch at once, so flags valid for
        a single slice - -mavx2, -march=armv8.2-a+sha3 - fail and get dropped for *all*
        slices. A universal2 build done that way loses essentially all of parpar's SIMD
        on both halves.

        Building each slice separately with CMAKE_SYSTEM_NAME set puts CMake into
        cross-compiling mode, which is what makes it honour the CMAKE_SYSTEM_PROCESSOR
        we hand it. nzbget's cmake/par2-turbo.cmake passes the same pair for the same
        reason.
        """
        slices = {}
        for arch in arches:
            slice_dir = "%s-%s" % (build_dir, arch)
            self.configure_and_build_par2(
                source_dir,
                slice_dir,
                [
                    "-DCMAKE_OSX_ARCHITECTURES=" + arch,
                    "-DCMAKE_SYSTEM_NAME=Darwin",
                    "-DCMAKE_SYSTEM_PROCESSOR=" + arch,
                ],
            )
            slices[arch] = slice_dir

        os.makedirs(build_dir, exist_ok=True)
        libraries = []
        for name in self.PAR2_LIBRARIES:
            merged = os.path.join(build_dir, "lib%s.a" % name)
            inputs = [self.find_par2_library(slices[arch], name) for arch in arches]
            log.info("==> Merging %s for %s", name, ", ".join(arches))
            subprocess.check_call(["lipo", "-create"] + inputs + ["-output", merged])
            libraries.append(merged)

        # config.h only records host/OS traits, which are identical across slices here,
        # so the glue can be compiled against any one of them.
        shutil.copy(os.path.join(slices[arches[0]], "config.h"), os.path.join(build_dir, "config.h"))
        return build_dir, libraries

    @staticmethod
    def usable_ninja() -> bool:
        """Whether Ninja is present *and* actually runs.

        Being on PATH is not enough. pip's build isolation puts the `ninja` wheel's
        launcher script in the overlay bin directory, and it is not always executable
        from there - CMake then fails the whole configure with "no such file or
        directory" rather than falling back. Run it once and believe the result;
        without Ninja, CMake picks its own default generator, which still builds in
        parallel through `cmake --build --parallel`.
        """
        ninja = shutil.which("ninja")
        if not ninja:
            return False
        try:
            subprocess.check_output([ninja, "--version"], stderr=subprocess.STDOUT)
            return True
        except (OSError, subprocess.SubprocessError):
            log.info("==> Ninja found at %s but does not run; letting CMake choose", ninja)
            return False

    @staticmethod
    def par2_cmake_env() -> dict:
        """Environment for the CMake calls, with any -arch flags stripped.

        CMake seeds CMAKE_C_FLAGS / CMAKE_CXX_FLAGS from CFLAGS / CXXFLAGS, so the
        `-arch x86_64 -arch arm64` cibuildwheel exports for universal2 would reach the
        compiler and contradict the CMAKE_OSX_ARCHITECTURES we set per slice - CMake
        detects the mismatch and refuses to configure. Architecture is ours to choose
        here, so take it out of the environment entirely.
        """
        env = dict(os.environ)
        for name in ("ARCHFLAGS", "CFLAGS", "CXXFLAGS", "LDFLAGS"):
            if name in env:
                stripped = re.sub(r"-arch\s+\S+", "", env[name]).strip()
                if stripped:
                    env[name] = stripped
                else:
                    del env[name]
        return env

    def configure_and_build_par2(self, source_dir: str, build_dir: str, extra_args: list) -> None:
        """Configure and build one par2 tree."""
        configure = [
            "cmake",
            "-S",
            source_dir,
            "-B",
            build_dir,
            "-DBUILD_LIB=ON",
            "-DBUILD_TOOL=OFF",
            "-DCMAKE_BUILD_TYPE=Release",
            # The static libraries end up inside a shared object.
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        ]

        if self.compiler.compiler_type == "msvc":
            # Let CMake find Visual Studio itself, and tell it which architecture the
            # extension is being built for. Emphatically do not hand it -G Ninja here:
            # Ninja makes CMake take the first compiler on PATH, which on the GitHub
            # Windows runners is MinGW gcc. That produces GNU-mangled symbols in .a
            # archives that link.exe cannot resolve against an MSVC-built par2.obj.
            architectures = {"win-amd64": "x64", "win-arm64": "ARM64", "win32": "Win32"}
            configure += ["-A", architectures.get(self.plat_name, "x64")]
        elif self.usable_ninja():
            configure += ["-G", "Ninja"]

        deployment_target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
        if sys.platform == "darwin" and deployment_target:
            configure.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + deployment_target)

        configure += extra_args

        env = self.par2_cmake_env()

        log.info("==> Configuring par2: %s", " ".join(configure))
        try:
            subprocess.check_call(configure, env=env)
        except subprocess.CalledProcessError:
            # build/ survives between runs, but a CMakeCache records absolute paths to
            # the compiler and generator. Under pip those can point into a build
            # isolation overlay that has since been deleted, and CMake fails rather
            # than re-detecting. Throw the cache away and configure once more.
            if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
                raise
            log.info("==> Configure failed; discarding stale CMake cache in %s and retrying", build_dir)
            shutil.rmtree(build_dir, ignore_errors=True)
            subprocess.check_call(configure, env=env)

        build = ["cmake", "--build", build_dir, "--config", "Release"]
        if self.parallel:
            build += ["--parallel", str(self.parallel)]
        log.info("==> Building par2: %s", " ".join(build))
        subprocess.check_call(build, env=env)

    @staticmethod
    def find_par2_library(build_dir: str, name: str) -> str:
        """Locate one of the static libraries CMake just produced.

        Where it lands depends on the generator: multi-config generators such as Visual
        Studio add a per-config subdirectory, single-config ones do not, and the prefix
        and suffix differ per platform. Search rather than predict.
        """
        candidates = []
        for suffix in (".a", ".lib"):
            for prefix in ("lib", ""):
                candidates.append(prefix + name + suffix)

        matches = []
        for root, _dirs, files in os.walk(build_dir):
            for candidate in candidates:
                if candidate in files:
                    matches.append(os.path.join(root, candidate))

        # A multi-config generator can leave artefacts for configurations we did not
        # ask for; take the one we built.
        for match in matches:
            if "Release" in match.split(os.sep):
                return match
        if matches:
            return matches[0]

        raise RuntimeError(
            "par2 build did not produce %s under %s - looked for %s"
            % (name, build_dir, ", ".join(candidates))
        )


    def build_extension(self, ext: Extension):
        # Try to determine the architecture to build for
        machine = platform.machine().lower()
        IS_X86 = machine in ["i386", "i686", "x86", "i86pc", "x86_64", "x64", "amd64"]
        IS_MACOS = sys.platform == "darwin"
        IS_ARM = machine.startswith("arm") or machine.startswith("aarch64")
        IS_AARCH64 = True

        log.info("==> Baseline detection: ARM=%s, x86=%s, macOS=%s", IS_ARM, IS_X86, IS_MACOS)

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
        # Baseline for src/par2.cc, the glue against the CMake-built par2 libraries
        par2_glue_flags = []
        if self.compiler.compiler_type == "msvc":
            # LTCG not enabled due to issues seen with code generation where
            # different ISA extensions are selected for specific files
            ldflags = ["/OPT:REF", "/OPT:ICF"]
            cflags = ["/O2", "/GS-", "/Gy", "/sdl-", "/Oy", "/Oi"]
            if autoconf_check(self.compiler, flag_check="/std:c++20"):
                cxx_std_flag = "/std:c++20"
            elif autoconf_check(self.compiler, flag_check="/std:c++17"):
                cxx_std_flag = "/std:c++17"
            else:
                cxx_std_flag = None
                log.info("==> C++17 flag not available")
            if cxx_std_flag:
                cflags.append(cxx_std_flag)
                ext.extra_compile_args.append(cxx_std_flag)

            # /EHsc because the glue catches par2's exceptions and distutils' MSVC
            # defaults omit it; /utf-8 to match how the par2 libraries were compiled.
            par2_glue_flags = ["/O2", "/GS-", "/Gy", "/Oy", "/Oi", "/utf-8", "/EHsc", "/std:c++17"]
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
                cxx_std_flag = "-std=c++20"
            elif autoconf_check(self.compiler, flag_check="-std=c++17"):
                cxx_std_flag = "-std=c++17"
            else:
                cxx_std_flag = None
                log.info("==> C++17 flag not available")
            if cxx_std_flag:
                cflags.append(cxx_std_flag)
                ext.extra_compile_args.append(cxx_std_flag)

            # Baseline for src/par2.cc. Differs from cflags in two ways, both required:
            #  - no -fno-exceptions, because the glue catches par2's exceptions
            #  - pinned to C++17, matching upstream's CMAKE_CXX_STANDARD. Under C++20
            #    libc++'s ~vector() is constexpr and gets eagerly instantiated, which
            #    gfmat_inv.h's std::vector<Galois16RecMatrixWorker> of an incomplete
            #    type does not survive
            par2_glue_flags = [
                "-Wno-unused-function",
                "-Wno-unused-parameter",
                "-fomit-frame-pointer",
                "-fno-rtti",
                "-O3",
                "-fPIC",
                "-fwrapv",
                "-pthread",
                "-std=c++17",
            ]

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

        par2_includes, par2_libraries = self.build_par2()

        # The glue has to agree with the libraries it links against on every macro that
        # changes a layout. PARPAR_INVERT_SUPPORT in particular decides whether
        # Galois16RecMatrix exists in gfmat_inv.h, which par2repairer.h embeds; these
        # mirror the add_compile_definitions() in the vendored cmake/common.cmake.
        par2_glue_group = {
            "sources": ["src/par2.cc"],
            "baseline": "par2_glue",
            "include_dirs": par2_includes + ["src"],
            "macros": [
                ("HAVE_CONFIG_H", None),
                ("PARPAR_ENABLE_HASHER_MD5CRC", None),
                ("PARPAR_INVERT_SUPPORT", None),
                ("PARPAR_SLIM_GF16", None),
            ],
        }

        # build yencode/crcutil
        output_dir = os.path.dirname(self.build_lib)
        compiled_objects = []
        for source_files in [
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
            par2_glue_group,
        ]:
            baseline = {
                "default": cflags,
                "par2_glue": par2_glue_flags,
            }[source_files.get("baseline", "default")]

            args = {
                "sources": source_files["sources"],
                "output_dir": output_dir,
                "extra_postargs": baseline[:],
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

        # attach to Extension. The par2 archives go last: static libraries only satisfy
        # symbols already demanded to their left, and src/par2.o is what demands them.
        ext.extra_link_args = ldflags + compiled_objects + par2_libraries
        ext.depends = ["src/sabctools.h"] + compiled_objects + par2_libraries

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
