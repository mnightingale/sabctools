# libpar2 probes

The harnesses behind [../libpar2-findings.md](../libpar2-findings.md). Each includes
`<par2/libpar2.h>` and nothing else, and links `libpar2.a` alone.

They are kept building against the vendored header, so they now run against the fixed
API rather than the one the findings were written from: the crash does not crash, a
basepath without its trailing separator is accepted, and the mapping finding 7 wanted is
there to print. Re-run them after a re-vendor to see whether any of it has moved.

## Build

`libpar2.a` comes from sabctools' own build of the vendored tree:

```bash
cmake -S . -B build -G Ninja -DPython_EXECUTABLE=$(which python3)
cmake --build build --target par2
c++ -std=c++17 -g -I src/par2/include docs/libpar2-probes/crash.cc build/libpar2.a -o /tmp/crash
```

## Run

Every one takes a PAR2 file and a basepath, in that order.

```bash
# a working set to operate on
mkdir -p /tmp/w && cp tests/par2files/* /tmp/w/

# 1. Repair() after eRepairNotPossible - used to segfault, now answers
rm /tmp/w/beta.bin                      # 640 blocks needed, 576 available
/tmp/crash /tmp/w/rec.par2 /tmp/w/

# 2. basepath with and without the trailing separator - both correct now
/tmp/crash /tmp/w/rec.par2 /tmp/w
/tmp/crash /tmp/w/rec.par2 /tmp/w/

# 3, 4, 5. AddPar2File contract, progress values, OnFile scope
/tmp/probe missing-par2      /tmp/w/rec.par2 /tmp/w/
/tmp/probe progress          /tmp/w/rec.par2 /tmp/w/
/tmp/probe verify            /tmp/w/rec.par2 /tmp/w/
/tmp/probe getinfo-before-add /tmp/w/rec.par2 /tmp/w/

# 6 and the SetKnownBlocks / Reassess checks
/tmp/flows reassess    /tmp/w/rec.par2 /tmp/w/rec.vol000+576.par2 /tmp/w/
/tmp/flows knownblocks /tmp/w/rec.par2 /tmp/w/rec.vol000+576.par2 /tmp/w/

# 7. rename detection, and the mapping GetRenamedFiles now returns
mv /tmp/w/gamma.bin /tmp/w/9f3ac1b7e2.dat
/tmp/ren /tmp/w/rec.par2 /tmp/w/ /tmp/w/9f3ac1b7e2.dat

# 10. the recovery block count, before and after a verify
/tmp/flows blocks /tmp/w/rec.par2 /tmp/w/rec.vol000+576.par2 /tmp/w/
```

`probe` and `flows` take a scenario name first; `crash` and `ren` do not.
