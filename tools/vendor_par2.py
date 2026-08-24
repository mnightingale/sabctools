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
Re-vendor src/par2/ from par2cmdline.

    python tools/vendor_par2.py                     # the pinned ref below
    python tools/vendor_par2.py --ref libpar2/...   # a tag or branch
    python tools/vendor_par2.py --ref 138c411       # or a commit

Leaves the result in the working tree for review; does not commit.
"""

import datetime
import os
import shutil
import sys
import tempfile

import vendor_common

REPO = "https://github.com/mnightingale/par2cmdline.git"

# Tip of the libpar2/* stack, which is where the library API lives, currently the
# branch libpar2/verifier-fixes. Pinned to the commit rather than the branch: these
# are topic branches being prepared for upstream, so they are rebased, and a branch
# name would not name the same tree twice running.
REF = "4165a7bac430c05f32b179826a6bb0bf6782f153"

DEST = os.path.join(vendor_common.ROOT, "src", "par2")

# The library sources and the public header. Upstream ships no CMake, so nothing
# of its build system is worth copying - our own CMakeLists.txt compiles these
# directly, which it can because there is no per-ISA flag matrix to honour.
COPY_TREES = ("src", "include")
COPY_FILES = ("COPYING", "AUTHORS", "ChangeLog", "README.md", "config.h.in")


def prune():
    """Drop upstream's unit tests and the command line tool.

    Only the library is wanted. par2cmdline.cpp is the tool's entry point, and
    the *_test.cpp files are built by `make check` against a test framework we
    do not vendor.
    """
    for name in os.listdir(os.path.join(DEST, "src")):
        if name.endswith("_test.cpp") or name in ("par2cmdline.cpp",):
            os.remove(os.path.join(DEST, "src", name))


def apply_patches():
    """No local patches.

    The /MT -> /MD patch the nzbgetcom fork needed was against its CMake, which
    upstream does not have; our own CMakeLists.txt picks the runtime library, and
    CMake defaults to the dynamic one a CPython extension needs. Kept as the place
    to add a patch, and vendor_common.patch() fails loudly once one stops matching.
    """


def write_vendor_notes(commit: str, ref: str):
    with open(os.path.join(DEST, "VENDOR.md"), "w", encoding="utf-8") as handle:
        handle.write(
            """# Vendored par2cmdline

| | |
|---|---|
| Upstream | {repo} |
| Ref | `{ref}` |
| Commit | `{commit}` |
| Vendored | {date} |

This is a fork of [Parchive/par2cmdline](https://github.com/Parchive/par2cmdline) carrying
a stack of `libpar2/*` topic branches that make par2 usable as a library, being prepared
for upstream. The pinned commit is the tip of that stack.

## Why this rather than par2cmdline-turbo

The lineage is upstream -> [animetosho/par2cmdline-turbo](https://github.com/animetosho/par2cmdline-turbo)
-> [nzbgetcom/par2cmdline-turbo](https://github.com/nzbgetcom/par2cmdline-turbo). We used
the nzbgetcom fork first, because it was the only one that could be driven as a library at
all - but doing so meant subclassing `Par2Repairer` and reading its protected members.

This fork instead exposes a real public API in `include/par2/libpar2.h`: a `Par2Verifier`
handle and a `Par2Observer` callback interface, with the implementation behind a pimpl. The
glue therefore depends on nothing but that header, which is the point - once these changes
reach turbo, moving there is a re-vendor rather than a rewrite.

The trade for now is that upstream has neither turbo's CMake nor its ParPar SIMD backend,
so repair throughput is the scalar implementation.

## Licensing

par2cmdline is GPL-2.0-or-later (see `COPYING`). sabctools is GPL-2.0-or-later, so they are
compatible.

## Updating

```bash
python tools/vendor_par2.py --ref <tag, branch or commit>
```

Then review the diff and rebuild. Update the `REF` default in `tools/vendor_par2.py` to
match, so a plain re-run reproduces the same tree.

## How it is built

Upstream builds with autotools, which does not fit a Python extension build and does not
cover MSVC. Our own `CMakeLists.txt` compiles the sources in `src/` into a static library
instead. That is viable here precisely because there is no ParPar: no per-ISA flag matrix,
no compiler probes, nothing upstream needs to own.

`src/par2.cc` calls only the public API in `include/par2/libpar2.h`. The headers under
`src/` are upstream's internals and are not part of its compatibility promise.

### Local patches

None. The vendoring script fails loudly if a patch it carries stops matching, so this
section is the one to check when adding one.
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
