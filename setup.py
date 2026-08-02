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

# Vendored dependencies, each built by its own upstream CMake.
# See src/par2/VENDOR.md and src/rapidyenc/VENDOR.md for the exact commits.
PAR2_DIR = "src/par2"
RAPIDYENC_DIR = "src/rapidyenc"


def flag_supported(compiler: Type[CCompiler], flag: str) -> bool:
    """Whether the compiler accepts a flag, by compiling an empty program with it.

    Only the C++ standard flags are probed here. Everything ISA-specific belongs to the
    vendored CMake builds, which run their own CHECK_CXX_COMPILER_FLAG probes.
    """
    with ExitStack() as stack:
        tmpdir = tempfile.mkdtemp()
        stack.callback(shutil.rmtree, tmpdir)
        tmp_fd, tmp_path = tempfile.mkstemp(suffix=".cc", prefix="sabctools_", dir=tmpdir)

        with closing(os.fdopen(tmp_fd, "w")) as f:
            f.write("int main (int argc, char **argv) { return 0; }")

        log.info("==> Checking support for flag: %s", flag)
        try:
            log.info("==> Please ignore any errors shown below!")
            compiler.compile([tmp_path], output_dir=tmpdir, extra_postargs=[flag])
            log.info("==> Success!")
        except (VendoredCompileError, StdlibCompileError):
            log.info("==> Not available!")
            return False

    return True


