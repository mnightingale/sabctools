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

import argparse
import datetime
import os
import shutil
import subprocess
import sys
import tempfile

REPO = "https://github.com/nzbgetcom/par2cmdline-turbo.git"
REF = "6b6a69942478e4ad0556f5f3eda2cec8c46f3d0e"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "src", "par2")

# Only what we compile, plus upstream's CMake build and the provenance/licence files.
# setup.py drives that CMake rather than reimplementing the per-file SIMD flag matrix.
COPY_TREES = ("src", "include", "cmake", os.path.join("parpar", "gf16"), os.path.join("parpar", "hasher"))
COPY_FILES = ("CMakeLists.txt", "COPYING", "AUTHORS", "ChangeLog", os.path.join("parpar", "gf16.cmake"),
              os.path.join("parpar", "hasher.cmake"))


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def fetch(ref: str, into: str) -> str:
    """Check out any ref - tag, branch or commit - and return the resolved commit."""
    run(["git", "init", "--quiet", into])
    run(["git", "-C", into, "remote", "add", "origin", REPO])

    # A shallow fetch of an exact object works on GitHub and is by far the cheapest,
    # but not every host allows it; fall back to fetching everything.
    try:
        run(["git", "-C", into, "fetch", "--quiet", "--depth", "1", "origin", ref])
    except subprocess.CalledProcessError:
        print("==> Shallow fetch rejected, retrying with full history")
        run(["git", "-C", into, "fetch", "--quiet", "origin"])
        run(["git", "-C", into, "fetch", "--quiet", "--tags", "origin"])

    try:
        run(["git", "-C", into, "checkout", "--quiet", "FETCH_HEAD"])
    except subprocess.CalledProcessError:
        run(["git", "-C", into, "checkout", "--quiet", ref])

    return subprocess.run(
        ["git", "-C", into, "rev-parse", "HEAD"], check=True, capture_output=True, text=True
    ).stdout.strip()


def copy_sources(source: str):
    if os.path.exists(DEST):
        shutil.rmtree(DEST)
    os.makedirs(DEST)

    for tree in COPY_TREES:
        shutil.copytree(os.path.join(source, tree), os.path.join(DEST, tree))
    for name in COPY_FILES:
        target = os.path.join(DEST, name)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy(os.path.join(source, name), target)


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


def patch(path: str, old: str, new: str, description: str):
    """Apply one local patch, and fail loudly if it no longer applies.

    A patch that stops matching means upstream changed underneath us - either it fixed
    the problem itself, in which case delete the patch here, or it moved the code and
    the patch needs rewriting. Silently carrying on is the one thing we must not do.
    """
    full = os.path.join(DEST, path)
    with open(full, encoding="utf-8") as handle:
        content = handle.read()

    if new in content and old not in content:
        raise SystemExit(
            "ERROR: patch '%s' is already present upstream in %s.\n"
            "       Remove it from %s." % (description, path, os.path.basename(__file__))
        )
    if old not in content:
        raise SystemExit("ERROR: patch '%s' no longer matches anything in %s." % (description, path))

    with open(full, "w", encoding="utf-8") as handle:
        handle.write(content.replace(old, new))
    print("==> Patched %s: %s" % (path, description))


def apply_patches():
    # Upstream targets a standalone executable and links the static CRT. A CPython
    # extension must use the dynamic CRT so it shares a heap and a std:: runtime with
    # python3xx.dll, and an explicit add_compile_options(/MT) cannot be overridden by
    # CMAKE_MSVC_RUNTIME_LIBRARY.
    patch(
        os.path.join("cmake", "common.cmake"),
        "add_compile_options(/MTd /Zi /MP /W4 /utf-8)",
        "add_compile_options(/MDd /Zi /MP /W4 /utf-8)",
        "dynamic CRT (debug)",
    )
    patch(
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

`setup.py` drives upstream's own CMake build (`CMakeLists.txt` + `cmake/` + `parpar/*.cmake`)
to produce the `par2-turbo`, `gf16` and `hasher` static libraries, then links them into the
extension. That keeps the ~100-file per-ISA SIMD flag matrix in upstream's hands and builds it
in parallel, rather than serially through distutils.

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
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ref", default=REF, help="tag, branch or commit to vendor (default: %(default)s)")
    parser.add_argument("--repo", default=REPO, help=argparse.SUPPRESS)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        checkout = os.path.join(temporary, "par2")
        print("==> Fetching %s at %s" % (arguments.repo, arguments.ref))
        commit = fetch(arguments.ref, checkout)
        print("==> Resolved to %s" % commit)

        print("==> Replacing %s" % DEST)
        copy_sources(checkout)
        prune()
        apply_patches()
        write_vendor_notes(commit, arguments.ref)

    print("==> Done. src/par2/ now at %s" % commit)
    print("    Review with: git status && git diff --stat")
    print("    Then rebuild: pip install . -v")


if __name__ == "__main__":
    sys.exit(main())
