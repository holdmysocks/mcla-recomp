# MCLA

Static recompilation of *Midnight Club: Los Angeles Complete Edition* (Xbox 360, title 545407F8) to native code, for PC and for jailbroken PS5 consoles.

**This repository contains no game code, assets or keys.** You supply your own legally obtained copy; a script extracts what is needed on your machine, and the recompiled code is generated locally and never committed.

## Status

| Target | State |
|---|---|
| Windows, D3D12 | Boots; menus, races, garage and upgrades work with audio and controller. 30 FPS with occasional texture-load hitches. |
| Windows, Vulkan | Reaches the title screen. Not tested further. |
| Linux | Not built yet. |
| PS5 | Not started. A console probe shows the guest memory layout is feasible; see `docs/ps5-feasibility.md`. |

Details and open problems: `docs/known-issues.md`.

## How it works

The game's PowerPC code is translated to C++ ahead of time by the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD-3, derived from Xenia), which also provides the Xbox 360 kernel, GPU and audio reimplementation. This repository holds the project manifest, function-recovery hints, a small host application, SDK patches, scripts and notes. See `docs/architecture.md`.

## Building (Windows)

Requirements: Git, CMake 3.25+, Ninja, Clang 20, Visual Studio Build Tools (for the Windows SDK), Python 3.

1. Clone ReXGlue v0.10.0 (`f5337cdc`) with submodules into `third_party/rexglue-sdk` and apply everything in `patches/` with `git apply`.
   On Windows, git checks out the libmspack symlinks as text files; replace them with copies of their targets.
2. Build and install the SDK: `cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF`, then build and install the `Release` configuration.
3. Extract your disc image: `python scripts/extract_game.py <your.iso>`. Files go to `game/`, which is gitignored.
4. `scripts/build.ps1` runs codegen on your `default.xex` and builds `mcla.exe`.
5. `scripts/play_loop.ps1` launches the game. If it stops on a function the recompiler missed, the script records it, rebuilds and relaunches.

`scripts/env.ps1` expects a portable LLVM under `tools/`; adjust it if clang is installed elsewhere.

## Layout

| Path | Contents |
|---|---|
| `config/` | Function-entry hints, each file documenting how its entries were found |
| `src/` | Host application: audio fallback, crash tracer, function-pointer scan |
| `patches/` | Changes to the ReXGlue SDK |
| `scripts/` | Extraction, build, run and analysis scripts |
| `ps5/` | Console probes |
| `docs/` | Recon report, architecture, function recovery, known issues, PS5 feasibility |

## Credits

- ReXGlue SDK by Tom Clay and contributors; Xenia by Ben Vanik and contributors.
- [LARecomp](https://github.com/mzzvxm/LARecomp) by mzzvxm and contributors, consulted as a reference with the author's permission. The `t:` drive mapping and the diagnosis of the SDK vblank timer problem come from that project's notes.
- XenonRecomp by hedge-dev, on which ReXGlue's analysis builds.

## Licence

GPL-3.0-or-later; see `LICENSE`. *Midnight Club* is a trademark of Take-Two Interactive. This project is not affiliated with or endorsed by Rockstar Games or Take-Two.
