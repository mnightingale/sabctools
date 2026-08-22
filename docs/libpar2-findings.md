# libpar2 findings

Notes from porting sabctools' par2 glue onto the public `libpar2.h` API, with a
reproduction for each. Three look like bugs, three are behaviours that differ from what
the header says, and three are things the API does not currently expose that this
consumer needs.

## Environment

| | |
|---|---|
| Fork | `mnightingale/par2cmdline` |
| Commit | `138c411357df2e31f6f5303225667165fdba4ec4` (tip of `libpar2/backup-files`) |
| Built by | `cmake/par2.cmake` in sabctools -> `libpar2.a`, C++17, `-O3` |
| Host | macOS 26, arm64, AppleClang, **no OpenMP** (AppleClang ships no libomp) |
| Fixtures | `tests/par2files/` - alpha.bin (960 blocks), beta.bin (640), gamma.bin (320); blocksize 64; 1920 data blocks; 576 recovery blocks |

Every harness below includes `<par2/libpar2.h>` and nothing else, and links `libpar2.a`
alone. That part works: the public header is self-contained and the library needs no
internal headers to use.

---

## Bugs

### 1. `Repair()` after `eRepairNotPossible` segfaults

The worst of these. If a verify concludes there is not enough recovery data, calling
`Repair()` anyway crashes rather than returning an error. Nothing in the header says
`Repair` may only be called after `eRepairPossible`.

Delete a file needing more blocks than the set can rebuild (640 needed, 576 available):

```
step: AddPar2File
  = 0
step: Verify
  = 2                       <- eRepairNotPossible
  complete=2 renamed=0 damaged=0 missing=1 missingblocks=640
step: Repair
  = [SIGSEGV]
```

```
thread #1, stop reason = EXC_BAD_ACCESS (code=1, address=0x240)
  frame #0: Par2::Par2Repairer::ComputeRSmatrix() + 676
```

Control, identical code path with damage that *is* repairable (1 block missing):

```
  Verify = 1                <- eRepairPossible
  Repair = 0                <- eSuccess
```

So it is specifically the not-possible case. `ComputeRSmatrix` presumably walks an input
block list that `Repair` never populated. Returning `eRepairNotPossible` (or
`eLogicError`) from `Repair` would be enough.

### 2. `basepath` must end with a path separator, and silently misbehaves otherwise

`Verify(basepath, ...)` and `Repair(basepath)` appear to concatenate `basepath` and the
filename directly. Given `/tmp/w` and `beta.bin` the result is `/tmp/wbeta.bin`, so every
file is reported missing - with no error, just a wrong answer.

Same intact set, same call, the only difference being a trailing `/`:

```
basepath = "/tmp/w"
  Verify -> 2   complete=0 damaged=0 missing=3 avail=0    missingblocks=1920

basepath = "/tmp/w/"
  Verify -> 0   complete=3 damaged=0 missing=0 avail=1920 missingblocks=0
```

This is easy to hit: `os.path.dirname(parfile)` in Python and `dirname(3)` in C both
return the form *without* the separator, so the natural way to derive a basepath is the
broken one. A caller has no way to tell this apart from genuinely missing data.

Worth either appending a separator inside the library when one is absent, or saying so
in the header. Silently reporting an intact set as entirely missing is the bad outcome.

### 3. `AddPar2File` returns `eSuccess` for a file that does not exist

The header says:

> Returns eFileIOError if the named file does not exist.

That holds only when the directory has no other PAR2 files:

```
# empty directory
  AddPar2File("/tmp/empty/nothing.par2") = 6     <- eFileIOError, as documented

# directory containing rec.par2 + rec.vol000+576.par2
  AddPar2File("/tmp/w/rec.par2.nope")    = 0     <- eSuccess
     ...having fired OnFile(rec.vol000+576.par2)
```

The named file is absent in both cases. In the second, the sibling search finds other
volumes and the call reports success, so a caller cannot detect a mistyped or
not-yet-downloaded filename. Either the documented contract or the behaviour needs to
move.

---

## Behaviour that differs from the header

### 4. `OnProgress` resets per file, not per operation

Documented as:

> Progress through the current operation, in thousandths

which reads as a single 0..1000 ramp. The values for one three-file verify, in order:

```
3 1000 1 1000 1 1000 2 3 1000
```

and for the repair that followed:

```
1 1000 2 1000
```

So it is per file. A consumer wiring this straight to a progress bar gets one that
reaches 100% and restarts once per file. Either is fine to implement against - but the
caller has to know which, and there is no per-file total in the callback to normalise
against.

### 5. `OnFile` also fires for PAR2 files, during `AddPar2File`

`OnFile` is documented as "Work has started on this file". It fires for the PAR2 files
being read as well as for the data files being verified, so the counts do not pair up:

