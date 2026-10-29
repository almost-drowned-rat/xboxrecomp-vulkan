# Lessons learned

Non-obvious bugs from the ports on this fork, each as **symptom → cause → fix → how to spot it early**. Add every
new one at the milestone it was found. Workflow: [porting_guide.md](porting_guide.md).

## Lifter and code coverage

### Bink movies with noise along every block edge (THPS 2X, M2)
- **Symptom:** all Bink movies show wrong blocks along edges; frames bit-identical run to run.
- **Cause:** `movsx ebp, bp` lifted as a zero extension (`_lift_movsx` lacked bp/sp) in Bink's 8×8 integer IDCT.
- **Fix:** lifter (fork `511fba8`) + test `test_lifter_movsx_bp.py`.
- **Spot early:** deterministic image errors = a decode bug, not a race. Bisect with the state replay
  (`xbox_StateSnapshot` + `tools.conformance.replay`, Unicorn as oracle) down to one function; hiding CPU features
  with `RECOMP_CPUID_EDX` separates SIMD paths.

### CPU feature probe takes the wrong path (THPS 2X, M1)
- **Symptom:** a null function pointer called a million times right after the first movie opens.
- **Cause:** Bink's MMX probe used `pushfd/popfd` + `cpuid`, which were untranslated – the blitter table stayed empty.
- **Fix:** lifter translates `cpuid` as the Xbox Pentium III and `pushfd/popfd` (fork `485fe41`).
- **Spot early:** `[UNIMPL]` lines in the first run; never ignore one inside a decoder or init path.

### Functions only reached through tables are missing (THPS 2X, M1–M3)
- **Symptom:** a hang or a no-op where a call should happen; `[ICALL] Failed to resolve VA 0x…`; "Enumerated but
  never attached" (USB), a menu that does nothing on one button.
- **Cause:** entry points reached only through data: XPP class-driver and endpoint tables, a Bink callback stored
  with `mov [mem], imm`, C++ vtables. The disassembler also *drops* a seed that lands inside an instruction its
  linear sweep decoded out of phase (inline jump table before the function: 0x00144FB0).
- **Fix:** seeds (`seeds/*.json`); `"observed": true` for addresses a run actually reached (the disassembler then
  trusts them over its sweep). Table scan: `scan_table_seeds.py` (16-byte aligned starts behind padding).
- **Spot early:** run the gap check before the first boot; rate-limit "Failed to resolve" logging (one missing
  vtable target wrote 850,000 log lines in two minutes); keep the ICALL feedback on during bring-up.

### Calls that silently do nothing: unresolved stubs (THPS 2X, M3 stage 3)
- **Symptom:** none at first – `gen/recomp_stubs_unresolved.c` held 303 entries. Each stub only pops the return
  address, so a branch or call into it is a no-op; error paths, loop edges and `__SEH_epilog` tails were skipped.
- **Cause:** (1) an alias body (a tail-jump entry into another function) loops back above its own start into
  the host, and nothing gave that target an entry; (2) an MSVC SEH function whose `__finally` block is a call
  target ends at that block, its error path and epilogue after it are a gap; (3) bogus "callers" – data (jump
  tables) decoded as code.
- **Fix:** disassembler alias closure (every direct branch into another body or into the gap after a body gets
  an alias entry, to a fixpoint): 303 → 37 stubs, the rest data. `tools/gap_check.py` explains each stub (gap /
  inside / outside) and suggests seeds.
- **Trap:** a NOP-padding boundary pass (early MSVC pads with `0x90`, not `int3`) found 4,000 more functions but
  split at least one live function – the menu tour crashed in a destructor. It is opt-in
  (`RECOMP_DISASM_EXTRA=nop_boundary`); it also cut the CRT `memmove` at a switch arm before the guard.
  Bisect a lifter regression with `RECOMP_DISASM_SKIP=<pass>` + relift + the menu tour.

### A reachability filter that drops entries only pointers reach (upstream v0.14 lifter, THPS 2X, M5)
- **Symptom:** after taking upstream v0.14.0's 52 lifter/disasm commits (branch `lifter-v0.14`, archived, not
  merged): 7831 → 6850 functions, gap check 37 → 14 stubs – but 7 of the 14 are new, and every level logged
  `[ICALL] Failed to resolve VA` for 0x000BF1A0, 0x000BF1B0, 0x000B9290 (hundreds of calls each). Frame rate and
  pictures looked normal, all lifter unit tests passed.
- **Cause:** 934 alias entries (`tail_jump_alias`) vanished. A reachability check now decides which branches
  count: upstream `afb99af` ("ignore unreachable branches during tail closure") and the same check added to the
  THPS 2X alias closure while merging. Recursive descent starts only from known function entries and switch-table
  arms, so code reached only through function pointers, vtables or callbacks counts as unreachable and its tail
  jumps into shared epilogues are ignored. The new stubs are exactly such targets: a D3D epilogue
  `pop edi; pop esi; pop ebx; ret 8` (0x00013869 – the stub only does the `ret 8`), the normal and `__except`
  exits of two Bink SEH functions (`call __SEH_epilog; ret 4`, 0x001BDF9E/A0, 0x001BE18B/D), a game tail
  (0x000F7100) and a CRT x87 tail (0x001E2313). Which of the two checks drops what was not bisected yet
  (`RECOMP_DISASM_SKIP=alias_reach` on the archive branch turns the THPS-side one off).
