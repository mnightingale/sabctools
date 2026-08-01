#!/usr/bin/env python3
# Copyright 2007-2026 The SABnzbd-Team (sabnzbd.org)
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

"""
Re-vendor src/par2/ from par2cmdline-turbo.

    python tools/vendor_par2.py                     # the pinned ref below
    python tools/vendor_par2.py --ref v1.4.0        # a tag or branch
    python tools/vendor_par2.py --ref 6b6a699       # or a commit

Leaves the result in the working tree for review; does not commit.
"""

import datetime
import os
import shutil
import sys
import tempfile

import vendor_common

REPO = "https://github.com/nzbgetcom/par2cmdline-turbo.git"
REF = "6b6a69942478e4ad0556f5f3eda2cec8c46f3d0e"

DEST = os.path.join(vendor_common.ROOT, "src", "par2")

# Only what we compile, plus upstream's CMake build and the provenance/licence files.
# Our own CMakeLists.txt drives that CMake rather than reimplementing the per-file SIMD
# flag matrix.
COPY_TREES = ("src", "include", "cmake", os.path.join("parpar", "gf16"), os.path.join("parpar", "hasher"))
COPY_FILES = (
    "CMakeLists.txt",
    "COPYING",
    "AUTHORS",
    "ChangeLog",
    os.path.join("parpar", "gf16.cmake"),
    os.path.join("parpar", "hasher.cmake"),
)


def prune():
    """Drop what we do not build: upstream's unit tests, MSBuild projects and OpenCL.

    controller_ocl* is already left out of upstream's own CMake source list.
    """
    for directory, _dirs, files in os.walk(DEST, topdown=False):
        for name in files:
            if name.endswith("_test.cpp") or ".vcxproj" in name or name.startswith("controller_ocl"):
                os.remove(os.path.join(directory, name))

    for leftover in (
        os.path.join(DEST, "parpar", "gf16", "opencl-include"),
        os.path.join(DEST, "parpar", "gf16", "suppressions-valgrind.supp"),
    ):
        if os.path.isdir(leftover):
            shutil.rmtree(leftover)
        elif os.path.exists(leftover):
            os.remove(leftover)


def apply_patches():
    # Upstream targets a standalone executable and links the static CRT. A CPython
    # extension must use the dynamic CRT so it shares a heap and a std:: runtime with
    # python3xx.dll, and an explicit add_compile_options(/MT) cannot be overridden by
    # CMAKE_MSVC_RUNTIME_LIBRARY.
    vendor_common.patch(
        DEST,
        os.path.join("cmake", "common.cmake"),
        "add_compile_options(/MTd /Zi /MP /W4 /utf-8)",
        "add_compile_options(/MDd /Zi /MP /W4 /utf-8)",
        "dynamic CRT (debug)",
    )
    vendor_common.patch(
        DEST,
        os.path.join("cmake", "common.cmake"),
        "add_compile_options(/MT /Oi /MP /utf-8 /guard:cf)",
        "add_compile_options(/MD /Oi /MP /utf-8 /guard:cf)",
        "dynamic CRT (release)",
    )


def write_vendor_notes(commit: str, ref: str):
    with open(os.path.join(DEST, "VENDOR.md"), "w", encoding="utf-8") as handle:
        handle.write(
            """# Vendored par2cmdline-turbo

| | |
|---|---|
| Upstream | {repo} |
| Ref | `{ref}` |
| Commit | `{commit}` |
| Vendored | {date} |

This is [nzbgetcom/par2cmdline-turbo](https://github.com/nzbgetcom/par2cmdline-turbo)'s
`nzbget` branch, a fork of [animetosho/par2cmdline-turbo](https://github.com/animetosho/par2cmdline-turbo)
that wraps the sources in `namespace Par2`, moves headers under `include/par2/`, and adds the
virtual `Sig*` hooks and `cancelled` flag that let `Par2Repairer` be driven as a library.

## Licensing

par2cmdline-turbo is GPL-2.0-or-later (see `COPYING`); the ParPar `gf16`/`hasher` backend
under `parpar/` is Public Domain / CC0. sabctools is GPL-2.0-or-later, so both are compatible.

## Updating

```bash
python tools/vendor_par2.py --ref <tag, branch or commit>
```

Then review the diff and rebuild. Update the `REF` default in `tools/vendor_par2.py` to
match, so a plain re-run reproduces the same tree.

## How it is built

Our top-level `CMakeLists.txt` runs upstream's own CMake (`CMakeLists.txt` + `cmake/` +
`parpar/*.cmake`) as a nested project to produce the `par2-turbo`, `gf16` and `hasher`
static libraries, and links them into the extension. Upstream owns the ~100-file per-ISA
SIMD flag matrix and the probes that gate it, so re-vendoring picks up new kernels without
any change here.

### Local patches

Applied by `tools/vendor_par2.py` on every run. Each one fails the vendoring if it stops
matching, so a patch that upstream has since fixed cannot be carried silently.

1. `cmake/common.cmake`: `/MT` -> `/MD` (and `/MTd` -> `/MDd`). Upstream targets a standalone
   executable and links the static CRT. A CPython extension must use the dynamic CRT so it
   shares a heap and a std:: runtime with `python3xx.dll`, and an explicit
   `add_compile_options(/MT)` cannot be overridden by `CMAKE_MSVC_RUNTIME_LIBRARY`.
""".format(repo=REPO, ref=ref, commit=commit, date=datetime.date.today().isoformat())
        )


def main():
    arguments = vendor_common.parse_args(__doc__, REF, REPO)

    with tempfile.TemporaryDirectory() as temporary:
        checkout = os.path.join(temporary, "par2")
        print("==> Fetching %s at %s" % (arguments.repo, arguments.ref))
        commit = vendor_common.fetch(arguments.repo, arguments.ref, checkout)
        print("==> Resolved to %s" % commit)

        print("==> Replacing %s" % DEST)
        vendor_common.copy_sources(checkout, DEST, COPY_TREES, COPY_FILES)
        prune()
        apply_patches()
        write_vendor_notes(commit, arguments.ref)

    print("==> Done. src/par2/ now at %s" % commit)
    print("    Review with: git status && git diff --stat")
    print("    Then rebuild: pip install . -v")


if __name__ == "__main__":
    sys.exit(main())
