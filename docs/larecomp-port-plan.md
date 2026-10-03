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

## Later work, requested by the user on 2026-10-03

Not in LARecomp; to be designed and built here after the menu port.

1. **Separate volume controls** for voices, music, ambient and car sounds, adjustable from the in-game menu.
   - Starting points: the audio engine init at `sub_82144B90`, the config it loads (`$/audio/X360/Config/driverSettings.xml`), the wave bank loader hooks LARecomp names `MCLA_Audio_BankOpened` (0x82146030) and `MCLA_Audio_BankError` (0x821461F4), and `mcMusicManager` (pause 0x821EF158, stop 0x821EF1F8).
   - First step: find out what the game's own options menu already exposes, and where category gains are applied in the mixer.
2. **Camera shake toggle or scale**, adjustable from the in-game menu.
   - Starting points: `camBoomCS::Update` (`sub_82320298`), `mcPlayerCamera` (`sub_822B1B58`), main camera update `sub_822C0320`.

## Button prompts: status

Implemented in `src/button_prompts.cpp` and `config/ui.toml` (2026-10-03). `--mcla_button_prompts=auto|xbox|playstation`, default `auto`: on Windows it shows PlayStation glyphs when a Sony controller (USB vendor 054C) is attached, re-checked every two seconds; on PS5 it is always PlayStation. Verified on the title screen: with `--mcla_button_prompts=playstation` the START prompt shows the PlayStation glyph; in `auto` with an Xbox 360 controller attached it shows the Xbox glyph. Not yet checked inside menus, and automatic detection has not been tried with a real Sony controller.

## In-game settings menu: status

Ported on 2026-10-03 as `src/settings_menu.cpp` and `config/settings_menu.toml` (`--mcla_settings_menu`, default on). It adds three submenus and a quit button to the Settings tab of the pause menu:

| Submenu | Rows |
|---|---|
| DISPLAY | FPS TARGET (30, 60, 120, UNCAPPED), VSYNC, FULLSCREEN, MOTION BLUR, DEPTH OF FIELD |
| PERFORMANCE | SINGLE TILE RENDERING, GPU WAIT YIELD |
| CONTROLS | BUTTON PROMPTS (AUTO, XBOX, PLAYSTATION), SKIP INTRO (NO LOGO) |

Changes apply immediately and are saved to `mcla.toml` beside the executable when the submenu is closed. Like LARecomp, it also enables the game's developer-only save button as "SAVE GAME".

Motion blur and depth of field are new options here (`--mcla_motion_blur`, `--mcla_depth_of_field`), implemented with LARecomp's hooks.

Left out of the port: LARecomp's carbon-fibre, cutscene, language, FidelityFX and native-renderer tabs, its deferred restart-only settings, and the extra row on the game's controller options screen.

**Status: builds, and the game boots with it compiled in. The menu itself has not been opened or exercised**; that needs someone at the controller. Things to check: the three buttons appear under Settings, each opens, checkboxes toggle with accept, value rows step with left/right and accept, back returns to the Settings tab with its normal rows, and the pause menu still works after closing and reopening.
