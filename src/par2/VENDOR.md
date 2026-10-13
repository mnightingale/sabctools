# Vendored par2cmdline

| | |
|---|---|
| Upstream | https://github.com/Parchive/par2cmdline.git |
| Ref | `15de89856bf6aee2c8c2896c13a891ecd55efa78` |
| Commit | `15de89856bf6aee2c8c2896c13a891ecd55efa78` |
| Vendored | 2026-10-30 |

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
