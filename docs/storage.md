# Storage layout for ports

How the runtime maps the console's drives onto the host, and the rules that keep a player's saves valid and safe
across runs, crashes and new versions. Code: `src/kernel/kernel_path.c` (rules, migration),
`src/kernel/kernel_file.c` (atomic save writes), `src/kernel/kernel_xbox.c` (console identity).
Test: `tests/storage_layout`.

## Three host folders

| Folder | Holds | May the player delete it? |
|---|---|---|
| `game_dir` | the extracted disc: `D:\`, `\Device\CdRom0\` | no (reinstall) |
| `save_dir` | everything a player would miss | **never** — the runtime never deletes anything here either |
| `data_dir` | everything the title rebuilds: caches, partition images, system data | yes, at any time |

```c
XboxPathConfig cfg;
xbox_path_config_default(&cfg);          /* tdata = XBOX_TDATA_SAVES */
cfg.game_dir = "...\\game";  cfg.save_dir = "...\\saves";  cfg.data_dir = "...\\data";
cfg.tdata = XBOX_TDATA_DATA;             /* only if the title keeps nothing important in E:\TDATA */
xbox_path_init_cfg(&cfg);                /* after xbox_MemoryLayoutInit (title ID), before the bridge init */
```

`xbox_path_init_ex(game, save, data)` is the same with `tdata = saves`. All paths are UTF-8.

## Drive mapping

| Guest path | Host path |
|---|---|
| `D:\`, `\Device\CdRom0\`, `Y:\`, `C:\`, `\Device\Harddisk0\Partition2\…` | `game_dir\…` |
| `E:\UDATA` (`\Device\Harddisk0\Partition1\UDATA`), `U:\` | `save_dir\UDATA\…`, `save_dir\UDATA\<title id>\…` |
| `E:\TDATA`, `T:\` | `<tdata>\TDATA\…`, `<tdata>\TDATA\<title id>\…` — `<tdata>` per title, below |
| cache partitions 3–5 (behind `X:`/`Y:`/`Z:`), `Z:\` | `data_dir\cache\<title id>\…` |
| raw partitions (`\Device\Harddisk0\PartitionN` bare) | `data_dir\PartitionN.img` |
| `\Device\Harddisk0\Partition2` bare (system data) | `data_dir\SystemData` |
| memory units `F:`–`M:`, `\Device\MU_*` | **not inserted**: translation fails |
| anything else | translation fails (logged; never passed to the host as written) |

Drive letters a title links itself (`IoCreateSymbolicLink`, e.g. XAPI's `T:`/`U:` to `E:\TDATA\<id>` and
`E:\UDATA\<id>`, `XMountUtilityDrive`'s `Z:`) are followed first and land in the same folders.

### TDATA: saves or data, per title

`E:\TDATA\<title id>` is the title's persistent data. Rule:

- the title keeps **user-relevant data** there (settings, unlocks, high scores, anything that would hurt to lose)
  → `save_dir\TDATA` (`XBOX_TDATA_SAVES`, the default — losing data is worse than an extra folder);
- it keeps **only caches** there → `data_dir\TDATA` (`XBOX_TDATA_DATA`).

Decide by looking at what the title writes (menu run plus one save; `RECOMP_HEAP_WATCH`, `[PATH]` log lines,
file timestamps) and write the decision into the game project. Changing it later is safe: the next start moves
`TDATA` to the new place.

### Memory units

Not supported, by decision (maintainer, 2026-10-15): every port saves to the hard disk only – also titles that
support memory units. No unit is ever inserted, the title's storage menus list the hard disk only, and no
memory-unit path reaches a host drive letter. Do not add a memory-unit emulation.

### Dashboard soundtracks

The dashboard's soundtrack database lives in `E:\TDATA\FFFE0000\MUSIC\` (`ST.DB`, then one folder per soundtrack).
Titles with custom-soundtrack support read it through XAPI (`XFindFirstSoundtrack`, …). **Decided (THPS 2X, M5,
2026-10-24): XAPI overrides, no database on disk.** `src/xdk/xdk_soundtrack.c` answers the XAPI soundtrack calls from
the albums of the port's `media\` folder (`src/media/media_scan.h`: every folder below it that holds music is one
soundtrack, at any depth, track order from an `.m3u` if present; `media\` itself holds only the optional
`boot.mp4|mkv`; the folder was called `soundtracks\` until 2026-10-18), and the WMA decoder a title creates for one of
those songs is a host media object (MP3/FLAC/WAV/Vorbis via dr_libs/stb_vorbis). Nothing is written under `media\`
and `E:\TDATA\FFFE0000` stays empty. A port binds the functions by address (THPS 2X: `src/soundtracks.c`).

## Saves stay valid

- **Console identity is fixed.** `XboxHDKey` (it signs non-roamable saves), the disk serial and the other keys have
  built-in defaults (all-zero key, serial `XR0000000000`) that must never change — a new default would invalidate
  every existing save. Never random. `save_dir\console.txt` may override them (in `save_dir`, because saves depend
  on it), e.g. for saves brought from a real console:

  ```
  hd_key = 00112233445566778899AABBCCDDEEFF
  serial = 123456789012
  ```

- **Layout version.** `save_dir\storage_layout.txt` (`layout 1 tdata data|saves`). A release that moves folders
  raises `XBOX_STORAGE_LAYOUT` and adds a step to `storage_migrate`: the next start moves the old folders once.
  Migration **moves, never deletes**: a name already taken at the destination is moved next to it as
  `<name>.migrated` (`.migrated2`, …). Layout 0 (before the file existed): `game\UDATA`, `game\TDATA`,
  `save\UserData` (old `U:`), `data\TitleData` (old `T:`), `data\TDATA`.

- **Atomic save writes.** A file under `save_dir` opened for writing is written to `save_dir\.pending\` and moved
  over the original when it is closed (`MoveFileEx` with replace + write-through). A run that dies mid-save keeps
  the previous file; the unfinished copy is dropped at the next start. Contents the title keeps (FILE_OPEN,
  FILE_OPEN_IF) are copied first, so in-place updates behave as before; a delete disposition on the handle deletes
  the original on close. Per file: a save made of several files can still be left half-written (the title then
  reports it as damaged, as on a console) — but an existing save is never damaged by a new one.
  `RECOMP_SAVE_CRASH_TEST=<text>` ends the process just before committing a file whose path contains `<text>`.

## Portability

- **Letter case.** FATX is case-insensitive; titles open `frontend\MainMenu.PANEL` for a file stored as
  `Frontend\mainmenu.panel`. `xbox_host_path_fix_case()` resolves each path component against the directory on
  case-sensitive hosts (POSIX); on Windows it does nothing.
- **Wide characters and long paths.** Host paths are UTF-8 in the API and UTF-16 at every Win32 call; translated
  paths longer than `MAX_PATH` get the `\\?\` prefix (buffers: `XBOX_HOST_PATH`). Guest names are Latin-1
  (Windows-1252), not the host's ANSI page. `xbox_fopen_utf8()` / `xbox_getenv_utf8()` for host files named in
  environment variables (pad scripts, screenshots).
- POSIX has the path rules and the case lookup; layout migration and atomic writes are Win32 only so far.

## Checks

- `tests/storage_layout`: migration from layout 0 (non-ASCII save folder), every rule above, atomic overwrite and
  in-place update, an interrupted write, identity defaults and override, TDATA moving with the title's choice.
- In a game project: save, kill mid-save (`RECOMP_SAVE_CRASH_TEST`), restart and load, delete `data\` and load
  again (THPS 2X: `tests/storage.ps1`).
