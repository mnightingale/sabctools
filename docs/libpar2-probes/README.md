# libpar2 probes

The harnesses behind [../libpar2-findings.md](../libpar2-findings.md). Each includes
`<par2/libpar2.h>` and nothing else, and links `libpar2.a` alone.

## Build

`libpar2.a` comes from sabctools' own build of the vendored tree:

```bash
cmake -S . -B build -G Ninja -DPython_EXECUTABLE=$(which python3)
cmake --build build --target par2
c++ -std=c++17 -g -I src/par2/include docs/libpar2-probes/crash.cc build/libpar2.a -o /tmp/crash
```

## Run

Every one takes a PAR2 file and a basepath. **The basepath needs its trailing separator**
- that is finding 2, and leaving it off makes every scenario report the set as missing.

```bash
# a working set to operate on
mkdir -p /tmp/w && cp tests/par2files/* /tmp/w/

# 1. Repair() after eRepairNotPossible - segfaults
rm /tmp/w/beta.bin                      # 640 blocks needed, 576 available
/tmp/crash /tmp/w/rec.par2 /tmp/w/

# 2. basepath with and without the trailing separator
/tmp/crash /tmp/w/rec.par2 /tmp/w       # everything reported missing
/tmp/crash /tmp/w/rec.par2 /tmp/w/      # correct

# 3, 4, 5. AddPar2File contract, progress values, OnFile scope
/tmp/probe missing-par2      /tmp/w/rec.par2 /tmp/w/
/tmp/probe progress          /tmp/w/rec.par2 /tmp/w/
/tmp/probe verify            /tmp/w/rec.par2 /tmp/w/
/tmp/probe getinfo-before-add /tmp/w/rec.par2 /tmp/w/

# 6 and the SetKnownBlocks / Reassess checks
/tmp/flows reassess    /tmp/w/rec.par2 /tmp/w/rec.vol000+576.par2 /tmp/w/
/tmp/flows knownblocks /tmp/w/rec.par2 /tmp/w/rec.vol000+576.par2 /tmp/w/

# 7. rename detection, and what is not retrievable afterwards
mv /tmp/w/gamma.bin /tmp/w/9f3ac1b7e2.dat
/tmp/ren /tmp/w/rec.par2 /tmp/w/ /tmp/w/9f3ac1b7e2.dat
```

`probe` and `flows` take a scenario name first; `crash` and `ren` do not.
