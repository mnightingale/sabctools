# Vendored par2cmdline

| | |
|---|---|
| Upstream | https://github.com/mnightingale/par2cmdline.git |
| Ref | `f141857fdf59372449f5419ea3cafef5be7d310a` |
| Commit | `f141857fdf59372449f5419ea3cafef5be7d310a` |
| Vendored | 2026-08-31 |

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
