# Porting guide (workflow M0–M6)

How a title goes from a disc image to a playable Windows port on this toolkit (fork `vulkan-backend`: lifter,
kernel replacement, Vulkan renderer `nv2a_vk`). Written from the THPS 2X port (`thps2x_recomp`)
and the Burnout 3 bench; updated at every milestone. Non-obvious bugs and how to spot them early:
[lessons_learned.md](lessons_learned.md). Storage rules: [storage.md](storage.md). Starter repo:
`templates/game/`.

Conventions used below (`templates/game/tools/paths.ps1`): the toolkit in `<root>\tools\xboxrecomp` (shared deps +
lock in `<root>\tools\private\`), the game repo in `<root>\projects\<game>\<game>_recomp`, generated C and the install
layout in `<root>\projects\<game>\private\` (never committed). Every folder path is free of spaces; no junctions or
links.

## Design principles (maintainer decision A10, 2026-10-15)

Apply to every decision, in this order of priority: **1. resource efficiency** (CPU, RAM, battery, disk),
**2. portability** (Windows, Linux, Android as equal targets), **3. compatibility** across games and XDK builds,
**4. development speed**. Consequences:

- Headless Linux build in CI from now on; Android test build after THPS 2X M4. Every HLE/renderer decision is
  checked for Android (tile GPUs, ARM64) from the start.
- LLE paths (pushbuffer executor, USB/OHCI, MCPX APU) are compile-time optional; a release build drops them once
  the HLE fully covers its game.
- Shaders: SPIR-V generated and cached on disk; glslang compiles only on a cache miss, or is removed entirely. Its
  built-in tables are built once at start-up on a background thread (`RECOMP_GLSLANG_WARM=0` off): ~24 MB RAM for
  the whole run, against an ~85 ms frame at the first compile during play (THPS 2X, M5).
- No settings overlay: settings live in the game's own options menu (plus env-var overrides).
- Dependencies limited to C, SDL3 and Vulkan; a new library needs a written reason.
- No busy-waiting: frame cap, pause and loading screens sleep; V-Sync on by default.
- No duplicate texture copies in host RAM.
- Budgets (RAM, CPU ms/frame, binary size) once the frame-time counter has data; CI warns when a change exceeds them.
- **Renderer policy** (maintainer, 2026-10-17): one renderer path – the NV2A pushbuffer executor translating to
  Vulkan; no D3D8 API-level HLE renderer, renderer cost is an executor optimisation problem. Rule, rationale and
  the only reopen condition: the toolkit's "Renderer policy" (a later, unpublished branch).

**Low-end target (A11, 2026-10-15): "runs on a potato"** – meaning *weak Vulkan 1.3 hardware*. THPS 2X applies
these where cheap, every later port from the start:

- **Vulkan 1.3 is the fixed baseline** (maintainer, 2026-10-16): dynamic rendering, extended dynamic state and
  vertex-input dynamic state are required (`nv2a_vk` uses all three); devices with only Vulkan 1.1/1.2 drivers are
  not supported. No 1.1 fallback and no downgrade work – a port can rely on these features.
- **Mobile texture path:** DXT transcoded once to ETC2/ASTC where BC is missing, cached on disk; RGBA8 only as the
  fallback.
- **Hard caps** for the texture, shader and pipeline caches (RAM and disk); total RAM tracked against a budget.
- **"Low" profile:** native resolution, 30 fps (game logic decoupled), no MSAA.
- **Shader pre-warm** on first start, in the background.
- **Lift/compile only what the game reaches;** release builds strip debug paths and unused XDK code.
- **Two-core design:** game thread + render thread, nothing else busy.
- **Reference low-end set** (paper targets: design guidance, not a test requirement; THPS 2X is measured on its
  dev machine's RTX 3050 Ti only):
  - Windows iGPU: AMD Radeon Vega 7 in the Ryzen 5 5600H (the dev laptop, with its
    RTX 3050 Ti switched off);
  - Linux: Intel HD Graphics 520 (Skylake, e.g. ThinkPad T460) with Mesa ANV (Vulkan 1.3).

**AI assistance (A13):** every repo built on this toolkit says in its README and release notes that it was
created with AI assistance (`templates/game/README.md`).

**Portable install layout (maintainer, 2026-10-17): every port uses this layout – a hard rule, not a choice**
(details and the drive mapping: [`docs/storage.md`](storage.md)):

```
<Game> Recomp/
  <Game>.exe      the recomp
  game/           the extracted disc (installer, from the player's own image)
  saves/          everything a player would miss - never deleted, not even by the runtime
  media/          the player's own media, read only (only if the media module is enabled): boot.mpg here =
                  alternative boot animation (MPEG-1 + MP2, src/bootvideo); every folder below it that holds music
                  = one custom soundtrack
                  (any depth; an .m3u/.m3u8 in it sets the track order)
  data/           everything safe to delete at any time: caches, logs, partition images
```

- Every path is relative to the exe (`xbox_path_init_cfg` with explicit `game_dir`, `save_dir`, `data_dir`).
- **Nothing is written outside this folder:** no AppData, no registry, no temp files left behind.
- **Media on the dev machine** (maintainer, 2026-10-25; every port): the master copy of the maintainer's media is
  `<parent folder>\media\` (never modified or deleted). Test layouts and new game layouts get a **copy** of `media/`
  from it, never a junction or link – a recursive delete or a tool walking the layout would reach the original.
- `E:\TDATA` goes to `saves/` or `data/` per game, by the storage rule (`docs/storage.md`, "TDATA: saves or
  data, per title").
- Checked by `templates/game/tests/outside_writes.ps1` (fails on any write outside the folder during a short run)
  and by the storage rules test (`tests/storage_layout`).

## M0 – toolchain and analysis

- [ ] Start the repo from `templates/game/` (`powershell -File templates\game\new_port.ps1 -Name <name> ...`).
      Fill in `CLAUDE.md`: image, title, push rules, milestones.
- [ ] `tools\setup.ps1`: Python 3.12 + `capstone pytest pefile numpy unicorn`, CMake, Ninja, llvm-mingw, Vulkan
      SDK, junction, reference clones. `tools\deps.ps1`: glslang + SDL3 built with the same clang.
- [ ] Toolkit tests: `py -3 -m pytest tools/` in the toolkit (expect all passed).
- [ ] `tools\m0_analyze.ps1`: extract the disc (`tools.xiso`), parse the XBE (`tools.xbe_parser`) → `docs/xbe.md`:
      title ID (cross-check Xbox-Original-GameList), **XDK build per library** (decides HLE signatures later),
      sections, kernel imports, movie format (Bink/XMV/WMV), music (WMA, custom soundtracks), network.
- [ ] Frame rate and logic coupling: note what the title targets (30/60) – checked properly once it runs (M1+).
      A correct clock at another rate is not proof that the physics is rate-independent: the 120 check under M6
      FRAME RATE measures it.
- [ ] `docs/testplan.md` with every game mode; licensing of everything borrowed.

## M1 – entry point runs, log up to the first crash

- [ ] `tools\lift.ps1` (disc → `<layout>\game`, xbe_parser → disasm (seeds) → func_id → abi_analysis → recomp →
      `private\<game>\gen`). Expect 0 failed functions; note the "unresolved stubs" count.
- [ ] **Gap check from day 1** (see "Improvements"): `py -3 -I tools\scan_table_seeds.py <xbe> <gen> seeds\table_seeds.json`
      – function pointers in data sections that were not lifted – and `py -3 -I tools\gap_check.py <xbe>
      <pipeline\disasm\functions.json> <gen> --report gaps.txt` – every unresolved stub explained. Re-lift.
- [ ] `tools\build.ps1` (≤2 jobs, incremental; full rebuild only after lifter changes).
- [ ] **One build/test lock on a machine with several projects or agents, two classes** (`tools\lock.ps1` in the
      game repos, `Enter-GlobalLock -Class shared|exclusive`):
      - shared: builds, relifts, normal test runs (menu tour, storage, screenshot checks); at most 2 holders, so with
        ≤ 2 compiler processes per build at most 4 on the machine;
      - exclusive: frame-time measurements, benchmarks, baselines, soak tests, emulator oracle comparisons – alone,
        for the whole run;
      - writer priority (a waiting exclusive request blocks new shared holders); open file handles
        (`<root>\tools\private\build.lock` gate + `lock\slot0|slot1`), so a dead holder releases its share; one line per
        holder in `<root>\tools\coordination.md` (who, class, what, since, PID + start time), stale lines removed by the
        next taker and logged in `lock\lock.log`; nested steps see `RC_GLOBAL_LOCK`.
- [ ] First runs with `--max-seconds`: every crash → `RECOMP_ICALL_FEEDBACK` dump → seeds → re-lift. A hang →
      `RECOMP_WATCHDOG_SECS=<s>` (guest stack), `RECOMP_PEEK=` (globals, `[ptr]+off`).
- [ ] Untranslated instructions show as `[UNIMPL]`; fix them in the lifter (never in generated C). CPU feature probes
      (`cpuid`, `pushfd/popfd`) matter: decoders pick code paths from them.
- [ ] Portable layout: `xbox_path_init_cfg` with explicit `game/ saves/ data/` – never AppData.

## M2 – Vulkan window, first pictures

- [ ] `XBOXRECOMP_VULKAN=ON`, `XBOXRECOMP_D3D11=OFF`, `RECOMP_PB_EXEC=1`, `nv2a_vk_install()`.
- [ ] D3D fence: wrap the title's fence wait (`BlockOnTime` / `sub_<fence>`), register `*(dev+<sem>)` with
      `nv2a_pb_set_semaphore_target`; mirror off. Without it D3D hangs after a few frames.
- [ ] Logos/movies visible: `RECOMP_GL_DUMP=<prefix>,<n>` frame dumps, `tools\dump_png.ps1` → 4:3 PNGs; look at them.
- [ ] Wrong pixels in a decoder → state replay (`xbox_StateSnapshot` + `tools.conformance.replay`, Unicorn oracle)
      to bisect the call tree to one function; `--xbe-func` conformance for SIMD helpers.
- [ ] **Frame-time counter from here on** (`RECOMP_FRAME_TIME=1` → `data\frametime.csv`,
      `tools/frametime_summary.py`). The guest is paced at 60 Hz (`RECOMP_GUEST_HZ`) and presents with V-Sync
      (`RECOMP_VSYNC=0` only for benchmarks): check that menus and gameplay run at console speed – one may count
      frames while the other times itself.

## M3 – menus, input, audio

- [ ] Input: XInput at API level (`src/xdk/xdk_xinput.h`). Find the title's XPP entry points (XInitDevices,
      XInputOpen/Close/GetState/SetState/Poll/GetCapabilities, XGetDevices/XGetDeviceChanges) and the
      XDEVICE_TYPE_GAMEPAD record (first entry of the type table passed to XInitDevices); bind the record with
      `xdk_xinput_bind()` and override each function in `recomp_manual.c` to call `xdk_X...()` (keep `sub_X_gen`
      as the fallback). XGetDevices/XGetDeviceChanges stay title code: call `xdk_UpdateDevices()` before them.
      Host pads come from SDL3 on every OS (`src/input`); the window owner inits `SDL_INIT_GAMEPAD`.
      Fallback (`RECOMP_XINPUT_HLE=0`): the title's own XPP driver on the OHCI model (`RECOMP_USB=1`); check its
      controller-port mapping (`XGetDeviceChanges` insertion mask, `RECOMP_USB_NDP`, `RECOMP_USB_PORT`).
- [ ] Split-screen: test with two controllers (`RECOMP_PADS=2` or a `0:p2-connect` script step). A title may
      count only the pad that pressed Start and fall back to a link check (THPS 2X): unlock it at that call site
      only, so System Link stays unavailable.
- [ ] Keyboard: `xbox_InputSetKeymap` (HID scancodes = physical keys), `nv2a_vk_set_key_handler`,
      `RECOMP_KEYBOARD=1`.
- [ ] Automated menu runs: `--input-script` (pad script; `shot=<name>` screenshots, `#` comments; hold presses
      ~300 ms), `--input-live` for exploring. One script per path through the menus, with screenshots at each step.
- [ ] Menus that ignore the pad: `RECOMP_PAD_TRACE` (what `XInputGetState` returns), `RECOMP_USB_REPORT_TRACE`,
      peeks of the handle. Stale state with a moving packet number = the report lands in the wrong memory.
- [ ] Menus that hang: watchdog → is it a D3D wait on a self-built KEVENT? Is it an unresolved ICALL (seed it,
      `"observed": true` if the disassembler drops it)?
- [ ] Menu graphics: `RECOMP_FRAME_TRACE=<flag file>` (one frame of draws: texture format, combiners, fog). A grey
      veil = fog factor wrong.
- [ ] Audio: DirectSound at API level (`src/xdk/xdk_dsound.h`, SDL3 mixer). Find the title's DSOUND entry points
      (the C wrappers adjust the interface pointer: IDirectSound `this-8`, IDirectSoundBuffer `this-0x1C`; the
      stream methods are an XMediaObject vtable reached from the CDirectSoundStream constructor), bind each with an
      override that calls `xdk_X()` and keeps `sub_X_gen`, and `xdk_dsound_bind()` the stream vtables. Wave-file
      media objects stay title code (file I/O only). `RECOMP_DSOUND_TRACE=1` logs calls and buffer formats,
      `RECOMP_DSOUND_WAV=<file>` records the mix (`RECOMP_APU_WAV` the LLE APU's, as an oracle).
      `RECOMP_AC97_READY` only for the LLE fallback (`RECOMP_DSOUND_HLE=0`).
- [ ] Network: the kernel's `PhyGetLinkState` reports no link (fork default; `RECOMP_NET_LINK=1` = link up), so
      XNET's `XNetGetEthernetLinkStatus` returns 0 and the title greys out System Link (THPS 2X: Multiplayer
      greyed with one controller). Input scripts must expect the greyed entry.

## Saves and paths (M3–M5)

- [ ] Decide TDATA: run the menus and one save, look at what lands in `E:\TDATA` → `XBOX_TDATA_SAVES` or
      `XBOX_TDATA_DATA` ([storage.md](storage.md)); write it into the game's CLAUDE.md.
- [ ] Save/load/overwrite scripts; a storage test like THPS 2X `tests/storage.ps1`: save, kill mid-save
      (`RECOMP_SAVE_CRASH_TEST=<file name part>`), restart and load, delete `data\` and load.
- [ ] Memory units: never, also for titles that support them – saves go to the hard disk only (maintainer,
      2026-10-15). Check the title lists the hard disk only, with no error.

## M3.5 – no emulation layer left (lessons from THPS 2X)

- [ ] Lift with `--weak-functions` (every function overridable from `recomp_manual.c` with a rebuild) and
      `--hooks config/hooks.toml` (per-game instruction hooks). Use them for diagnostics too: a census of a library's
      calls (count + time per call) is a strong override per function, no re-lift.
- [ ] LLE paths off in the default build (`XBOXRECOMP_LLE_APU`, `XBOXRECOMP_LLE_USB`); keep a `-Lle` build for
      comparisons.
- [ ] Guest vblank (kernel, `RECOMP_GUEST_HZ`): mirror the title's "frames completed" counter so the title's own
      swap throttle paces it; override its spin-delay helper so GPU waits sleep (`xbox_GuestFlipWait`,
      `xbox_GuestYield`).
- [ ] D3D's kickoff flush spin: hook `recomp_spin_wake()` before the test instruction of each flush loop.
- [ ] **No blocking file / log I/O in kernel-call, DPC, ISR or gate-holding paths** (maintainer's standing rule,
      2026-10-27; every port): output goes through a dedicated logger thread, a lint enforces it. Built on
      a later toolkit branch (not in this snapshot): `cmake/HotPathLogging.cmake` force-includes
      `src/kernel/log_async.h` into xbox_kernel / xbox_usb / xbox_apu - stdio on stderr/stdout there is formatted
      on the calling thread into a bounded ring and written by a logger thread (full ring = the line dropped and
      counted, never a wait); `xbox_log_drain()` before ExitProcess / TerminateProcess / `_exit` / abort;
      `RECOMP_LOG_SYNC=1` = the old synchronous output (use it when chasing a hard crash: without a drain the last
      ~20 ms of lines can be lost); lint `tools/hot_path_io.py`. **A port's own hooks that run inside a kernel
      call, ISR or DPC** (`recomp_manual.c` / `main.c` overrides of the title's code on those paths) follow the
      same rule: no file open / write / flush there - use the redirected stdio of a hot library or the `xbox_log_*`
      calls, and drain before the process ends. This snapshot (`vulkan-backend`, THPS 2X) does not have the logger
      yet; it comes with a later toolkit update.
- [ ] Measure process CPU in an idle menu, per thread. Nothing should spin.
- [ ] Unhandled NV2A methods: triage by "ever set" (`or=` in the executor report) and "read by the back end".
- [ ] Readback: `RECOMP_RT_WATCH=1` before writing any render-target readback.
- [ ] No D3D HLE renderer (renderer policy above): renderer cost goes into the executor. Time the D3D library
      (census) and the executor (frame-time counter) to see where. THPS 2X menus: D3D 0.215 ms + decode/record
      0.34 ms of 2.06 ms.

## M4 – one level, 10 minutes, 60 fps; frame-rate decoupling

- [ ] Find the simulation step: does logic run per frame or per vblank? (SCDA `fpsfixes.cpp`, black-pc,
      burnout3-sw real-time stepping.) Target: uncapped / 120+ fps without the game speeding up.
- [ ] Frame cap, vblank at 60 Hz, present mode (`RECOMP_PRESENT=fifo|mailbox|immediate`; default MAILBOX when the device has it, FIFO the
      fallback, logged as `present mode: <mode> (<reason>)`; a port's frame limiter asks `nv2a_vk_present_fifo()`),
      frame-cap waits through `platform/host_wait.h` (`host_sleep_ns`, `host_wait_until_ns` for exact deadlines),
      frames in flight (`RECOMP_VK_FRAMES=1|2`, default 2). Input-to-present latency: `input_ms` column of
      the frame-time CSV (last pad read by the title → end of its present; XInput HLE). On-screen counter:
      `RECOMP_FPS_HUD=1` (fps + longest frame of the last 0.5 s; transfer blits, never in screenshots).
- [ ] Movies above 30 fps: check whether the title's movie loop already gates `BinkDoFrame` on `BinkWait`
      (THPS 2X does, `sub_001B05E0`); only a title that steps one movie frame per game frame needs the
      XboxXBE-Recomp hold (`runtime/xdk/bink.cpp`).
- [ ] Measure with the frame-time counter; profile hot guest functions (`RECOMP_SAMPLE`, THPS 2X host).
- [ ] Timers and animations that stand still while everything else runs: the title counts vblanks
      (D3D `GetDisplayFieldStatus` VBlankCount, its vblank callback, the vblank event). Answer the count from
      `xbox_DisplayVblankCount` (THPS 2X `recomp_manual.c`, `sub_00013A20`); see `lessons_learned.md`.

**Known toolkit item – real D3D vblank interrupt (`RECOMP_VBLANK=1`, off by default).** Needed only for titles
that *register a vblank callback* (D3D `SetVerticalBlankCallback`, miniport +0x1E4 in XDK 3947) or wait on the
vblank event. Delivery boots (held bits, the title's vblank-handler override calls `xbox_Nv2aVblankTaken`, the
counter mirror stands down), but it is not safe yet: D3D queues flips in its software-method trap handler and
retires them in the vblank DPC, here on two host threads (executor/IRQ thread and timer thread) where the console
runs both at DISPATCH on one CPU – THPS 2X saw "completed" one ahead of "submitted" after ~1500 swaps, which stalls
the swap throttle for good. **To do before enabling it for such a title:** one shared lock (recursive, guest
DISPATCH semantics) around D3D's ISR, its DPC, the vblank handler, the PGRAPH/software-method trap handler and
D3D's busy-waits that call those handlers – the guest lock burnout3-sw has (`xbox_gil_enter`, `t_gil_nopreempt`).

## M5 – everything in the test plan

- [ ] Every item of `docs/testplan.md`; controllers via SDL3 incl. split-screen; widescreen (projection + HUD);
      custom soundtracks; installer (= the exe's start check, below; no separate launcher – maintainer 2026-10-24).
- [ ] **Custom soundtracks** (every port with the dashboard's soundtrack feature): find the title's XAPI soundtrack
      functions (`XFindFirstSoundtrack` / `XFindNextSoundtrack` / `XFindClose` / `XGetSoundtrackSongInfo` /
      `XOpenSoundtrackSong`: the string `%s%04x\%08x.WMA` and the `ST.DB` path lead there) and its WMA decoder create
      (`WmaCreateDecoderEx`) plus the WMA media object vtable; bind them to `src/xdk/xdk_soundtrack.h`
      (`xdk_soundtrack_init(media_dir, songs the title's screen holds)`, `xdk_music_bind`). Playback modes and the
      album choice are the port's (`src/media/media_playlist.h`; THPS 2X `src/soundtracks.c`); song ends in a mode
      that moves on are handled inside the host media object, so the title's stream never ends. A game track inside
      a playlist is the title's own WMA object driven from the host – give it the packet size the title uses.
- [ ] **Installer = the port's first start** (`src/install/game_setup.h`, maintainer 2026-10-24; every port): the
      player puts the extracted disc **or one disc image** (`.iso`/`.xiso`) into `game/`; the exe calls
      `xr_game_setup` before anything else (THPS 2X: `src/main.c` `thps_game_setup`). An image is extracted into
      `game/.extracting/`, **every file checked against its XDVDFS entry** (size + CRC-32 taken while writing and
      again reading it back), moved up into `game/` (`default.xbe` last), then the image is deleted; any mismatch,
      a damaged/truncated image, several images, a nested folder or a full disk → a clear message, the image kept.
      No manifest, no journal (a stopped run starts over).
      **Version rules (maintainer, 2026-10-24):** `default.xbe` is extracted, read back and checked **first**, before
      any other file (fail fast): title ID ≠ the port's → "This is not the `<GAME>` image. Please provide the correct
      game."; the right title ID, SHA-1 not supported → "This `<GAME>` version is not supported. Supported version(s):
      `<LIST>`. Please provide a supported version." Abort, image kept, `game/.extracting/` cleared; the same rules
      for an extracted folder; same text in the log (`RECOMP_NO_DIALOGS=1`: log only).
      **Port config fields** (`xr_game_setup_opts`): `game` (name in the messages), `title_id` (from the XBE
      certificate), `versions[]` = `{ name, xbe_sha1 }` + `version_count`; list the same versions in the port's
      README / CLAUDE.md so players know which image to provide. XISO and full redump (video partition in front)
      both work. **No separate launcher:** the exe's start check is the M5 launcher item; settings go into the
      in-game RECOMP OPTIONS (M6) + the settings file. Progress: `xbox_install_ui` (SDL3 window, `SDL_RenderDebugText`, closing
      it cancels). It touches **only `game/`** – never `media/`, `saves/`, `data/` or anything outside the layout,
      and never the player's original image elsewhere. `RECOMP_NO_DIALOGS=1` for automated tests (no message
      boxes, errors to the log only); `test_free_bytes` (THPS: `RECOMP_SETUP_TEST_FREE_MB`) for the disk-full path;
      every written file is flushed to the disk (`FlushFileBuffers` / `fsync`) before it is read back. Reader: `xdvdfs.c` from takedawn-xboxrecomp (MIT, sp00nz). Test: `tests/game_setup`
      (synthetic image, fault injection).
- [ ] **Aspect-ratio rule (maintainer, 2026-10-16, every port):**
      - **Only 4:3 and 16:9** (maintainer, 2026-10-18; every port, no 21:9 or others). 16:9 applies **only to gameplay**.
      - A game without native 16:9 support keeps menus, loading screens and movies at **4:3, pillarboxed** (never stretched); only gameplay is rendered in the chosen aspect ratio (wider projection/FOV, not stretching).
      - The in-game HUD during gameplay always follows the chosen aspect ratio (maintainer, 2026-10-18; every port):
  anchored to the screen edges of the wider view, never stretched.
      - A game with native 16:9 support (e.g. the Xbox dashboard's widescreen setting) uses its native mode for everything instead.

## M6 – settings in the game's own menu

- [ ] RE the title's options menu; add entries (references: SCDA, black-pc, CoD3Recomp). One settings file in
      `saves\`, env-var override per setting.
- [ ] **Options menu layout (maintainer, 2026-10-18; every port, model: THPS 2X `src/recomp_manual.c`):** the game's own
      Options list gets these lines, inserted where they fit the title's menu:
      - **CONTROLS** – one line opening a sub-list PLAYER 1…n CONTROLS (pads that are not connected greyed) and
        **BUTTON MAPPING** below them (maintainer, 2026-10-25; THPS 2X: the game's per-player screen could not take
        another line): one screen with PLAYER (1…n), DEVICE (the keyboard or that player's controller), RESET TO
        DEFAULT, then the device's bindings page by page. Bindings per device: toolkit `src/input/xinput_bind.h`
        (keyboard by scancode, pads by SDL GUID, own binding wins, default layout still navigates menus; the port
        supplies its settings store and env prefix; `<prefix>_RESET=1` is the emergency exit). Rumble stays the
        game's own setting if it has one.
      - **SOUNDTRACKS** – right under the game's sound settings, which stay stock: **SOUNDTRACK** (GAME MUSIC, COMBINED =
        game music + custom albums, then every custom album from `media/`) and **PLAYBACK MODE** with the 8 modes: play
        songs in order, play album in order, shuffle game music, shuffle album, shuffle all custom albums, shuffle all,
        play all in order, play all custom in order. Kept in the settings file.
      - **RECOMP OPTIONS** – the last line; one list (maintainer, 2026-10-26, built in THPS 2X; the model for
        every port): DISPLAY MODE (FULLSCREEN = exclusive at the display's native mode / BORDERLESS / WINDOWED;
        default BORDERLESS, applies at once), RENDER RESOLUTION, ASPECT RATIO (4:3 / 16:9), MENUS AT 16:9
        (PILLARBOXED / STRETCHED, shown only with 16:9; movies always stay 4:3), ANISOTROPIC FILTER,
        **ANTIALIASING ▸** (a sub-menu with every choice: OFF / FXAA / SMAA / MSAA 2X / MSAA 4X, default FXAA;
        applies at once), **FRAME RATE ▸** (FRAME RATE, VSYNC, FPS COUNTER: OFF / FPS / FPS AND FRAME TIME / FRAME
        TIME), **SHADERS ▸** (the optional slang shaders, if the port has the module), **BOOT ANIMATION ▸ as the
        last line** (SHADERS before it - maintainer, 2026-10-27; a status line - BOOT.MPG FOUND / "NEEDS BOOT MPG IN
        THE MEDIA FOLDER" / "CONVERT BOOT VIDEO TO BOOT.MPG  SEE README" when only a boot.mp4 / mkv is there - then
        ON / OFF, greyed without media/boot.mpg; see "Boot animation" below). Every other line is a value line
        "NAME: VALUE": left / right (or A) steps it and saves it at once; hints sit in the value ("16:9 AFTER
        RESTART", "120 NEEDS 120 HZ"). No RESOLUTION line: the window is dragged to size. Leaving RECOMP OPTIONS
        with a value changed that needs a restart (THPS 2X: ASPECT RATIO, VSYNC and BOOT ANIMATION - the animation
        plays only at a start; maintainer, 2026-10-27) asks once: RESTART NEEDED -> RESTART NOW / LATER (THPS 2X
        `host_restart`: the same arguments without an input script, the environment the process started with).
        Not in it: monitor, field of view, FSR. Toolkit hooks: `nv2a_vk_set_render_scale` /
        `nv2a_vk_render_scale_fits`, `nv2a_vk_set_fps_hud`, `nv2a_vk_set_stretch_43`, `nv2a_vk_aniso`,
        `nv2a_vk_set_display_mode`, `nv2a_vk_set_window_rect` / `nv2a_vk_window_rect`,
        `nv2a_vk_set_antialiasing` / `nv2a_vk_antialiasing_fits`. Automated test runs force WINDOWED
        (`<prefix>_DISPLAY_MODE=windowed`), so a run does not cover the maintainer's screen.
      - **Window** (`nv2a_vk`, every port; maintainer 2026-10-26: "sehr angenehm"): the window has the title's
        aspect ratio – 4:3 → 960x720, 16:9 (`xbox_video_widescreen`) → 1280x720 – and keeps it when dragged
        (`SDL_SetWindowAspectRatio`; maximise / snap are not held). The windowed place and size are saved by the
        port once the window has stood still for about a second (`nv2a_vk_window_rect`, a counter that changes on
        every move / resize; THPS 2X `window_rect` after 60 quiet swaps) and handed back at the next start
        (`nv2a_vk_set_window_rect`; the height is kept, the width follows the aspect ratio; a place whose centre is
        on no display is centred, with a log line). Switching to BORDERLESS / FULLSCREEN and back restores it.
      - **Antialiasing** (`nv2a_vk`, every port; maintainer 2026-10-26): FXAA is the old pass. SMAA is the reference
        implementation (iryoku/smaa, MIT, vendored in `src/nv2a_vk/third_party/smaa`; HIGH preset, three passes at
        the presented surface's size before the present; no new library). MSAA only on the presented surface and
        its depth buffer – render-to-texture targets stay single-sample – resolved at the end of each render
        pass. A failed allocation falls back 4x → 2x → none with a log line; the port reads
        `nv2a_vk_antialiasing_fits` and undoes the saved value. Cost (THPS 2X, RTX 3050 Ti, 16:9, 2026-10-26; all
        at 60 fps): GPU ms at 4.5x render scale off 2.9 / FXAA 3.1 / SMAA 3.8 / MSAA 2x 3.3 / MSAA 4x 3.4, VRAM
        288 / 288 / 369 / 464 / 751 MB; at 1x everything within 0.1 ms.
      - **Shaders** (`nv2a_vk`, every port; spec THPS `docs/shader_module.md`, phase 2 built 2026-10-27; the
        SHADERS menu follows): RetroArch slang presets through **librashader**, `src/nv2a_vk/shader_pass.c`. The
        library is **built from source by the port's deps script** (maintainer 2026-10-27, every port; THPS 2X
        `tools\deps.ps1`): librashader at a pinned commit, `librashader-capi` with `--no-default-features --features
        runtime-vulkan` only, Rust pinned and installed project-local (build time only), target
        `x86_64-pc-windows-gnullvm` so glslang / SPIRV-Cross and the link use the port's llvm-mingw clang;
        `CXXSTDLIB_<target>=static=c++` + `-C target-feature=+crt-static` (a plain `-static` link flag does not
        help) so the DLL imports nothing beyond Windows' own DLLs - check with `llvm-objdump -p`. **Not the
        official Windows build:** it imports `D3DX9_43.dll` (its D3D9 runtime), which Windows does not ship. CMake
        copies the DLL next to the exe; no Rust = no DLL = shader module unavailable (menu greyed with a hint).
        Loaded with `SDL_LoadObject` from `<exe folder>/librashader.*` only (not `librashader_ld.h`: it includes
        `windows.h` and follows the DLL search order), C ABI 2 checked first; the log tells "missing" from "does
        not load". Android later: the same Vulkan-only build bundled in the APK, loaded from the native library
        dir. librashader's own cache is always off
        (`disable_cache`: it writes to `%LOCALAPPDATA%`). A preset is built on a worker thread with
        `libra_vk_filter_chain_create_deferred` into a command pool of its own (the plain create submits to the
        queue and waits on it - a race with the render thread); the render thread submits that set-up and puts the
        chain in place at a flip, the picture unshaded until then (THPS 2X: crt-easymode 1 pass 141-201 ms,
        crt-guest-advanced 12 passes 238-387 ms, under 1.1 ms on the render thread, no extra hitch). The pass
        replaces the FXAA draw / blit: input = the presented surface (after the render scale and SMAA / MSAA),
        output = the swapchain image, viewport = the game rectangle (the chain clears the rest black - the bars
        are never shaded; the FPS counter stays on top); FXAA is skipped while a chain is in place. History is
        cleared when the rectangle or the input size changes (4:3 <-> 16:9, resize). Broken preset / failed
        frame: logged, unshaded, never a crash. Hooks: `nv2a_vk_set_shader_preset`, `nv2a_vk_set_shader_param`,
        `nv2a_vk_shader_status` (OFF / LOADING / ON / FAILED / NO_LIBRARY), `nv2a_vk_shader_library_present`;
        `RECOMP_SHADER_PRESET` overrides. No preset = no pass, no cost.
      - **Shader packs** (`src/shaderpack/`, every port; THPS 2X phase 3, 2026-10-27): the user puts
        `shaders_slang.zip` (and any extra pack) or an extracted folder into `media/shaders/` (read only).
        `shader_pack_start(media/shaders, data/shaders)` at start, on a low-priority thread of its own: each zip
        is extracted once to `data/shaders/<zip name>/` (own central-directory reader + miniz `tinfl`, stored +
        deflate, CRC-checked, no ZIP64; the zip-slip guard `zip_safe_path` refuses absolute paths, `..`, drive
        letters / `:` streams, control characters, Windows device names; one root folder common to every entry
        is stripped), first into `<pack>.partial/` with a list of the files written (`.pack_files`), renamed when
        complete, the marker `.pack_source` (the zip's size + modification time) written last - a cut extraction
        is redone, an unchanged zip is never touched again, a changed one replaces exactly the listed files (no
        recursive delete anywhere). Folders in `media/shaders/` are packs used in place. The preset index
        `data/shaders/index.txt` (pass count per `.slangp`, through `#reference` up to 16 deep; folders named
        `bezel` / `border` left out - they draw outside the picture) is rebuilt only when a pack changed;
        `shader_pack_state` / `shader_pack_index` give it to the menu. Deleting `data/` re-extracts at the next
        start. Unit test `tests/shaderpack` (71 checks, a crafted malicious zip among them).
      - **Icon** (every port; maintainer 2026-10-27): an original, neutral design of the port's own (THPS 2X: a
        plain skateboard) - never the game's logo, cover art, title lettering or publisher / developer branding;
        the maintainer sees it before a release. The port keeps `assets/icon.ico` (16 / 32 / 48 / 256 px) and
        `assets/icon_64.bmp` (32-bit, alpha): the `.ico` goes into the exe as a resource (`game.rc`, `llvm-rc`,
        build layer only), the BMP's bytes are embedded at build time and handed to
        `xbox_set_window_icon(bmp, n)` (kernel.h) before the first window - nv2a_vk's window and the installer's
        progress window apply it with `SDL_SetWindowIcon` on every platform. `templates/game/assets/README.md` is
        the placeholder slot.
      - **Boot animation** (`src/bootvideo/`, every port; THPS 2X 2026-10-27): the player's `media/boot.mpg` -
        **MPEG-1 video + MP2 audio** (MPEG-PS), decoded on the CPU by pl_mpeg (MIT, vendored; no GPU video decode,
        no H.264 / AAC: portable to every driver and Android) - plays before the title and **cannot be skipped**
        (maintainer, 2026-10-27, every port: the boot animation is the one video that is never skippable; the
        title's own movies keep their skip rules; closing the window still quits). The port calls `boot_video_find(media)` (also reports a `boot.mp4` / `boot.mkv` alone: log line + the
        menu's "CONVERT BOOT VIDEO TO BOOT.MPG  SEE README"; **never convert or touch files in `media/`**), then -
        with BOOT ANIMATION on - `nv2a_vk_boot_begin()`, `boot_video_play(path, sink)` with the sink
        `nv2a_vk_boot_frame`, `nv2a_vk_boot_end()`, after `nv2a_vk_install` and before the title's entry. Threads:
        the port's thread decodes and paces (clock-driven `plm_decode`, sleeps between frames, sound through an
        SDL3 audio stream); the **NV2A thread presents** - it owns the window and pumps its events - through
        `g_xbox_nv2a_idle_hook` (kernel.h, called once per pass of its loop): the frame becomes the presented
        surface for one flip, so letterbox, display mode, antialiasing and a shader chain apply. Never present from
        the port's thread: the window would belong to it and the title's flips (NV2A thread) would no longer get
        its events. The README gives the one-time conversion, 720p at 30 fps (a light CPU decode, a small file):
        `ffmpeg -i boot.mp4 -vf scale=-2:720 -r 30 -c:v mpeg1video -q:v 2 -c:a mp2 -b:a 192k boot.mpg`. The NV2A
        thread's hook is set only while the animation plays; afterwards its loop tests a NULL pointer.
        FRAME RATE values (maintainer, 2026-10-24): ORIGINAL (the title's own 25/30 or 50/60), 60 only when the
        original is 25/30, and 120 – no Unlocked / uncapped.
        **120 only after this check** (THPS 2X, 2026-10-26: its session clock was right at 120, yet the skater fell
        1.5-2 % slower and moved 4.5-9 % faster - "time based" was concluded from the clock alone; `lessons_learned.md`):
        (1) game time / clocks exact at the original rate and at 120 (a timed session: clock against the wall clock);
        (2) **physics**: the player's position logged every frame, gravity from parabola fits of the flight phases and
        speed percentiles **over fixed 100 ms windows** (per-frame speeds inflate single-frame snaps at the smaller dt),
        at the original rate and at 120 - two runs per rate as the noise floor, ±2 %; (3) how the
        title turns dt into integer steps (truncation, a precomputed step²) - exact with a carried remainder above the
        original rate, the original rate left bit-identical; (4) replays: record and play back at both rates (playback
        may take one recorded frame per rendered frame); (5) 120 only on a display at 120 Hz or more
        (`nv2a_vk_display_hz`; 144/165 Hz included - the original 60 judders there just as much, maintainer
        2026-10-26), below that the original rate with a log line and a menu hint; the log says whether 120 divides
        the display rate evenly; a test switch forces it.
        **120 Hz display session (maintainer, 2026-10-26; every port):** (1)-(4) run on the 60 Hz panel with the test
        switch. Only at the end, once they pass, the agent **asks the maintainer to connect the 120 Hz display** and
        does everything that needs it in one go: the real display gate (the setting, no test switch), the cost series
        original vs 120 and the maintainer's frame-pacing / adaptive-sync check. Then it says so, and the display is
        disconnected again. Nothing is replugged during that session.
        **PAL60 rule** (maintainer, 2026-10-24; every port): a PAL title that supports PAL60 never runs at 50 Hz in
        the recomp – no 50 Hz option, no 50 Hz override, its PAL variant always answers PAL60 (a 30-fps title:
        30 instead of 25 fps). A PAL title without PAL60 keeps 50 Hz; improved later only by interpolation over
        fixed-step logic, never by a faster game tick.
      Same names, order and value lists in every port unless the title cannot offer an option (e.g. no 30 fps entry
      for a 60 fps game).
- [ ] **RENDER RESOLUTION (maintainer, 2026-10-21; cap 5x 2026-10-23; every port):** a scale factor on the game's
      native lines (e.g. 480), **1.00x–5.00x in 0.25 steps** (17 values), default 1.00x; 1.5x = 720p, 2.25x = 1080p,
      3x = 1440p, 4.5x = 4K (2160 lines), 5x = 2400 lines. **5x is the cap for future ports too** (maintainer,
      2026-10-23: "das sollte für zukünftige Projekte ebenfalls reichen"). Nothing above 5x: THPS 2X on an RTX 3050 Ti held 60 fps
      at 5x 16:9 but missed vblanks at 5.5x and 6x 16:9 (measured 2026-10-23); beyond 4K only edge
      smoothing is left, MSAA/SMAA do that cheaper. The factor is vertical; X follows the aspect ratio: 4:3 → X = factor,
      16:9 gameplay → X = factor × 4/3, menus/movies kept 4:3 pillarboxed → X = factor. The menu shows the factor and
      the resulting resolution, updated with the aspect ratio ("2.25X 1440X1080" / "2.25X 1920X1080"); left/right
      steps through the values; values over the device's image limits are greyed out, not hidden. No VRAM estimate
      (maintainer, 2026-10-26: the player knows what they do): a step whose allocation fails goes back to the last
      good value with a log line, and the port undoes the saved value (`nv2a_vk_render_scale_fits`).
      At 16:9 the menu offers the standard sizes only (854X480 native, 1280X720, 1600X900, 1920X1080, 2560X1440, 3200X1800, 3840X2160, 4267X2400; maintainer 2026-10-26 – the 0.25 grid gave 1707X960); each aspect keeps its own value and switching carries the width over (3X 1920X1440 → 1920X1080). A step to a value the device cannot hold is refused.
      Replaces any "50–200 %" / "20–200 %" entry.

## Tools at a glance

| Tool | Use |
|---|---|
| `tools\lift.ps1`, `tools\build.ps1`, `tools\lock.ps1` (game repo) | lift, build (shared lock, ≤2 jobs) |
| `tools\scan_table_seeds.py` (game repo) | data-section pointers to unlifted functions → seeds |
| `tools/gap_check.py` (toolkit) | every unresolved stub explained (gap / inside / outside), seeds for gaps |
| `RECOMP_DISASM_SKIP`, `RECOMP_DISASM_EXTRA` | bisect / opt into disassembler passes (`alias_closure`, `nop_boundary`) |
| `RECOMP_ICALL_FEEDBACK` | unresolved indirect-call targets → seeds |
| `RECOMP_STUB_LOG=1` | a reached unresolved stub (a silent no-op) logs its address and caller |
| `RECOMP_WATCHDOG_SECS`, `RECOMP_PEEK` | where a hang is, guest globals |
| `RECOMP_GL_DUMP`, `RECOMP_FRAME_TRACE`, pad-script `shot=` | pictures and draw traces |
| `RECOMP_PAD_TRACE`, `RECOMP_INPUT_DIAG` (HLE; `=2`: every port, each state change, polls per port); `RECOMP_USB_REPORT_TRACE`, `RECOMP_USB_TRACE` (LLE) | input chain |
| `RECOMP_HEAP_WATCH=<va>`, `THPS_WATCH_WRITE=<va>` (host DR0–3 watchpoint) | who owns / who writes memory |
| `RECOMP_NULL_GUARD` (default on; `=0` off, `=log` log every access and resume) | guest page 0 is no-access: a null dereference stops with a `[NULLPTR]` line (access, host pc, guest caller) |
| `RECOMP_HEAP_RECLAIM=0` | old never-reuse heap/contiguous allocator (reclaim is the default); an exhausted contiguous arena is `[FATAL]` either way |
| `RECOMP_MEM_LOG=<s>` | `[MEM]` line every s seconds: contiguous and heap high-water mark and live bytes (leak check in soak tests) |
| `RECOMP_DEBUG=1` | renderer debug lines: surfaces / depth buffers dropped because their contiguous block was freed (`[VK] surface ... dropped: block ... freed`) |
| state replay + `tools.conformance` | lifted vs original machine code, byte-exact |
| `tests/storage_layout` | storage rules |

## Improvements for the next port

Things that cost THPS 2X days and should be done up front:

1. **HLE first.** API-level DirectSound / XInput / D3D8 (signatures per XDK build) instead of bringing up the LLE
   paths (USB driver on OHCI, MCPX APU, pushbuffer executor) title by title. LLE stays the fallback.
2. **Lifter gap check from day 1.** Before the first run: `tools/gap_check.py` on the lift (should leave only
   data-decoded stubs) plus the table scan for code pointers. THPS 2X found five such gaps one hang at a time (XPP class drivers,
   Bink callback, vtables at 0x0011F760 and 0x00144FB0).
3. **Frame-time counter from M2.** Game logic / pushbuffer or HLE translation / GPU, CSV in `data\` – so
   "faster" and "slower" are numbers from the start.
4. **Screenshots in every scripted run** (`shot=`), contact sheets per run; look at them – a file existing proves
   nothing.
5. **Physical addresses:** any bus master (USB, APU, GPU) must resolve guest physical addresses by owner, not by
   range ([lessons_learned.md](lessons_learned.md)).
6. **Log hygiene:** repeated messages rate-limited per key from the start; a 1 GB log hides the one line that
   matters.
7. **Low-end from day 1 (A11):** Vulkan 1.3 baseline (weak 1.3 hardware is the low end), ETC2/ASTC texture path, cache caps, "low" profile,
   shader pre-warm, reached-code-only builds, game thread + render thread (Design principles).
8. **From XboxXBE-Recomp (A12; TGP482, MIT – keep the attribution in every borrowed file):**
   - per-game TOML config for functions, switch tables and data ranges, instead of seeds spread over JSON files
     and `recomp_manual.c`;
   - lifter coverage: gap discovery, every jump-table form, C++ exceptions, x87 precision, LTCG libraries;
   - incremental, partitioned code generation (a changed function rebuilds its partition, not the title);
   - a renderer extension API (game patches hook draws without forking the renderer);
   - `xbe2pe` export for Ghidra; a frame-capture key; XBE image switching (multi-XBE titles).
   THPS 2X takes from it already (M3.5/M4): overridable recompiled functions (weak symbols) plus per-game
   instruction hooks, the Bink fix above 30 fps, host CRT math.
