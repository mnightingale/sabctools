# Vendored par2cmdline-turbo

| | |
|---|---|
| Upstream | https://github.com/nzbgetcom/par2cmdline-turbo.git |
| Ref | `6b6a69942478e4ad0556f5f3eda2cec8c46f3d0e` |
| Commit | `6b6a69942478e4ad0556f5f3eda2cec8c46f3d0e` |
| Vendored | 2026-08-02 |

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
