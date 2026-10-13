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
    python tools/vendor_par2.py --ref master        # a tag or branch
    python tools/vendor_par2.py --ref a6d3423       # or a commit

Leaves the result in the working tree for review; does not commit.
"""

import datetime
import os
import shutil
import sys
import tempfile

import vendor_common

REPO = "https://github.com/Parchive/par2cmdline.git"

# A commit on feature/in-place-repair
REF = "f0b003720c7294739f510c94c01c2e1b8dc2c04c"

DEST = os.path.join(vendor_common.ROOT, "src", "par2")

# The library sources, the public headers and upstream's CMake build
COPY_TREES = ("src", "include", "cmake")
COPY_FILES = ("CMakeLists.txt", "COPYING", "AUTHORS", "ChangeLog", "README.md")


def prune():
    """Drop upstream's unit tests, the command line tool, its cross toolchains and its generator.

    Only the library is wanted. par2cmdline.cpp is the tool's entry point, the
    *_test.cpp files are built by upstream's test suite, which we do not vendor,
    cmake/toolchains/ only serves upstream's own cross builds, and
    generate-unicodedata.py writes unicodedata.h, which is vendored already.
    """
    for name in os.listdir(os.path.join(DEST, "src")):
        if name.endswith("_test.cpp") or name in ("par2cmdline.cpp", "generate-unicodedata.py"):
            os.remove(os.path.join(DEST, "src", name))
    shutil.rmtree(os.path.join(DEST, "cmake", "toolchains"))


def apply_patches():
    """No local patches. vendor_common.patch() fails loudly once one stops matching."""


def write_vendor_notes(commit: str, ref: str):
    with open(os.path.join(DEST, "VENDOR.md"), "w", encoding="utf-8") as handle:
        handle.write("""# Vendored par2cmdline

| | |
|---|---|
| Upstream | {repo} |
| Ref | `{ref}` |
| Commit | `{commit}` |
| Vendored | {date} |

This is [Parchive/par2cmdline](https://github.com/Parchive/par2cmdline) itself, pinned to a
commit. Its public API in `include/par2/libpar2.h` is a `Par2Verifier` handle and a
`Par2Observer` callback interface.

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

Upstream's own `CMakeLists.txt` builds the library, pulled into ours with
`add_subdirectory(src/par2 EXCLUDE_FROM_ALL)` and linked as `par2::par2`. Not being the
top-level project, it leaves its tool, tests and install rules off by default, and only
the library is ever built. It probes nothing per instruction set, so a universal2 build
needs no slicing. ParPar is vendored separately, in `src/parpar/`.

`src/par2.cc` calls only the public API in `include/par2/libpar2.h`. The headers under
`src/` are upstream's internals and are not part of its compatibility promise.

### Local patches

None. The vendoring script fails loudly if a patch it carries stops matching, so this
section is the one to check when adding one.
""".format(repo=REPO, ref=ref, commit=commit, date=datetime.date.today().isoformat()))


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