- **Fix (when it is picked up again):** seed the reachability with everything that is an entry for other reasons
  – alias entries, seeds, data-referenced pointers (vtables, callback immediates), observed ICALL targets – or keep
  the old unfiltered closure for alias entries. Until then THPS 2X stays on v0.13.1 + its own lifter fixes.
- **Spot early:** compare a relift against the previous one before running anything: function count, gap-check
  stubs **by address** (a lower total can hide new ones), and grep the run logs for `[ICALL] Failed to resolve`.
  A stub is a silent no-op – only the gap check and the ICALL log show it.

## Memory and buses

### Pad input dies in every menu after the first (THPS 2X, M3)
- **Symptom:** main menu takes input, Career menu ignores everything; the game's `XInputGetState` returns the same
  state forever (A held) while the handle's packet number keeps moving.
- **Cause:** physical addresses are ambiguous here: pool/heap memory is identity-mapped low RAM, the contiguous
  window is separate storage, both use physical numbers 0–64 MB. The OHCI model resolved "below the contiguous
  high-water mark" into the window. XPP's report buffer was pool memory at 0x01C0F52A; once > 28 MB of contiguous
  memory was allocated, reports went into the window.
- **Fix:** `MmGetPhysicalAddress` records the owner per 4 KB page (`xbox_PhysOwner`), bus masters ask it first.
- **Spot early:** byte-trace what a device writes and read it back from the driver's side; a write the hardware
  watchpoint (`THPS_WATCH_WRITE`) never sees at the driver's address went somewhere else. Audit every bus master
  (APU, `dma_resolve`) for the same rule.

### Controller on the wrong port (THPS 2X, M3)
- **Symptom:** some menus ignore the pad entirely.
- **Cause:** XPP 3911 maps root-hub ports 0/1/2/3 to controller ports 2/3/0/1; the default put pad 1 on port 2, and
  menus that read only player 1 never saw it.
- **Fix:** `RECOMP_USB_NDP=4`, `RECOMP_USB_PORT=2` in the game's `main.c`.
- **Spot early:** log `XGetDeviceChanges`' insertion mask and `XInputOpen`'s port on the first run.

## GPU

