# assets/ – the port's own icon (placeholder slot)

Every port supplies its own icon here (maintainer, 2026-10-27; porting guide M6, "Icon"):

| File | Use |
|---|---|
| `icon.ico` | the exe's icon in Explorer: 16, 32, 48 and 256 px (PNG entries), embedded at build time through `game.rc` (`1 ICON "icon.ico"`, compiled with `llvm-rc`) |
| `icon_64.bmp` | the window / taskbar icon: 64x64, 32-bit with alpha; CMake embeds its bytes into a generated header and `main.c` hands them to the toolkit (`xbox_set_window_icon`) before the first window, so the game window and the installer's progress window both show it (`SDL_SetWindowIcon`) |

**Rules:** an original, neutral design (THPS 2X: a plain skateboard) - never the game's logo, cover art, title
lettering or any publisher / developer branding. The maintainer sees the icon before it goes into a release.

Until a port has its own files, the build leaves the default icon in place (no `game.rc`, no `xbox_set_window_icon`).
