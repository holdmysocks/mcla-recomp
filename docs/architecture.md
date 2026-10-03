# Architecture

## Layout

| Path | Contents | In git |
|---|---|---|
| `mcla_manifest.toml` | ReXGlue project manifest: entrypoint XEX path, hint files | yes |
| `config/*.toml` | Function-recovery hints (see `function-recovery.md`) | yes |
| `src/` | Host application: `MclaApp` (subclass of `rex::ReXApp`), diagnostics, later hooks | yes |
| `scripts/` | User-side extractor, build, smoke-run and recovery-loop scripts | yes |
| `ps5/` | PS5 probes and, later, the PS5 platform layer | yes |
| `docs/` | These notes | yes |
| `generated/rexglue.cmake` | SDK CMake boilerplate | yes |
| `generated/default/` | Recompiled game code (derived from the user's XEX) | **no** |
| `game/` | Files extracted from the user's disc image | **no** |
| `third_party/` | ReXGlue SDK checkout and reference projects | no (to become a submodule) |
| `tools/` | Portable LLVM | no |
| `out/` | Build trees, logs, run captures | no |

## Pipeline

1. `scripts/extract_game.py <iso>` walks XDVDFS and copies the game files to `game/`. No decryption.
2. `rexglue codegen mcla_manifest.toml` loads `game/default.xex` (decrypting in memory with the SDK's own key), analyses it, and writes C++ to `generated/default/`.
3. CMake builds `mcla_recomp` (the generated code) and `mcla` (host app), linked against `rexruntime` from the SDK. The Xenos GPU implementation is a plugin, `rexgpu-xenos`, loaded at start with `--gpu_plugin=xenos`.

## Runtime (from ReXGlue, Xenia-derived)

- Guest memory: one 0x120000000-byte shared mapping. Guest virtual addresses are views at host `0x100000000 + guest`, and the 512 MiB of physical memory is aliased into several guest ranges.
- Guest code: each PPC function is a host function taking a CPU context and the memory base. Indirect calls go through a per-module function table indexed by guest address; a miss is fatal.
- GPU: Xenia's Xenos command processor with D3D12 and Vulkan backends. It tracks guest writes to GPU-visible memory with page protection and fault handling.
- Audio: XMA decode through FFmpeg, output through SDL.
- Kernel and XAM: Xenia's high-level reimplementation.

## Targets

- **PC**: Windows today (D3D12 by default). Vulkan and Linux are next, because Vulkan is the only backend the PS5 can use.
- **PS5**: not started. Plan is a platform layer for the runtime's POSIX files (16 files, about 3,700 lines) plus a Vulkan driver for PS5 homebrew. `generated/default/` and `src/` are shared with PC unchanged.

## Licence

GPL-3.0 is planned for this repository (required by the PS5 SDK and Vulkan drivers). ReXGlue is BSD-3 and stays under its own licence in `third_party/`.