### Grey veil over the menus, text invisible (THPS 2X, M3)
- **Symptom:** front end washed out grey, button text gone, some screens (3D) fine.
- **Cause:** fixed-function draws with fog on (planar, D3D's default final combiner mixes by `fog.a`) got fog
  distance 0 and a "raw" factor of 0 → every pixel in the fog colour. Fog off was also wrong for fixed function.
- **Fix:** fog off = factor 1 for every transform mode (xemu behaviour); fixed-function fog distance per
  `SET_FOG_GEN_MODE` in the vertex shader.
- **Spot early:** `RECOMP_FRAME_TRACE` shows fog enable/colour per draw; a veil in exactly the fog colour.

### D3D hangs waiting for an event nobody sets (THPS 2X, M3)
- **Symptom:** a screen change hangs with the GPU idle; `KeWaitForSingleObject` returns `C0000001` in a loop.
- **Cause:** D3D builds its KEVENT itself (no `KeInitializeEvent`) and sets it from its PGRAPH handler on a NOP
  trap; the bridge cannot map such an event. `RECOMP_TITLE_KEVENTS=1` maps it but then blocks D3D's vblank wait.
- **Fix:** the fence-wait wrapper spins on the GPU semaphore (D3D's own short-wait path) instead of sleeping.
- **Spot early:** watchdog stack in a D3D wait + `eax=C0000001`; an object address inside D3D's data whose
  WaitListHead points at itself.

### D3D fence never completes (THPS 2X, Burnout 3, M2)
- **Symptom:** hang after the first frames in D3D's fence/BlockOnTime.
- **Cause:** the GPU time D3D waits on must be the executor's semaphore release, not a mirror.
- **Fix:** wrap the fence wait, register `*(dev+<offset>)` with `nv2a_pb_set_semaphore_target`.

### A ~90 ms frame in play although the shader cache is warm (THPS 2X, M5)
- **Symptom:** with a full SPIR-V/pipeline cache, one frame of 85–91 ms now and then in a level; the hitch line
  says `1 programs compiled (85–91 ms)`.
- **Cause:** not the cache key – a genuinely new program (one the earlier runs never drew). The program costs
  ~3–7 ms; the rest is glslang building its built-in symbol tables at the **first compile in a process**.
  With a warm cache that first compile is the first new program in play. Measured with a cache of one, then
  three programs: ~95 ms vs ~106 ms in total.
- **Fix (`nv2a_vk`):** a thread compiles a tiny vertex + fragment shader right after
  `glslang_initialize_process()` (same version/client settings), so the tables exist before the title draws;
  glslang guards them with its global lock, a real compile meanwhile only waits. First in-run compile
  89–96 → 6.8 ms. Cost: glslang's tables stay in memory, ~24 MB for the whole run (THPS 2X menus: private
  261 → 285 MB). `RECOMP_GLSLANG_WARM=0` turns it off.
- **Spot early:** compare the cost of the first compile of a run with the later ones (`prog_ms` per frame);
  a fixed ~85 ms on top of the first only is start-up cost, not shader size.

### Level start: ~17 ms of a 40–50 ms frame is vkAllocateMemory (THPS 2X, M5)
- **Symptom:** with warm shader caches the level-start frame still took 40–54 ms; `texmk_ms` (image + memory
  creation) was ~17 ms of it for ~150 textures.
- **Cause:** one `vkAllocateMemory` per texture, ~0.12 ms each on this driver (Windows, NVIDIA).
- **Fix (`nv2a_vk`):** textures up to 1/8 of a block take a range of a device-local block (own first-fit range
  allocator `range_alloc.c`, unit test `tests/range_alloc`); render targets, depth, cube maps and larger
  textures keep their own allocation. Block size 16 MB (`RECOMP_VK_TEX_BLOCK_MB`), at most 1/32 of the
  device-local heap, at least 4 MB; number of blocks = texture budget (`RECOMP_GL_TEX_MB`) / block + 1. All
  blocks full → own allocation (logged once); that failing too → dummy texture (logged). Ranges return with
  the frame garbage, an emptied block is freed (the last one stays). `RECOMP_VK_TEX_BLOCKS=0` = old path.
  Texture create 17–22 → 0.7 ms; level-start frame Hangar 53 → 35 ms, Streets 45 → 22 ms; VRAM 57 → 54 MB.
  16/32/64 MB gave the same gain; 64 MB cost +45 MB VRAM and ~+35 MB private bytes. Rebuild soak (38 set-ups):
  1–2 blocks, VRAM and RAM flat.
- **Spot early:** time image creation separately from the upload; a per-texture fixed cost that scales with the
  count, not the size, is the allocation.

### A title's own decoder crashes when driven with odd packet sizes (THPS 2X, M5 custom soundtracks)
- **Symptom:** a playlist going from a custom song to a game track crashed in WMADEC (`sub_001F9A8B`, read at
  `0x7FFF7FFF`; the stack full of clipped PCM), followed by a bogus DPC line; a game track as the first song played.
- **Cause:** the host media object asked the title's WMA media object (`Process`) for the rest of a half-filled
  packet – a size the title itself never uses (always 0x2000 bytes). Its state broke.
- **Fix:** the host side always hands the title's object whole packets of the title's size and keeps the surplus
  (THPS 2X `src/soundtracks.c`, `gs_read`).
- **Spot early:** when host code drives a title's media object, log the sizes the title itself passes and use only
  those.

### A saved soundtrack choice crashes the level load (THPS 2X, M5 custom soundtracks)
- **Symptom:** with a saved custom album of 20 songs, the game crashed right after the intro (the main menu is a
  level): `songs\.bet` not found, then a null read in `0x00097070`. Albums of 2–4 test songs never showed it.
- **Cause:** the port put the saved album into the game's state at the level's music set-up. A later level-load
  step (`0x00097160`) loads one side file per song of the *current soundtrack's* song count, but the names come from
  the level-song list (16). On the console the soundtrack is always the game's own at that point, so the counts
  match.
- **Fix:** that step sees the level song count while it runs (THPS 2X `src/soundtracks.c`, `sub_00097160`).
- **Spot early:** when the port sets title state earlier than the console could, list every reader of that state
  (grep the generated C for the address) and test with an album larger than the game's own list.

### A gloss that never moves: fixed-function texgen ignored (THPS 2X, M5)
- **Symptom:** none obvious. The skaters looked right; over Roswell's green floor they took on a green-dark cast
  everywhere, whatever the view.
- **Cause:** the skaters are lit fixed-function draws with REFLECTION_MAP texgen on stage 0 (S/T/R, `0x03C0`) and
  the stage-0 texture matrix on (`0x0420`); the combiner adds 0.2 x a cube map to the skin texture before the
  lighting. The shader passed the vertex's own 2-component coordinates through, so the cube map was read from one
  fixed direction.
- **Fix (`gl_vsh_ff.c`, after xemu vsh-ff.c):** `reflect(normalize(eye position), eye normal)` on the generated
  components, then `oT = oT * texMat` (rows as delivered, like the model-view). `RECOMP_VK_TEXGEN=0` turns it off,
  `=t<n>` off every other n frames for on/off pictures. Other texgen modes are not built (no title uses them yet).
- **Spot early:** count fixed-function features per level before building any (a diagnostic build counting the
  registers at each fixed-function draw: `0x17C4` two-sided, `0x0328` skinning, `0x03C0+` texgen); then per draw:
  is the stage sampled (`SET_SHADER_STAGE_PROGRAM`), cube or 2D, does a
  combiner input read it. Dropping those draws in a diagnostic build on a paused frame shows which objects they are.
- **Uniform ring:** `VsBlock` lives in a ring that is not cleared per frame, so a field set only under a condition
  keeps a stale value. Write every field per draw (`tgen` does). `foggen` is set only with fog on; harmless,
  because with fog off `u_fogp.w = 1` and `nv2a_fog()` returns 1 without reading `vFog`.

### "Time based" at 120 fps, but the skater moves differently (THPS 2X, M5)
- **Symptom:** none visible at first: the session clock ran exactly at 30/60/120 fps (M4), so the logic was booked
  as time based. Measured in M5 (the skater's position every frame, two runs per rate): at 120 gravity 1.5-2 %
  weaker; at the same rate the runs differ by about 1 %.
- **Cause:** the level loop turns the frame time (us) into **integer** steps, truncated - the integration step
  (128 per 60 Hz frame), a timer factor (2048) and a **precomputed step² >> 8**, plus a step clock accumulated from
  the step. At 60 the loss is 0.2 %, at 120 0.7 % for the step and up to 3 % for its square (63² >> 8 = 15 for 15.5).
- **Fix:** above the original rate, the same steps exact with the fraction carried from frame to frame, and the step
  clock with them (THPS `recomp_manual.c` `thps_timestep_fix`, right after the level loop `sub_0015B080`); at the
  original rate the title's own truncation stays bit for bit. Gravity at 120 then +0.4-0.6 % against 60.
- **A wrong lead from the measurement:** the speed taken per frame from position differences came out 4.5-9 %
  higher at 120 and looked like a per-frame speed term. It was the method: single-frame position jumps (snapping
  onto rails and the ground, collision push-out) count as distance / dt, and at 120 dt is half as long. Over fixed
  100 ms windows the corrected 120 differs by +0.2 % (p90 and p99), and the title's own velocity field (+0x4C) by
  0-1 %. Measure speed over fixed time windows (or read the velocity field), never per frame.
- **Also found:** replay playback takes one recorded frame per rendered frame (a 2:00 replay played in 60 s at
  120 fps) - the recorder writes once per game tick and a replay plays at the original rate.
- **Spot early:** never conclude "time based" from a clock alone. Log the player's position per frame (find it
  through what the replay recorder gets), fit gravity in flight and compare speed percentiles at both rates, two
  runs per rate as the noise floor (porting guide, M6 FRAME RATE: the 120 check). Grep the level loop for the
  frame-time conversion: integer division / `>>` on the step, a squared step.

### A player's game ran on without a picture: the device lacked one Vulkan extension (THPS 2X, v0.9.2)
- **Symptom:** a player log (AMD Radeon RX 580, Windows, Vulkan 1.3.260) shows `vkCreateDevice failed` and then
  the game running on - sound, saves, no window content.
- **Cause:** AMD's Windows driver for Polaris (RX 400 / RX 500) has no `VK_EXT_vertex_input_dynamic_state`;
  `nv2a_vk` requested it unconditionally and, when device creation failed, fell back to its headless test path.
- **Fix:** device check at start (`8ac412a`): every failure of window, instance, device or swapchain ends in
  `vk_fatal` - log + message box naming the GPU, the driver's Vulkan version and every missing item, then the
  port's handler (`nv2a_vk_set_fatal_handler`, its requirements link); headless only when asked for
  (`RECOMP_VK_HEADLESS=1`, SDL offscreen). `RECOMP_VK_TEST_MISSING=<extension>` fakes a missing extension for tests.
  For devices without the extension: the vertex formats are baked into the pipelines (THPS 2X v0.9.3;
  `RECOMP_VK_NO_DYN_VERTEX_INPUT=1` forces it on any GPU).
- **Spot early:** a test-only fallback (headless) must never be reachable from a player's failure. Check every
  required extension against gpuinfo.org per vendor and OS before relying on it (Polaris has it on Linux/RADV,
  not on Windows), and keep a switch that forces each fallback on the dev GPU so it gets tested.

## Kernel, files, storage

### Saves land in the game folder; guest paths reach host drives (THPS 2X, M3)
- **Symptom:** `E:\UDATA` written into the extracted disc folder; `F:\…` (memory units) and any unknown path passed
  to the host as written.
- **Fix:** storage layout ([storage.md](storage.md)): UDATA/TDATA rules, memory units "not inserted", unknown paths
  fail.
- **Spot early:** search the first run's log for `[PATH]` prefixes that have no rule.

### The game says "Unable to load most recent save" (THPS 2X, M3)
- **Symptom:** at boot after an interrupted save.
- **Cause:** a multi-file save interrupted after its metadata was written: per-file atomic writes keep old saves
  intact but cannot make a new multi-file save all-or-nothing. Same on a console. Expected.

### "Already exists" when overwriting a save (THPS 2X, M3 stage 3)
- **Symptom:** Options → Load/Save → overwrite CAREER A, keep the name → "a file named CAREER A already exists".
- **Cause:** game logic, not the kernel: from this menu the save screen is built in "new save" mode
  (0x0010CB8D); "overwrite" mode needs the in-game path (previous menu state 0x16). Found with
  `RECOMP_SAVE_TRACE` wrappers around the game's list/name/delete helpers. Fixed along the way: `NtCreateFile`
  with `FILE_CREATE` on an existing directory now collides, and `Information` reports created/opened/overwritten.
- **Lesson:** trace the title's own helpers before "fixing" the kernel; behaviour that looked random (3 of 4)
  became deterministic once the lifter gaps were closed.

## Process

### A test passes because a screenshot file exists (THPS 2X, M3)
- **Symptom:** "screenshot after loading: PASS", but the picture shows the skater editor.
- **Cause:** the input script took a different path (a boot message only shown with a damaged recent save).
- **Fix:** checkpoint screenshots at every step, contact sheets, and look at them.

### Timing-based input scripts drift (THPS 2X, M3)
- **Symptom:** a press does nothing, the script is out of step.
- **Cause:** screens take input only after their fade-in; Select Player polls the pad more slowly (150 ms presses
  missed); cursors start at different entries depending on state (fresh start vs after a load).
- **Fix:** 300 ms holds, a second or more after each screen appears, checkpoint shots, separate scripts per start
  state.

### A derived input script silently loses its `@include` lines (THPS 2X, M5)
- **Symptom:** a script copied with a few lines changed ran without error, but the level select stayed on a locked
  level: the unlock cheat was never entered. Levels that are open by default looked fine.
- **Cause:** `@include` paths are relative to the including script's folder; the copy lived elsewhere, the
  snippet was not found and the step block was skipped.
- **Fix:** write derived scripts next to the original (or next to its `snippets\`).
- **Spot early:** check the log's `PAD: t=... step N` lines for the included steps, or a checkpoint shot right after
  them.

### Before/after pictures from two builds do not line up (THPS 2X, M5)
- **Symptom:** the same input script on the old and the new build gave different scenes at the same timestamp
  (score, position, even the camera).
- **Cause:** a changed shader is a SPIR-V cache miss in the new build; its one-time compile shifts the level load,
  and the time-based script drifts from there.
- **Fix:** compare inside one run: a switch that flips the feature every n frames (`RECOMP_VK_TEXGEN=t4`) and short
  shot bursts; the log's `[VK] screenshot ... (frame N)` tells which state each shot has (the shot shows frame
  N-1, so leave out shots at a block boundary). A paused frame works only when no menu covers the object.

### "No network link" greys out local split-screen too (THPS 2X, M3)
- **Symptom:** Multiplayer greyed out in the main menu even with two controllers connected.
- **Cause:** after Press Start the title maps only the pad that pressed it; the menu counts mapped players and,
  below two, asks the network link check. "No link" was answered for both System Link and split-screen.
- **Fix:** an override of the link check at that one call site (return address) that answers "link" with two or
  more controllers connected; the kernel keeps reporting no link, so System Link stays greyed.
- **Spot early:** run the menu tour with two controllers (`0:p2-connect`) as well as one.

### A sound crash that only shows with a second controller (THPS 2X, M3)
- **Symptom:** divide by zero in DSOUND code (`sub_0004C924`) as soon as a second pad is opened, only without
  `RECOMP_AC97_READY`.
- **Cause:** the title's DirectSound reads the (absent) APU; with two pads it plays a sound the one-pad path never
  plays.
- **Fix:** run with the APU (or DirectSound at API level); test the two-controller path with every audio setting.

### The lifter's output depends on the capstone version (toolkit CI, 2026-10-15)
- **Symptom:** `test_lifter_rep_compare_flags` passes on Windows, fails in the Linux CI (`cmpsw` count 2: got -1).
- **Cause:** the CI took Ubuntu's `python3-capstone` 4.0.2; Windows has 5.0.7. Same lifter, different
  disassembly, different C.
- **Fix:** pin capstone 5.0.7 wherever the lifter runs (CI venv); lift a title with the version its seeds were made
  with.

### "Audio works" with an APU that outputs silence (THPS 2X, M3)
- **Symptom:** none audible at first – the menus run, DSOUND calls return, nothing crashes.
- **Cause:** the LLE APU path ran but mixed nothing: `RECOMP_APU_WAV` recorded 134 s of zeros on the menu tour,
  while the DirectSound HLE recorded the movies, menu sounds and effects of the same run.
- **Fix:** DirectSound at API level. Record the mix (`RECOMP_DSOUND_WAV`) on every audio change and check its
  level over time before asking a human to listen.

### The game runs as fast as the display (THPS 2X, M3.5)
- **Symptom:** none in a quick look. The frame-time counter showed the menus at ~460 fps with V-Sync off and at
  98 fps on a 99 Hz display with V-Sync on; the main-menu skater animation went through a trick in 0.6 s
  uncapped instead of 1.6 s at 99 Hz – with identical screenshot stalls in both runs, so the animation counts
  frames, not time.
- **Cause:** nothing paced the guest. THPS 2X gets no vblank interrupt here (`RECOMP_VBLANK` unset) and its swap
  throttle only waits for the GPU, so it swaps as fast as the host presents; burnout3-sw's renderer chose an
  unblocked present mode because Burnout 3 paces itself.
- **Fix:** `nv2a_vk` paces every flip to a fixed 60 Hz guest clock (`RECOMP_GUEST_HZ`, default 60; 0 = off for
  benchmarks), sleeping on a high-resolution timer, and presents with FIFO (V-Sync) by default. Measured: 60.0 fps
  with V-Sync at 99 Hz and with V-Sync off (p99 17.1 ms). A 144 Hz display shows the 60 Hz frames with judder until
  the logic is decoupled (M4); a display below 60 Hz slows the game down.
- **Spot early:** frame-time counter from M2 on; check menu and in-game speed separately – a title can time one and
  count frames in the other.
- **Next (D3D8 HLE, maintainer note 2026-10-15):** the pacing must not live only in the pushbuffer path. Plan: a
  real 60 Hz guest vblank – the signal the title's swap throttle waits on (D3D's vblank event/counter or the
  NV2A vblank interrupt) – driven by one runtime clock that both paths use; the pushbuffer flip and the HLE
  Present/Swap wait on it instead of sleeping in the renderer.

### Two 60 Hz pacers: missed vblanks in pairs (THPS 2X, M5)
- **Symptom:** 2000–5000 frames > 25 ms per 10 minutes of a level at 60 Hz, irregular (0–55 % of the time per
  30 s): two presents ~1.5 ms apart, then a 31–48 ms frame in which the game thread waits on a GPU fence; plus
  15–66 doubled frames per 10 min after the port's frame limiter slept.
- **Cause:** two clocks of nearly equal rate – the guest vblank (host timer, 60.000 Hz; the D3D swap throttle waits
  on it) and the display (59.997 Hz) through FIFO back-pressure with 2 frames in flight. Their phase drifts slowly;
  near the edge the order flips. Separately, the limiter's sleep ended ~13 ms into the frame and the throttle then
  missed the vblank by a hair.
- **Fix:** `nv2a_vk` presents with MAILBOX by default (FIFO only when the device lacks it, or `RECOMP_PRESENT=fifo`);
  the port's limiter does not wait while the guest vblank paces and the swapchain is not FIFO
  (`nv2a_vk_present_fifo()`); with FIFO it keeps its old wait (no limiter wait made FIFO's pair mode worse).
  Waits go through `platform/host_wait.h`. Measured (`host-wait`): Hangar and Streets 0 frames > 25 ms in 4 x 10 min,
  p99 17.2–17.3 ms; FIFO forced at the old baseline; the session clock exact.
- **Spot early:** frame-time CSV in a 10-minute level run; count frames > 25 ms and look for present pairs.
- **Long term:** one clock (guest vblank from the display / present-wait) – parked with uncapped / 120 fps.

### Where a menu frame's CPU time goes (THPS 2X, M3.5)
- **Measured** (uncapped menu tour, frame-time counter + timed D3D census): frame 2.06 ms = game logic 1.63 ms
  (including the D3D library, 0.215 ms) + pushbuffer decode and Vulkan recording 0.34 ms + present 0.09 ms.
- **Consequence:** a full D3D-level HLE that skips encode and decode would save < 0.3 ms per frame in the menus;
  the pushbuffer path stays THPS 2X's renderer (maintainer, option A). Re-measure in a level scene (M4): if decode +
  recording exceed ~15 % of the frame there, revisit the full HLE.
- **Also found:** 37 of the 65 D3D functions THPS 2X calls write the pushbuffer at once (directly or through a
  callee), only 2 keep shadow state only – an API-level layer that drops immediate writers loses state.

### Two host threads spin a core each while the game idles (THPS 2X, M3.5)
- **Symptom:** idle main menu at 60 fps, 216 % of one core for the process; two threads at 100 % each.
- **Cause:** the NV2A register threads (flag thread, ack thread with the pushbuffer executor) paused with `Sleep(0)`,
  a bare yield; burnout3-sw wakes them from the title's poll loops (`recomp_spin_wake`, emitted by its lifter) –
  this fork's lifter emits no such hints.
- **Fix so far:** the flag thread polls on a high-resolution timer (`RECOMP_NV2A_POLL_US`, 200 µs) → 119 %. The ack
  thread still yields: sleeping it stalled the boot (GET stuck behind PUT – the executor's trap handshake with the
  driver). D3D's kickoff flush spin is acknowledged by an instruction hook (`recomp_spin_wake` before the test at
  0x00019DE0 / 0x00019ED0) instead of by a polling thread – uncapped menus went from 468 to 530 fps.
- **Next:** wake the ack thread from a hook after the kickoff's PUT write (event), then let it sleep (A5, early M4).
- **Spot early:** measure process CPU in an idle menu (per thread) as soon as the menus run.

### "Unhandled method" is not "ignored" (toolkit executor, M3.5)
- **Symptom:** the executor reported 263 distinct unhandled NV097 methods on the THPS 2X menu tour, yet the menus
  render correctly.
- **Cause:** every 3D-class method lands in the register file first; the back end reads most state from there
  (combiners, fog, texture stages, matrices). "Unhandled" only means "no case in the switch".
- **Fix:** triage with the OR of every value a method carried (`or=` in the report): 148 never set, 48 set but read
  by nobody, of which one could change the picture (window clip – now the scissor).

### Readback: check before building it (THPS 2X, M3.5)
- `RECOMP_RT_WATCH=1` protects the pages of every surface the renderer drew into after each flip (both CPU aliases
  of the contiguous window) and logs the first CPU touch. THPS 2X menus and the save flow: 0 touches – no readback
  needed there; repeat in a level.

### A clock that never moves: titles count vblanks (THPS 2X, M4)
- **Symptom:** THPS 2X's Single Session clock stood at 2:00; everything else ran (time-based physics, menus).
- **Cause:** the game's `VSync()` counts the vblanks since its last call through D3D `GetDisplayFieldStatus`
  (VBlankCount = the miniport's vblank count, +0x820), and its game time only advances by that count. Only D3D's
  vblank interrupt handler increments it, and nothing raised that interrupt; the flip pacing (mirrored "completed"
  counter) never touched it.
- **Fix (title override):** answer `GetDisplayFieldStatus` from the runtime's display clock
  (`xbox_DisplayVblankCount`: fixed 60 Hz, the guest vblank's time base, unaffected by `xbox_GuestSetHz`).
- **Look for it in every port:** a frozen timer or animation with everything else alive → grep the game for
  `GetDisplayFieldStatus` / VBlankCount readers before suspecting the logic.

### Delivering the D3D vblank interrupt (`RECOMP_VBLANK=1`, experimental)
- The ISR masks PMC_INTR_EN and queues a DPC; the DPC re-enables it. D3D's vblank handler acknowledges by writing
  PCRTC_INTR (write-1-to-clear) and spinning until PMC_INTR bit 24 drops – on plain-RAM registers that spin never
  ends and held the timer thread, so no DPC ran again and the next PGRAPH trap hung the boot. The bits are now
  held until the vblank DPC starts (`xbox_Nv2aVblankHold/Release`, burnout3-sw), and a title override of the
  handler calls `xbox_Nv2aVblankTaken` first.
- With the interrupt on, D3D retires flips itself (its retire count is the swap throttle's "completed"): the
  counter mirror must stand down, or completed runs ahead of submitted and the throttle (unsigned difference
  >= 2) waits forever.
- Still racy without a guest lock: D3D queues flips from the software-method trap and retires them in the DPC,
  on two host threads; on the console both run at DISPATCH on one CPU. THPS 2X saw completed one ahead after
  ~1500 swaps. Keep it off until ISR/DPC/trap handlers are serialised.

### Decals that z-fight: NV2A polygon offset (THPS 2X, M4)
- **Symptom:** floor oil/dirt decals and wall graffiti in the Hangar broke up in patches that changed with the camera.
- **Cause:** 13–15 % of the level's depth-tested draws enable POLY_OFFSET_FILL (factor −1.25/bias −5, −1.5/−6, compare
  LESS); the back end ignored 0x0330–0x0338/0x0384/0x0388. Measured with `RECOMP_VK_ZSTATS=1`.
- **Fix (`nv2a_vk`):** `depthBiasEnable` on in every pipeline, the values per draw with `vkCmdSetDepthBias` (0/0 when
  the title has it off; Vulkan 1.0 core, no pipeline variants, no shader depth write – early-Z stays), filled
  triangles only, clamp 0. Slope = the factor. Constant = the bias converted from guest depth units (1/zmax of the
  0..1 range: Z24 1/0xFFFFFF, Z16 1/0xFFFF) to the host format's r:
  - **D24S8:** r = 2^-24 → ×2^24/zmax (Z24 ≈ 1, Z16 = 256).
  - **D32_SFLOAT_S8 fallback** (AMD and many Android GPUs lack D24S8 in Vulkan): r = 2^(e−23), e the exponent of
    the primitive's largest depth – not known per draw. Taken as e = −1 (depth 0.5..1, where a perspective scene
    lies almost entirely), giving the same scale; nearer than 0.5 the offset halves per octave (float depth has
    the precision to spare there). Tested by forcing it: `RECOMP_VK_DEPTH_FMT=d32`.
  - Offset on points/lines is logged once and not applied. `RECOMP_VK_DEPTH_BIAS=0` turns it off for comparisons.

### A port option that never applied: an unset env var counted as set (`nv2a_vk`, M6)
- **Symptom:** RENDER RESOLUTION from the THPS 2X menu changed nothing; the log said "render scale option ignored:
  RECOMP_GL_SCALE[_X] is set" with nothing set. The API (`69f40b1`) had never been driven by a port before.
- **Cause:** `k = e ? strtod(e) : 1.0; s_scale_env = k > 0` – unset gave 1.0, so "set".
- **Fix:** `s_scale_env = e && k > 0` (branch `recomp-options`).
- **Spot early:** drive every run-time option once from a clean environment and look for the log line that says it
  applied (`[VK] rendering at ...`), not only for the absence of errors.

### A menu table whose "none" marker is a valid id (THPS 2X, M6)
- **Symptom:** after the RECOMP OPTIONS rework, A on OPTIONS -> MEMORY UNIT stepped the render scale instead of
  opening the screen; the storage test failed 22 checks while the menu tour and split screen stayed green.
- **Cause:** the line -> value map marked "no value" with -1 only in the lists it built; a static array is 0 in every
  other list (OPTIONS itself, CONTROLS ...), and 0 was the first value id (RENDER RESOLUTION).
- **Fix:** store id + 1, 0 = none (THPS `recomp_manual.c`, `opt_val_at`).
- **Spot early:** in a zero-initialised table zero must mean "nothing"; the full regression after every menu change
  (the storage test's input scripts walk the Options list) caught it.

### Restart from the menu: hand over the start environment (THPS 2X, M6)
- A port that turns settings into env defaults at start (`host_setenv_default`: THPS_WIDESCREEN, RECOMP_VSYNC) must
  not let a restarted process inherit them – it would start with the old values. Keep the environment block at the
  very start (`GetEnvironmentStringsW`) and pass it to `CreateProcessW`; leave an `--input-script` out (it is timed
  from the start and would play again); the new process waits for the old one (`--restart-wait <pid>`) before it
  opens the log. THPS `host_restart`, `src/platform/host_win32.c`.

### Prove a fallback by forcing the failure (`nv2a_vk`, M6)
- Render scale and MSAA have no VRAM estimate any more (maintainer, 2026-10-26); a failed allocation must fall back
  to the last good value, log it, and let the port undo its saved value. On a card with free VRAM that path never
  runs by itself.
- **Test switch:** `RECOMP_VK_FAIL_ALLOC_MB=<n>` refuses every image larger than n MB (w x h x layers x samples x 4
  bytes) in `make_image_ml` and logs it. THPS 2X: n = 20 sends a 4.25x render step back to 4.00x; n = 3 sends
  FXAA -> MSAA 4X back to FXAA (both in the toolkit log and in `saves\settings.ini`).
- **Size the threshold from the images, not from a guess:** the first MSAA run did not trip, because the main-menu
  image at MSAA 2x has the same size as the options screen at MSAA 4x, so any n that refuses one refuses both or
  neither. Pick a step whose images are strictly larger than all images of the step before, and check the refusal
  line in the log before reading the result.

### Validation layer after a renderer change (`nv2a_vk`, M6)
- Run one pass with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`,
  `VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG` and `VK_KHRONOS_VALIDATION_LOG_FILENAME=<file>`
  (Windows; no code change). The SMAA / MSAA work found VUID-vkCmdDraw-None-04441 there and nowhere else: the
  1x1 HUD images were sampled without `VK_IMAGE_USAGE_SAMPLED_BIT` (fixed, they always get it). Still reported and
  older than this work: VUID-vkQueueSubmit-pSignalSemaphores-00067 (a swapchain semaphore reused before its present
  finished; open item).

### A user-supplied DLL: check its imports, not only that it exists (`nv2a_vk` shaders, M6)
- The first shader test series ran with the official `librashader.dll` next to the exe and every run said "no
  librashader": `SDL_LoadObject` failed because the DLL **imports `D3DX9_43.dll`** (its D3D9 runtime; the DirectX
  end-user runtime, not part of Windows 11). The win7 build imports it too. `llvm-objdump -p <dll> | grep "DLL
  Name"` showed it in one line. **Check the import table of any third-party binary before the first test**, and
  make the log tell "file missing" from "file there, does not load" (`shader_pass.c` names the likely missing
  library). Outcome (maintainer, 2026-10-27): librashader is built from source, Vulkan runtime only.
- **A Rust cdylib built with llvm-mingw imports `libc++.dll` / `libunwind.dll`** unless told otherwise, and
  `-C link-arg=-static` does not change it (rustc writes `-Bdynamic` in front of every library a crate asks for as
  a dylib). What works: `-C target-feature=+crt-static` (std takes `libunwind.a`) and
  `CXXSTDLIB_<target>=static=c++` (the cc crate asks for `libc++.a`), plus `-L native=<llvm-mingw>/<triple>/lib`.
  Check the import table of every build (`llvm-objdump -p`), not only that it loads on the build machine - the
  build machine has llvm-mingw's DLLs on its PATH.
- **Library calls that submit on your queue:** `libra_vk_filter_chain_create` (the plain one) submits its set-up
  to the queue and waits for the queue to go idle - from a worker thread that races the render thread's
  `vkQueueSubmit` (queues need external sync). Use the deferred create on the worker (its own command pool) and
  submit that command buffer from the thread that owns the queue. Read the library's create path before you
  thread it.

### Menu texts in the game's font: result lines and glyphs (THPS 2X SHADERS menu, M6)
- **A result line ("SAVED AS MY PRESET 1") is state, not a label:** the first SHADERS menu still showed it after
  SHADER OFF (maintainer's review). Clear it in the one function every change goes through (THPS `sh_apply`, plus
  the parameter setter and the reset) and when the screen is left, and shoot the screen after the next change.
- **Check the game font's glyph set before writing a text:** the THPS 2X panel font has capitals, digits, the dot,
  the colon and the space only; "CHANGE(S)" would lose its parentheses, so the text picks CHANGE / CHANGES by the
  count. Write the glyph set into the port's docs once (THPS: `docs/shader_module_phase1.md`, font limits) and map
  user strings (preset names, parameter descriptions) through one function (`bind_upper`).

### Boot animation: which thread owns the window; pl_mpeg's probe seeks back (`src/bootvideo`, M6)
- **The window belongs to the NV2A thread**, not to the port's main thread: the executor (and so `vk_flip`, the
  lazy `ready()` that creates the window, and the SDL event pump) runs on the kernel's NV2A ack thread. A boot
  video presented from the main thread before the title would have created the window there, and from then on the
  title's flips would pump an event queue that never gets the window's messages (Windows queues them per thread).
  So the main thread decodes and hands frames over; the NV2A thread presents them through `g_xbox_nv2a_idle_hook`.
  Before any new "present something before the title" feature: find the thread that calls the backend's flip.
