# Plan: porting LARecomp's fixes and options

Requested by the user on 2026-10-03: bring over all stability fixes and improvements, all optional quality-of-life options, DLC content handling, an in-game menu for the options, and PlayStation button prompts. LARecomp is used with its author's permission.

LARecomp's `src/mc_engine` is about 37,000 lines. Roughly 17,000 of that is the mod loader, custom music, cutscene gallery, online play and a native renderer, which are out of scope here unless asked for. The parts below are in scope.

## Portability rule

Everything ported must build for PS5 as well as PC. LARecomp is Windows-only in places (`windows.h`, Media Foundation, XAudio2). Ported code keeps platform calls behind `#ifdef` or uses the SDK's own facilities.

## Batches, in order

| # | Batch | LARecomp source | Size | Notes |
|---|---|---|---|---|
| 1 | Done: frame timing, 60 FPS corrections, single tile, fence yield | `hooks.cpp` | small | See `known-issues.md` |
| 2 | Crash and freeze guards: menu list null table, pause list vtable, race-reset null brain, map cursor guards, impostor shadow loop, missing-lights skip, traffic chassis bound | `hooks.cpp`, config | small | Each is a few lines plus a hook entry |
| 3 | Remaining runtime performance: cache-flush bypass (`mc_FlushDataCache`), sleep replacement (`mc_Sleep`), resume-thread hook, heap redirection (`[rexcrt]`) | `threading.cpp`, config | small | Measure each with the profiler before keeping it |
| 4 | Graphics options: motion blur, depth of field, MSAA, foliage/impostor shadows, shadow phases, fullscreen blur, aspect ratio, FOV, city/traffic LOD, draw distance, ambient density | `hooks.cpp` | medium | All become `--mcla_*` options first |
| 5 | Gameplay options: speed units, rubber-banding, ride height and wheel fit unlocks, time of day, weather | `hooks.cpp`, `hud_units.cpp`, `time_of_day.cpp`, `real_weather.cpp` | medium | |
| 6 | Button prompts: Xbox or PlayStation glyphs | `hooks.cpp` (`button_prompts`) | small | Both glyph sets already ship in the game's UI files; the hook flips the platform flag the UI scripts read. Default: match the connected controller on PC, PlayStation on PS5 |
| 7 | DLC content: vehicle DLC and content-check hooks | `hooks.cpp` | small | User decision 2026-10-03: include. Only content present in the user's own files can load |
| 8 | In-game settings menu | `pause_menu.cpp` (3,400 lines), `graphics_button.cpp`, `string_table.cpp` | large | Adds a settings entry to the game's own pause/options menu and rows bound to options. Depends on batches 4 to 6 existing as options |
| 9 | Language selection, save import | `string_table.cpp`, app header | small | |

## What the menu can and cannot offer

LARecomp's menu already has rows for: FPS limit, vsync, resolution and scale, MSAA, motion blur, depth of field, shadows and foliage, LOD, traffic and pedestrian density, speed units, camera smoothing, FOV, time of day, weather, language.

Not in LARecomp, would be new work:

- **Separate volume levels** for voices, music, ambient and car sounds. The game's audio engine has categories (it loads a `driverSettings.xml` and wave banks by type), so per-category volume is plausible, but the control points have not been located. The game's own options menu may already expose some of this; to be checked.
- **Camera shake.** No hook exists; the camera code is partly mapped (`camBoomCS::Update`), so finding the shake term is a bounded search.

## Open questions

- Intro movies at more than 30 FPS (see `known-issues.md`).
- Whether the settings menu's Scaleform hooks are stable enough to carry to PS5 unchanged; they are game-side, so they should be.
