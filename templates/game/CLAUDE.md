# @TITLE@ Recomp – Rules for Agents and Contributors

Static recompilation of *@TITLE@* (Xbox, @REGION@) on the xboxrecomp fork (`vulkan-backend`).
Goal: playable on Windows, **Vulkan only** (same base for later Linux/Android ports).
This repo holds only game-specific code; everything generic belongs in the toolkit.
Workflow and pitfalls: toolkit `docs/porting_guide.md`, `docs/lessons_learned.md`, `docs/storage.md` – link
there instead of repeating them. Handoffs go in `docs/status.md`: **read it before every task, add an entry
after** (`date | agent | done | evidence | open / request`).

## Design principles (toolkit, in priority order)

1. Resource efficiency (CPU, RAM, battery, disk) 2. portability (Windows, Linux, Android equal)
3. compatibility across games and XDK builds 4. development speed. Consequences (no settings overlay, deps only
C/SDL3/Vulkan, no busy-waiting, LLE compile-time optional, ...): toolkit `docs/porting_guide.md`.

## Layout and sources

```
<parent folder>\
  roms\xbox\       the maintainer's disc images – read only, never modify, move or rename (@ISO@)
  projects\
    @NAME@_recomp\ this repo
    xboxrecomp\    toolkit fork (branch vulkan-backend)
    refs\          read only – never modify; reference builds only out of tree
    private\       local inputs and outputs: extracted disc, generated C, install layout, logs (never committed)
```

Paths contain spaces: always quote them. Builds run via the junction `C:\rc` → `<parent folder>\projects`.
**Search first, then build:** search `refs/` and the toolkit history before implementing or debugging.
Borrowed code: origin (repo, commit, file) in the header comment and in `docs/sources.md`.

## Milestones (current one at the top of `docs/status.md`)