- **pl_mpeg:** `plm_probe` walks the stream and then seeks back to where it started. A buffer made with
  `plm_buffer_create_with_capacity` + a load callback cannot seek (ring mode: a seek clears it), so the read data
  was lost and the first test said "no MPEG-1 video stream". Use `plm_buffer_create_with_callbacks` with load /
  seek / tell on the file (here an SDL_IOStream, for UTF-8 paths) - it re-reads from the seek target. Also:
  `pl_mpeg.h` names `FILE`, so `<stdio.h>` comes before it.
- **Test without taking the desktop:** keys go to the game window with PostMessage(WM_KEYDOWN) - no focus change
  (used to prove the boot animation ignores them: it cannot be skipped, maintainer 2026-10-27); captures with PrintWindow (the window's own content - CopyFromScreen had caught other windows on
  top, the maintainer's screen among them).

### No system tools, no connectivity pre-checks on the first-start path (`src/launcher`, release v0.9.1)
- **What happened (v0.9.0 testers, Linux with Proton / Wine, the Windows build):** the launcher said "no internet?"
  while fetching llvm-mingw, and unpacking the compiler then failed because it wanted `tar.exe`. The launcher had run
  Windows' own `System32\curl.exe` and `tar.exe` (there since Windows 10 1803): Wine has neither, so the process
  could not start and the guessed message blamed the network.
- **Rule (maintainer, 2026-10-27; every port's release):** shipped code calls **no external system tools**
  (curl, tar, powershell, certutil, cmd ...) - only programs we ship or download ourselves; and **never asks "is the
  network up"** before a download (Wine's answers to those APIs are often wrong) - it tries and reports the real error
  class (DNS / connect / TLS / HTTP status, `http_get.c`) with *Retry*. Download in-process with WinHTTP (on Wine it
  maps to the host's GnuTLS and CA store); unpack in-process with our zip reader, **streamed**
  (`zip_extract_to_file`): the 64 MB in-memory cap of `zip_extract` is for shader packs, libLLVM-23.dll alone is 78 MB.
- **A message must not guess:** "no internet?" was a guess from a failed process start. Carry the real cause up to
  the box (and into `data\launcher.log`).