```
  AddPar2File:  OnFile(rec.par2), OnFile(rec.vol000+576.par2)
  Verify:       OnFile(alpha.bin) OnFileDone(alpha.bin, 960/960)
                OnFile(beta.bin)  OnFileDone(beta.bin, 640/640)
                OnFile(gamma.bin) OnFileDone(gamma.bin, 320/320)

  totals: OnFile=5  OnFileDone=3
```

Reasonable behaviour, but an observer that displays "verifying <name>" will show PAR2
filenames while loading. There is no flag distinguishing the two, so the only way to tell
them apart is to know which library call is currently running.

### 6. `AddPar2File` pulls in sibling volumes by itself

Adding only the index file also loads the volume files sitting beside it. After
`AddPar2File("rec.par2")` alone, the first verify already reports `recovery=576`, all of
it from `rec.vol000+576.par2` which was never named.

This is what par2 has always done, and for a one-shot call it is what you want. It does
mean a caller adding files one at a time cannot control which recovery data is in play -
relevant to the "verify, fetch more blocks, `Reassess`" flow, where the point is to
verify with what is on hand and add more later. Worth a sentence in the header.

---

## Not exposed, and needed here

### 7. Which file was renamed to what

Only the count is available. par2 detects and applies the rename, but the mapping is not
retrievable. Renaming `gamma.bin` to `9f3ac1b7e2.dat` and passing it as an extra file:

```
  Verify(extrafiles=1) = 1
  complete=2 renamed=1 damaged=0 missing=0     <- detected
  Repair = 0
  GetBackupFiles count=0                       <- correctly excludes a caller's extra file
  GetFileInfo still reports set names only:
    beta.bin
    gamma.bin
    alpha.bin
```

`9f3ac1b7e2.dat` is gone from disk afterwards and `gamma.bin` is in its place, but
nothing in the API says so. SABnzbd needs the pairing twice over: to tell the user which
of their files was renamed, and to avoid deleting a file par2 has just renamed while
cleaning up leftovers.

A `GetRenamedFiles(std::vector<std::pair<std::string, std::string>> *)` alongside
`GetBackupFiles` would fit the existing shape.

### 8. Where a file actually lives

`Par2FileInfo` carries what the set records - name, size, block count, hashes - but not
the local path. Callers that want to open a file themselves have to rebuild it from
`basepath` plus `filename`, which is exactly the concatenation that finding 2 shows is
easy to get wrong, and it assumes the library does nothing else to the path.

sabctools needs it to read files for quick verify, and to tell a set's own targets apart
from extra files it supplied. A `localfilename` on `Par2FileInfo` would settle it.

### 9. Skipping the post-repair verification pass

After `SetKnownBlocks` has vouched for a file, par2 still re-reads and re-hashes
everything it rebuilt. Where the caller has already checksummed the data on the way in,
that pass can be most of the wall time of a repair. There is currently no way to turn it
off - it is not a property of the set but a choice the caller should make, so something
like `SetVerifyAfterRepair(bool)` on the verifier.

### 10. How many recovery blocks have been loaded, before verifying

`recoveryblockcount` lives on `Par2VerifyResult`, and `GetVerifyResult` returns false
until something has been verified. So between `AddPar2File` and the first `Verify` there
is no way to ask how much recovery data the set now holds:

```
  AddPar2File("rec.par2")      = 0
  GetSetInfo   blocks=1920 files=3 blocksize=64      <- available
  GetVerifyResult                                    -> false
  AddPar2File("rec.vol000+576.par2") = 0
  GetVerifyResult                                    -> still false
  Verify()                     = 0
  GetVerifyResult  recoveryblockcount=576            <- only now
```

The count is not a property of the verify: it is a property of the packets that have
been read, exactly like `datablocks` and `blocksize`, and it is known the moment they
are. Two places need it before a scan has happened. SABnzbd logs what a set is worth as
soon as it is loaded, and the "fetch more blocks and retry" loop adds par2 files that
arrived late and asks what it now has - which on this API reads as zero until a verify
runs, even though the blocks are loaded and counted.

`Par2SetInfo` already describes "what a PAR2 set describes, known once its packets have
been loaded", so a `recoveryblocks` field on it would say this in the place the header
already promises it.

---

## Things that behave exactly as documented

Checked because this consumer depends on them:

- **`SetKnownBlocks`** does what it says, trust caveat included. Vouching for every block
  of a damaged file makes the set report as complete; passing an empty vector for that
  file forgets it and the next verify finds the damage again.
- **`Reassess`** returns an updated verdict without re-reading the data files.
- **Repeated `Verify`** on one object starts a fresh pass, as the header promises.
- **`GetSetInfo` / `GetFileInfo` / `GetVerifyResult`** all return false before there is
  anything to report, and `Reassess` before any verify returns `eLogicError` (7).
- **`GetBackupFiles`** lists the `.1` files a repair renamed out of the way, and excludes
  files the caller supplied as extra files.
