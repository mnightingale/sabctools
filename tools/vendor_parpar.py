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
Re-vendor src/parpar/ from ParPar.

    python tools/vendor_parpar.py                # the pinned ref below
    python tools/vendor_parpar.py --ref master   # a tag or branch
    python tools/vendor_parpar.py --ref f4fe1ce  # or a commit

Leaves the result in the working tree for review; does not commit.
"""

import datetime
import os
import sys
import tempfile

import vendor_common

REPO = "https://github.com/animetosho/ParPar.git"
REF = "f4fe1cee823b77da4bb04a8fb122fb0082917518"

DEST = os.path.join(vendor_common.ROOT, "src", "parpar")

# The GF16 engine, the hashing engine, and the headers both share. Everything else
# upstream is the Node.js module and its tooling.
COPY_TREES = ("gf16", "hasher", "src")
COPY_FILES = ("README.md",)

# src/ also holds the Node.js binding, which is not built here
PRUNE = ("src/gf.cc",)


def apply_patches():
    """No local patches.

    Kept as the place to add one, and vendor_common.patch() fails loudly once a patch
    stops matching.
    """


def write_vendor_notes(commit: str, ref: str):
    with open(os.path.join(DEST, "VENDOR.md"), "w", encoding="utf-8") as handle:
        handle.write("""# Vendored ParPar

| | |
|---|---|
| Upstream | {repo} |
| Ref | `{ref}` |
| Commit | `{commit}` |
| Vendored | {date} |

[animetosho/ParPar](https://github.com/animetosho/ParPar) is a PAR2 creator whose GF16
multiplication and MD5/CRC32 hashing engines pick the fastest kernels the CPU supports at
run time. Only those engines are vendored: `gf16/`, `hasher/` and the headers they share in
`src/`.

## Licensing

ParPar is Public Domain / CC0, compatible with sabctools' GPL-2.0-or-later.

## Updating

```bash
python tools/vendor_parpar.py --ref <tag, branch or commit>
```

Then review the diff and rebuild. Update the `REF` default in `tools/vendor_parpar.py` to
match, so a plain re-run reproduces the same tree.

## How it is built

`cmake/parpar.cmake` compiles the engines into the `parpar` static library, giving each group
of kernels the flags for the instruction set it targets when the compiler accepts them. A
kernel whose flags are refused is compiled without them and its guards leave it empty.

`src/par2_parpar.cc` puts par2's `Processor` and `Hasher` interfaces on these engines, and the
par2 glue hands them to every verifier through `par2::Backends`. par2 itself is vendored
separately, in `src/par2/`, so either can be updated on its own.

### Local patches

None. The vendoring script fails loudly if a patch it carries stops matching, so this
section is the one to check when adding one.
""".format(repo=REPO, ref=ref, commit=commit, date=datetime.date.today().isoformat()))


def main():
    arguments = vendor_common.parse_args(__doc__, REF, REPO)

    with tempfile.TemporaryDirectory() as temporary:
        checkout = os.path.join(temporary, "parpar")
        print("==> Fetching %s at %s" % (arguments.repo, arguments.ref))
        commit = vendor_common.fetch(arguments.repo, arguments.ref, checkout)
        print("==> Resolved to %s" % commit)

        print("==> Replacing %s" % DEST)
        vendor_common.copy_sources(checkout, DEST, COPY_TREES, COPY_FILES)
        for path in PRUNE:
            os.remove(os.path.join(DEST, path))
        apply_patches()
        write_vendor_notes(commit, arguments.ref)

    print("==> Done. src/parpar/ now at %s" % commit)
    print("    Review with: git status && git diff --stat")
    print("    Then rebuild: pip install . -v")


if __name__ == "__main__":
    sys.exit(main())
