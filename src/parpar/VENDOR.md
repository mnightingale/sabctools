# Vendored ParPar

| | |
|---|---|
| Upstream | https://github.com/animetosho/ParPar.git |
| Ref | `f4fe1cee823b77da4bb04a8fb122fb0082917518` |
| Commit | `f4fe1cee823b77da4bb04a8fb122fb0082917518` |
| Vendored | 2026-10-11 |

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