| # | Done when |
|---|---|
| M0 | Toolchain works; XBE analysis in `docs/xbe.md` (XDK build per library, title ID, movie format, frame rate); `docs/testplan.md` |
| M1 | Entry point runs, log up to the first crash; gap check run |
| M2 | Vulkan window, logos/movies visible; frame-time counter in place |
| M3 | Menus usable (keyboard + `--input-script`), audio works, saves/paths per `docs/storage.md` |
| M4 | One level 10 minutes at 60 fps; game logic decoupled from the frame rate (target 120 fps; options Original / 60 for 25-30 fps titles / 120, no uncapped) |
| M5 | Every item in `docs/testplan.md`; controllers, widescreen, soundtracks, installer (the exe's start check = the launcher item; settings in the in-game RECOMP OPTIONS + settings file, no separate launcher) |
| M6 | All settings in the game's own options menu |

## Hard rules

- **AI assistance is stated** in the README and in every release note (toolkit rule A13).
- **Fix bugs in the lifter, never in generated C.** Workarounds go in `src/recomp_manual.c` as overrides with a
  comment: why, address, how it was verified.
- **Never commit or distribute:** game data, XBE, generated C, disassembly, dumps, saves, private paths, binaries.
- **Push only to the maintainer's own repos.** No pull requests, issues or comments in foreign repos.
- **License:** GPL-3.0-or-later. MIT/BSD/GPL-or-later code is fine; GPL-2.0-only or unclear: reference only.
- "Fixed" only with evidence (test run, log excerpt, screenshot) in `docs/status.md`. Look at screenshots.
- Visuals and game feel are decided by a human: save a screenshot and ask.
- Commit small and often; push at the end of every session.

## Features

- **Aspect ratios other than 4:3 (maintainer rule, every port; toolkit `docs/porting_guide.md`, M5):**
  - **Only 4:3 and 16:9** (maintainer, 2026-10-18; every port, no 21:9 or others). 16:9 applies **only to gameplay**.
  - A game without native 16:9 support keeps menus, loading screens and movies at **4:3, pillarboxed** (never stretched); only gameplay is rendered in the chosen aspect ratio (wider projection/FOV, not stretching).
  - The in-game HUD during gameplay always follows the chosen aspect ratio (maintainer, 2026-10-18; every port):
  anchored to the screen edges of the wider view, never stretched.
  - A game with native 16:9 support (e.g. the Xbox dashboard's widescreen setting) uses its native mode for everything instead.

- **Options menu layout (maintainer, 2026-10-18; every port, model: THPS 2X `src/recomp_manual.c`):** the game's own
  Options list gets these lines, inserted where they fit the title's menu:
  - **CONTROLS** – one line opening a sub-list PLAYER 1…n CONTROLS (pads that are not connected greyed) and
    **BUTTON MAPPING** below them: one screen with PLAYER, DEVICE (keyboard or that player's controller), RESET TO
    DEFAULT, then the bindings (toolkit `src/input/xinput_bind.h`; `<prefix>_RESET=1` emergency exit). Rumble
    stays the game's own setting if it has one.
  - **SOUNDTRACKS** – right under the game's sound settings, which stay stock: **SOUNDTRACK** (GAME MUSIC, COMBINED =
    game music + custom albums, then every custom album from `media/`) and **PLAYBACK MODE** with the 8 modes: play
    songs in order, play album in order, shuffle game music, shuffle album, shuffle all custom albums, shuffle all,
    play all in order, play all custom in order. Kept in the settings file.
  - **RECOMP OPTIONS** – the last line; one list (maintainer, 2026-10-26, built in THPS 2X; the model for
    every port): DISPLAY MODE (FULLSCREEN = exclusive at the display's native mode / BORDERLESS / WINDOWED;
    default BORDERLESS, at once), RENDER RESOLUTION, ASPECT RATIO (4:3 / 16:9), MENUS AT 16:9 (PILLARBOXED /
    STRETCHED, shown only with 16:9; movies always stay 4:3), ANISOTROPIC FILTER, **ANTIALIASING ▸** (a sub-menu
    with every choice: OFF / FXAA / SMAA / MSAA 2X / MSAA 4X, default FXAA, at once), **FRAME RATE ▸** (FRAME RATE,
    VSYNC, FPS COUNTER: OFF / FPS / FPS AND FRAME TIME / FRAME TIME), **SHADERS ▸** (if the port has the shader
    module), BOOT ANIMATION ▸ as the last line (status line BOOT.MPG FOUND / "NEEDS BOOT MPG IN THE MEDIA FOLDER" /
    "CONVERT BOOT VIDEO TO BOOT.MPG  SEE README", ON / OFF greyed without media/boot.mpg; toolkit src/bootvideo;
    a change asks RESTART NEEDED). Every other line is a value line "NAME: VALUE": left / right (or
    A) steps it and saves it at once; hints sit in the value ("16:9 AFTER RESTART", "120 NEEDS 120 HZ"). No
    RESOLUTION line: the window is dragged to size. Leaving RECOMP OPTIONS with a value changed that needs a
    restart asks once: RESTART NEEDED -> RESTART NOW / LATER (THPS 2X `host_restart`: the same arguments
    without an input script, the environment the process started with). Not in it: monitor, field of view,
    FSR. Toolkit hooks: `nv2a_vk_set_render_scale`, `nv2a_vk_set_fps_hud`, `nv2a_vk_set_stretch_43`,
    `nv2a_vk_aniso`, `nv2a_vk_set_display_mode`, `nv2a_vk_set_window_rect` / `nv2a_vk_window_rect`,
    `nv2a_vk_set_antialiasing` / `nv2a_vk_antialiasing_fits` (porting guide M6). Automated runs force WINDOWED.
  - **Window** (`nv2a_vk`, every port; maintainer 2026-10-26: "sehr angenehm"): the window has the title's
    aspect ratio – 4:3 → 960x720, 16:9 (`xbox_video_widescreen`) → 1280x720 – and keeps it when dragged
    (`SDL_SetWindowAspectRatio`; maximise / snap are not held). Its place and size are saved and restored at the
    next start (off-screen → centred).
  - **Antialiasing:** SMAA from the reference implementation (vendored, MIT), MSAA on the presented surface only;
    a failed allocation falls back with a log line and the port undoes the saved value.
  - **Shaders** (RetroArch slang presets, `nv2a_vk_set_shader_preset`; porting guide M6): librashader is built by
    the port's deps script from source, **Vulkan runtime only** (Rust pinned, project-local, build time only;
    `CXXSTDLIB_<target>=static=c++` + `+crt-static` so the DLL imports only system DLLs), copied next to the exe,
    never committed; never the official Windows build (it imports `D3DX9_43.dll`). No Rust = no shaders, the
    game unchanged. The cache of librashader stays off (it writes to `%LOCALAPPDATA%`).
  Same names, order and value lists in every port unless the title cannot offer an option (e.g. no 30 fps entry
  for a 60 fps game).
- **RENDER RESOLUTION (maintainer, 2026-10-21; cap 5x 2026-10-23; every port):** a scale factor on the game's native
  lines, **1.00x–5.00x in 0.25 steps** (17 values), default 1.00x (1.5x = 720p, 2.25x = 1080p, 3x = 1440p, 4.5x = 4K,
  5x = 2400 lines; nothing higher – 5.5x/6x at 16:9 missed vblanks on the reference GPU, MSAA/SMAA smooth edges
  cheaper). Vertical factor; X = factor at 4:3 and for
  menus/movies kept 4:3, X = factor × 4/3 for 16:9 gameplay. The menu shows factor and resulting resolution
  ("2.25X 1440X1080" / "2.25X 1920X1080"), left/right steps; values over the device's image limits are greyed out, not
  hidden; no VRAM estimate - a step whose allocation fails goes back to the last good value. At 16:9 the menu offers the standard sizes only (854X480 native, 1280X720, 1600X900, 1920X1080, 2560X1440, 3200X1800, 3840X2160, 4267X2400; maintainer 2026-10-26 – the 0.25 grid gave 1707X960); each aspect keeps its own value and switching carries the width over (3X 1920X1440 → 1920X1080). A step to a value the device cannot hold is refused.

## Resources

- Builds only via `tools/build.ps1` (lock, ≤2 compiler processes, incremental; full rebuild only after lifter
  changes). Test runs always with `--max-seconds`; leave no processes running.
- Never load generated code or logs raw – grep / summarize.

## Portability

- Platform code only in `src/platform/`. SDL3 for window, input and audio. Clang (llvm-mingw) only; no MSVC
  extensions, no inline asm. Vulkan 1.3 only. No assumptions about pointer size or endianness; correct letter case.

## Portable install layout (hard rule)

Every port uses this layout – not negotiable (toolkit `docs/porting_guide.md`, details and drive mapping in
`docs/storage.md`):

```
@TITLE@ Recomp/
  @EXE@.exe     the recomp
  game/         the extracted disc (installer, from the player's own image)
  saves/        everything a player would miss: UDATA (U:), TDATA if user data, settings, console.txt,
                storage_layout.txt -- never deleted, not even by the runtime
  media/        the player's own media, read only (only if the media module is enabled): boot.mpg here =
                alternative boot animation (MPEG-1 + MP2; never skippable - the one video that is not, maintainer
                2026-10-27); every folder below it that holds music = one custom
                soundtrack
                (any depth; an .m3u/.m3u8 in it sets the track order)
  data/         everything safe to delete at any time: caches (cache/<title id> = Z:), TDATA if caches only,
                partition images, shader cache, logs
```

- **Nothing is written outside this folder:** no AppData, no registry, no temp files left behind. All paths are
  relative to the exe; `xbox_path_init_cfg` with explicit `game_dir`, `save_dir`, `data_dir`.
- **Media on the dev machine** (maintainer, 2026-10-25): the master is `<parent folder>\media\` (never modified or
  deleted); test layouts and this game's layout get a **copy** of `media/` from it, never a junction or link.
- **First start = the installer** (toolkit `src/install/game_setup.h`; no separate launcher): the player puts the
  extracted disc or one disc image (XISO or full redump) into `game/`; `main.c` calls `xr_game_setup` before the boot
  (model: THPS 2X `thps_game_setup`). Port config: `game` = "@TITLE@", `title_id` = the title ID from `docs/xbe.md`, `versions[]` =
  supported `{ name, default.xbe SHA-1 }` – the same list in this CLAUDE.md and the README ("Supported version").
  Version rules: wrong title ID → "This is not the @TITLE@ image. Please provide the correct game."; unsupported
  SHA-1 → "This @TITLE@ version is not supported. Supported version(s): <LIST>. Please provide a supported
  version." (checked on default.xbe first, image kept). Touches only `game/`; `RECOMP_NO_DIALOGS=1` in every
  automated test; installer tests only in throwaway layouts.
- `E:\TDATA` goes to `saves/` or `data/` per game, by the storage rule (`docs/storage.md`, "TDATA: saves or data,
  per title").
- Saves go to the hard disk only, never to memory units (toolkit rule, also for titles that support them).
- Storage decisions for this title (TDATA place, soundtracks): **TODO** – decide by `docs/storage.md` and record
  here.
- Tests use fresh temporary `saves/` and `data/` (`--save-dir`, `--data-dir`). `tests/outside_writes.ps1` fails
  if the exe writes anywhere outside the folder during a short run – run it at M1 and before every release.
