# Build the vendored par2cmdline as a static library.
#
# Upstream ships autotools and an MSVC project, neither of which fits a Python
# extension build. There is no CMake to drive, so unlike rapidyenc this is not an
# ExternalProject but a target in our own build - which is straightforward here
# precisely because this fork carries no ParPar: no per-ISA flag matrix, no
# compiler probes, nothing that has to stay in upstream's hands.
#
# It also means a macOS universal2 build needs no slicing for par2. The slicing
# exists because CHECK_CXX_COMPILER_FLAG probes with every -arch at once and so
# drops every ISA flag; with no probes to run, one build with several -arch flags
# is correct.

include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CheckSymbolExists)
include(TestBigEndian)

set(PAR2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/par2")

# --- what upstream builds into libpar2 ---------------------------------------
# Mirrors libpar2_a_SOURCES in the vendored Makefile.am. Deliberately a list
# rather than a glob, and checked against the tree below, so that re-vendoring a
# reshaped upstream fails at configure time instead of at link time.
#
# commandline.cpp is not in that list and is not built: it is the tool's option
# parser, and the library proper never includes commandline.h. That separation is
# what lets the glue depend on the public header alone.
set(PAR2_SOURCES
    crc.cpp
    creatorpacket.cpp
    criticalpacket.cpp
    datablock.cpp
    descriptionpacket.cpp
    diskfile.cpp
    filechecksummer.cpp
    galois.cpp
    libpar2.cpp
    mainpacket.cpp
    md5.cpp
    par1fileformat.cpp
    par1repairer.cpp
    par1repairersourcefile.cpp
    par2creator.cpp
    par2creatorsourcefile.cpp
    par2fileformat.cpp
    par2repairer.cpp
    par2repairersourcefile.cpp
    recoverypacket.cpp
    reedsolomon.cpp
    utf8.cpp
    verificationhashtable.cpp
    verificationpacket.cpp
)

file(GLOB _par2_present RELATIVE "${PAR2_DIR}/src" CONFIGURE_DEPENDS "${PAR2_DIR}/src/*.cpp")
list(REMOVE_ITEM _par2_present commandline.cpp)
set(_par2_expected ${PAR2_SOURCES})
list(SORT _par2_present)
list(SORT _par2_expected)
if(NOT _par2_present STREQUAL _par2_expected)
    message(FATAL_ERROR
        "the vendored par2 sources do not match cmake/par2.cmake.\n"
        "  in the tree : ${_par2_present}\n"
        "  expected    : ${_par2_expected}\n"
        "Re-check libpar2_a_SOURCES in src/par2/Makefile.am and update PAR2_SOURCES.")
endif()

list(TRANSFORM PAR2_SOURCES PREPEND "${PAR2_DIR}/src/")

# --- config.h -----------------------------------------------------------------
# libpar2internal.h has a no-config fallback, but it never defines __BYTE_ORDER,
# and `#if __BYTE_ORDER == __LITTLE_ENDIAN` with both sides undefined quietly
# evaluates true. Probing properly is the only way that is not an accident.
check_include_file_cxx(stdio.h HAVE_STDIO_H)
check_include_file_cxx(stdlib.h HAVE_STDLIB_H)
check_include_file_cxx(limits.h HAVE_LIMITS_H)
check_include_file_cxx(memory.h HAVE_MEMORY_H)
check_include_file_cxx(unistd.h HAVE_UNISTD_H)
check_include_file_cxx(sys/types.h HAVE_SYS_TYPES_H)
check_include_file_cxx(sys/stat.h HAVE_SYS_STAT_H)
check_include_file_cxx(dirent.h HAVE_DIRENT_H)
check_include_file_cxx(sys/ndir.h HAVE_SYS_NDIR_H)
check_include_file_cxx(sys/dir.h HAVE_SYS_DIR_H)
check_include_file_cxx(ndir.h HAVE_NDIR_H)
check_include_file_cxx(endian.h HAVE_ENDIAN_H)

if(HAVE_STDLIB_H AND HAVE_STDIO_H AND HAVE_LIMITS_H)
    set(STDC_HEADERS 1)
endif()

check_symbol_exists(memcpy string.h HAVE_MEMCPY)
check_symbol_exists(fseeko stdio.h HAVE_FSEEKO)
check_symbol_exists(posix_fadvise fcntl.h HAVE_POSIX_FADVISE)

test_big_endian(WORDS_BIGENDIAN)

set(PAR2_PACKAGE "par2cmdline")
set(PAR2_VERSION "${SABCTOOLS_VERSION}")
set(PAR2_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/par2-config")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/par2-config.h.in"
    "${PAR2_CONFIG_DIR}/config.h"
    @ONLY
)

# --- the library --------------------------------------------------------------
add_library(par2 STATIC ${PAR2_SOURCES})

# C++17 is upstream's own requirement, from its Makefile.am. It is also what the
# glue must be built at, for the same reason: under C++20 libc++ instantiates
# constexpr destructors eagerly and par2's incomplete types do not survive it.
set_target_properties(par2 PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(par2
    PUBLIC "${PAR2_DIR}/include"
    PRIVATE "${PAR2_DIR}/src" "${PAR2_CONFIG_DIR}"
)
target_compile_definitions(par2 PRIVATE HAVE_CONFIG_H)

# par2 throws, so exceptions stay on here whatever the rest of the build does.
# Upstream compiles with -Wall; its warnings are not ours to act on.
if(MSVC)
    target_compile_options(par2 PRIVATE /O2 /EHsc /utf-8 /W0)
else()
    target_compile_options(par2 PRIVATE -O3 -w)
endif()

# par2repairer, par2creator and diskfile parallelise with OpenMP, and
# SetThreadCounts is what sizes it. Without OpenMP the pragmas are ignored and
# the library still works, single-threaded - which AppleClang, shipping no
# libomp, is the usual reason for.
find_package(OpenMP COMPONENTS CXX)
if(OpenMP_CXX_FOUND)
    target_link_libraries(par2 PRIVATE OpenMP::OpenMP_CXX)
    message(STATUS "par2: OpenMP enabled")
else()
    message(STATUS "par2: no OpenMP, par2 will not use multiple threads")
endif()