class SABCToolsBuild(build_ext):
    # Static libraries produced by the vendored par2 CMake build, in link order:
    # archives only satisfy symbols demanded to their left.
    PAR2_LIBRARIES = ("par2-turbo", "gf16", "hasher")

    def build_vendored(
        self,
        name: str,
        source_dir: str,
        cmake_args: list,
        library_names: tuple,
        copy_from_slice: tuple = (),
    ) -> tuple:
        """Build one vendored dependency with its own upstream CMake build.

        Driving upstream's CMake rather than reimplementing it in setup.py keeps the
        per-ISA SIMD flag matrices, the ISA probes that gate them, the MASM stub for
        MSVC's XOR-JIT and any generated config.h in upstream's hands - and builds them
        in parallel instead of serially through distutils. Re-vendoring then picks up
        new kernels with no change here.

        copy_from_slice names files that a universal2 build should take from the first
        slice; see build_universal().

        Returns (build_dir, libraries) for linking into the extension.
        """
        build_dir = os.path.abspath(os.path.join(self.build_temp, name))
        source_dir = os.path.abspath(source_dir)

        arches = sorted(set(re.findall(r"-arch\s+(\S+)", os.environ.get("ARCHFLAGS", ""))))

        if sys.platform == "darwin" and len(arches) > 1:
            build_dir, libraries = self.build_universal(
                name, source_dir, build_dir, arches, cmake_args, library_names, copy_from_slice
            )
        else:
            extra = []
            if sys.platform == "darwin" and arches:
                extra.append("-DCMAKE_OSX_ARCHITECTURES=" + arches[0])
            self.configure_and_build(name, source_dir, build_dir, cmake_args + extra)
            libraries = [self.find_static_library(build_dir, library) for library in library_names]

        for library in libraries:
            log.info("==> Built %s", library)

        return build_dir, libraries

    def build_universal(
        self,
        name: str,
        source_dir: str,
        build_dir: str,
        arches: list,
        cmake_args: list,
        library_names: tuple,
        copy_from_slice: tuple,
    ) -> tuple:
        """Build one slice per architecture, then lipo them together.

        A single configure with several -arch flags does not work: CMake reports one
        CMAKE_SYSTEM_PROCESSOR, so the vendored CMake picks exactly one of IS_X86 /
        IS_ARM, and the per-file ISA flags are applied for that one only. Worse,
        CHECK_CXX_COMPILER_FLAG then probes with every -arch at once, so flags valid for
        a single slice - -mavx2, -march=armv8.2-a+sha3 - fail and get dropped for *all*
        slices. A universal2 build done that way loses essentially all of the SIMD on
        both halves.

        Building each slice separately with CMAKE_SYSTEM_NAME set puts CMake into
        cross-compiling mode, which is what makes it honour the CMAKE_SYSTEM_PROCESSOR
        we hand it. nzbget's cmake/par2-turbo.cmake passes the same pair for the same
        reason.
        """
        slices = {}
        for arch in arches:
            slice_dir = "%s-%s" % (build_dir, arch)
            self.configure_and_build(
                name,
                source_dir,
                slice_dir,
                cmake_args
                + [
                    "-DCMAKE_OSX_ARCHITECTURES=" + arch,
                    "-DCMAKE_SYSTEM_NAME=Darwin",
                    "-DCMAKE_SYSTEM_PROCESSOR=" + arch,
                ],
            )
            slices[arch] = slice_dir

        os.makedirs(build_dir, exist_ok=True)
        libraries = []
        for library in library_names:
            merged = os.path.join(build_dir, "lib%s.a" % library)
            inputs = [self.find_static_library(slices[arch], library) for arch in arches]
            log.info("==> Merging %s for %s", library, ", ".join(arches))
            subprocess.check_call(["lipo", "-create"] + inputs + ["-output", merged])
            libraries.append(merged)

        # par2's generated config.h only records host/OS traits, which are identical
        # across slices here, so the glue can be compiled against any one of them.
        for generated in copy_from_slice:
            shutil.copy(os.path.join(slices[arches[0]], generated), os.path.join(build_dir, generated))
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
    def cmake_env() -> dict:
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

    def configure_and_build(self, name: str, source_dir: str, build_dir: str, extra_args: list) -> None:
        """Configure and build one vendored tree."""
        configure = [
            "cmake",
            "-S",
            source_dir,
            "-B",
            build_dir,
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

        env = self.cmake_env()

        log.info("==> Configuring %s: %s", name, " ".join(configure))
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
        log.info("==> Building %s: %s", name, " ".join(build))
        subprocess.check_call(build, env=env)

    @staticmethod
    def find_static_library(build_dir: str, name: str) -> str:
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
            "CMake build did not produce %s under %s - looked for %s" % (name, build_dir, ", ".join(candidates))
        )

    def build_extension(self, ext: Extension):
        # Compiler flags for our own sources. Nothing ISA-specific is decided here any
        # more: every SIMD kernel now lives in a vendored tree with its own CMake, which
        # runs its own architecture detection and per-file flag probes.

        # Baseline for src/par2.cc, the glue against the CMake-built par2 libraries
        par2_glue_flags = []
        if self.compiler.compiler_type == "msvc":
            # LTCG not enabled due to issues seen with code generation where
            # different ISA extensions are selected for specific files
            ldflags = ["/OPT:REF", "/OPT:ICF"]
            cflags = ["/O2", "/GS-", "/Gy", "/sdl-", "/Oy", "/Oi"]
            if flag_supported(self.compiler, "/std:c++20"):
                cxx_std_flag = "/std:c++20"
            elif flag_supported(self.compiler, "/std:c++17"):
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
            if flag_supported(self.compiler, "-std=c++20"):
                cxx_std_flag = "-std=c++20"
            elif flag_supported(self.compiler, "-std=c++17"):
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

        par2_build_dir, par2_libraries = self.build_vendored(
            "par2",
            PAR2_DIR,
            ["-DBUILD_LIB=ON", "-DBUILD_TOOL=OFF"],
            self.PAR2_LIBRARIES,
            copy_from_slice=("config.h",),
        )
        par2_includes = [os.path.join(os.path.abspath(PAR2_DIR), "include"), par2_build_dir]

        # Only the static library is wanted; the shared one and the CLI tools would be
        # built for nothing. The sources call the public C API in rapidyenc.h, which
        # resolves relative to src/ and so needs no include directory of its own.
        _, rapidyenc_libraries = self.build_vendored(
            "rapidyenc",
            RAPIDYENC_DIR,
            ["-DDISABLE_SHARED=ON", "-DDISABLE_TOOL=ON"],
            ("rapidyenc",),
        )

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

        output_dir = os.path.dirname(self.build_lib)
        compiled_objects = []
        for source_files in [
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
                "macros": [],
            }
            if self.compiler.compiler_type == "msvc":
                if "msvc_libraries" in source_files:
                    ext.libraries += source_files["msvc_libraries"]
            elif "gcc_flags" in source_files:
                args["extra_postargs"] += source_files["gcc_flags"]

            if "include_dirs" in source_files:
                args["include_dirs"] = source_files["include_dirs"]
            if "macros" in source_files:
                args["macros"].extend(source_files["macros"])

            self.compiler.compile(**args)
            compiled_objects += self.compiler.object_filenames(source_files["sources"], output_dir=output_dir)

        # attach to Extension. The vendored archives go last: static libraries only
        # satisfy symbols already demanded to their left, and it is our own objects -
        # src/par2.o, src/yenc.o, src/crc32.o - that demand them.
        vendored_libraries = par2_libraries + rapidyenc_libraries
        ext.extra_link_args = ldflags + compiled_objects + vendored_libraries
        ext.depends = ["src/sabctools.h"] + compiled_objects + vendored_libraries

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
