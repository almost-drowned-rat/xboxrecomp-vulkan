# Test plan – @TITLE@

Every game mode and feature, one line each, with how it is checked (input script + screenshots where possible).

| Area | Item | How | Status |
|---|---|---|---|
| Boot | logos/movies, title screen | `tests/input/main_menu.txt` | |
| Menus | every main-menu entry and back | input script + `shot=` per step | |
| Saves | save, load, overwrite, interrupted save, delete `data\` | storage test script | |
| Audio | menu music, sound effects | listen + log | |
| Game | each mode, 10 minutes | input script | |
