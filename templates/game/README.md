# Game port template

Starter repo for a new port on this toolkit fork. Create a port:

```
powershell -ExecutionPolicy Bypass -File templates\game\new_port.ps1 -Dest "<root>\projects\mygame\mygame_recomp" `
    -Title "My Game" -Name mygame -Exe MYGAME -Prefix MYGAME -Region USA `
    -Iso "<root>\roms\xbox\My Game (USA).iso" [-XbeSha1 <sha1>] [-TitleId <8 hex>]
```

Placeholders replaced: `@TITLE@ @NAME@ @EXE@ @PREFIX@ @REGION@ @ISO@ @XBESHA1@ @TITLEID@`. Then follow
`docs/porting_guide.md` from M0. What is in here comes from the THPS 2X port and is kept generic:

| Path | What |
|---|---|
| `CLAUDE.md` | rules, milestones, portable layout, storage and push rules (fill in the TODOs) |
| `CMakeLists.txt` | game exe against the toolkit (Vulkan, no D3D11), generated C from `private\<name>\gen` |
| `tools/setup.ps1`, `deps.ps1` | machine setup, glslang + SDL3 with llvm-mingw |
| `tools/m0_analyze.ps1` | disc + XBE analysis → `docs/xbe.md` |
| `tools/lift.ps1`, `build.ps1` | lift (seeds/*.json), build (lock, ≤2 jobs) into the install layout |
| `tools/scan_table_seeds.py` | lifter gap check: data-section pointers to unlifted functions |
| `tools/dump_png.ps1` | frame dumps / screenshots → 4:3 PNG |
| `src/main.c` | boot sequence with storage config, input, Vulkan |
| `src/platform/` | host layer: UTF-8 paths + wide APIs, log with repeat collapsing, crash handler, watchpoints, `--input-script` |
| `src/input.c` | keyboard layout (HID scancodes) |
| `src/recomp_manual.c` | ICALL diagnostics, unimplemented-instruction log, place for overrides |
| `tests/input/` | input scripts (`shot=` screenshots) |
