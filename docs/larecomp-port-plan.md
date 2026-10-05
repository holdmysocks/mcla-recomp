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
| CONTROLS | BUTTON PROMPTS (AUTO, XBOX, PLAYSTATION), INTRO (NORMAL, FAST, SKIP (NO LOGO)) |

Changes apply immediately and are saved to `mcla.toml` beside the executable when the submenu is closed. Like LARecomp, it also enables the game's developer-only save button as "SAVE GAME".

Motion blur and depth of field are new options here (`--mcla_motion_blur`, `--mcla_depth_of_field`), implemented with LARecomp's hooks.

Left out of the port: LARecomp's carbon-fibre, cutscene, language, FidelityFX and native-renderer tabs, its deferred restart-only settings, and the extra row on the game's controller options screen.

**Status: builds, and the game boots with it compiled in. The menu itself has not been opened or exercised**; that needs someone at the controller. Things to check: the three buttons appear under Settings, each opens, checkboxes toggle with accept, value rows step with left/right and accept, back returns to the Settings tab with its normal rows, and the pause menu still works after closing and reopening.

## Second look at LARecomp, and the detail options (2026-10-05)

Read at its commit of 2026-10-04. What "runs above 144 FPS" rests on there:

- The simulation is correct at any frame rate. That is the timing, camera and chassis hooks, all of which are here already (our 27 hooks are a subset of its 160, at the same addresses).
- Its own notes say the game "holds 60", and 80-88 FPS in dense areas after removing a per-packet timer from the unpublished ReXGlue build it is developed against. Stock 0.10.0, which we use, has no such timer in the graphics path.
- The route to much higher rates there is a native Direct3D 12 renderer (about 36,000 lines, `src/native_gfx`) that replaces the emulated GPU. It is off by default, still changing daily, and Windows-only.

Looked at and found not to apply here: its sleep and resume-thread replacements (the sleep hook fixed a busy-wait in its own earlier hook; here the game's sleep goes through the SDK's ordinary delay); most of the runtime settings it applies (async shader compilation, bindless, readback, primary-buffer submission are the SDK's defaults already); the larger texture cache (tried here on 2026-10-03, misses did not fall; see `known-issues.md`); guards for features we do not have (mouse on the map, mod loader).

Ported as `src/perf_options.cpp` and `config/perf_options.toml`, every option defaulting to the game as shipped:

| Option | Menu row (PERFORMANCE) | Takes effect |
|---|---|---|
| `mcla_shadows` | SHADOWS | at once |
| `mcla_foliage_shadows` | FOLIAGE SHADOWS | at once |
| `mcla_city_lod`, `mcla_traffic_lod` | CITY / TRAFFIC DETAIL DISTANCE | at once |
| `mcla_traffic_distance`, `mcla_pedestrians`, `mcla_parked_cars` | TRAFFIC DISTANCE, PEDESTRIANS, PARKED CARS | at once |
| `mcla_race_shadows`, `mcla_fast_car_shadows`, `mcla_fullscreen_blur`, `mcla_msaa` | RACE SHADOWS, SIMPLE CAR SHADOWS, FULLSCREEN BLUR, ANTI ALIASING | next start |
| `mcla_foliage_impostors` | DISTANT TREES | next load of the city |

Also there: the cache-flush bypass (`sub_821D5510`, a `dcbf` loop the host does not need), and three guards (a UI movie with no lights node, the racing AI's reset with no brain, the map cursor step with no neighbouring cell) plus the impostor search guard that the DISTANT TREES option needs (it fired in testing: without it the game searches forever).

Measured on PC (Ryzen 7 7800X3D, RTX 4080, Direct3D 12, title screen, uncapped). The title screen's camera cycles through views at random, so separate runs cannot be compared (the same settings gave 3,700 to 6,800 draws a frame). `--mcla_ab_test=<option>` switches one option every 4 s within a run and logs both states:

| Option reduced | Frame time, as shipped | reduced | Draws a frame, as shipped | reduced |
|---|---|---|---|---|
| Shadows off | 20.5 ms | 18.4 ms | 6,747 | 5,515 |
| Foliage shadows off | 18.9 ms | 18.4 ms | 6,894 | 6,603 |
| Detail distances 0.5 | 18.3 ms | 14.4 ms | 4,969 | 3,920 |
| Traffic 250 m, pedestrians and parked cars 0.5 | 18.2 ms | 17.4 ms | 6,284 | 5,787 |
| All four | 22.2 ms | 11.2 ms | 6,899 | 3,257 |

Frame time follows draws (about 2 us a draw plus about 2.6 ms a frame); no run went above about 112 FPS however few the draws, so the game's own thread is a second ceiling near 9 ms. The next-start options cannot be measured this way. On PS5 (PS5 Pro, play build, 2026-10-05) the user reports that everything works as before and that the options made no difference to the frame rate that could be felt, better or worse. No figures: the play build does not log frame counts, so whether the options take effect there, and what they do to the draw count in the map view, is still to be measured with a test build.
