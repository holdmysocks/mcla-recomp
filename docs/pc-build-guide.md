# Building and running the PC version (Windows)

The same game, recompiled for Windows. As with the PS5 version, nothing of the game is in this repository: you build from your own disc image of **Midnight Club: Los Angeles Complete Edition, USA/Europe, Xbox 360** (title id `545407F8`), and what you build is for you, not for sharing.

State: boots, menus, races, garage and upgrades work with audio and a controller, at 30 FPS by default on Direct3D 12. The Vulkan backend reaches the title screen and is otherwise untested. Open problems are in `known-issues.md`.

## What you need

| | |
|---|---|
| Windows 10 or 11, x86-64 | A Direct3D 12 graphics card. Developed on an RTX 4080 and a Ryzen 7 7800X3D. |
| Git | `winget install Git.Git` |
| CMake 3.25 or newer | `winget install Kitware.CMake` (the scripts look in `C:\Program Files\CMake\bin`) |
| Ninja | `winget install Ninja-build.Ninja` |
| Python 3 | `winget install Python.Python.3.12` |
| Visual Studio Build Tools 2022 | With the "Desktop development with C++" workload: it supplies the Windows SDK and the Microsoft C++ libraries. The compiler used is Clang, below. |
| Clang 20 | The portable build `clang+llvm-20.1.8-x86_64-pc-windows-msvc` from the [LLVM releases](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.8), unpacked into `tools\` in this repository. `scripts\env.ps1` puts it on the path; edit that file if your Clang is elsewhere. |
| Disk | About 20 GB: the disc image, the extracted game, the SDK and the build trees. |

All commands below are PowerShell, run from the repository's folder.

## 1. Get the sources

```powershell
git clone https://github.com/holdmysocks/mcla-recomp.git
cd mcla-recomp
git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git third_party\rexglue-sdk
```

The SDK must be v0.10.0 (`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`). Apply this project's patches to it:

```powershell
git -C third_party\rexglue-sdk apply ..\..\patches\rexglue-v0.10.0-mcla.patch
git -C third_party\rexglue-sdk\thirdparty\FFmpeg apply ..\..\..\..\patches\rexglue-ffmpeg-ps5-config.patch
```

One Windows-specific repair. Git on Windows checks out symbolic links as small text files unless it is allowed to create links, and one of the SDK's libraries has fifteen of them. Replace them with copies of the files they point to:

```powershell
$dir = "third_party\rexglue-sdk\thirdparty\libmspack\cabextract\mspack"
Get-ChildItem $dir -File | Where-Object { $_.Length -lt 200 } | ForEach-Object {
    $target = Join-Path $dir (Get-Content $_.FullName -Raw).Trim()
    if (Test-Path $target) { Copy-Item $target $_.FullName -Force }
}
```

## 2. Build and install the SDK

```powershell
. .\scripts\env.ps1
cd third_party\rexglue-sdk
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF
cmake --build out\build\win-amd64 --config Release
cmake --install out\build\win-amd64 --config Release
cd ..\..
```

This is the long step, once. It leaves the recompiler and the runtime in `third_party\rexglue-sdk\out\install\win-amd64`.

## 3. Extract your game

```powershell
python scripts\extract_game.py C:\path\to\your.iso
```

Thirteen files (about 6 GB) go to `game\`, which git ignores. The files are copied as they are; nothing is decrypted or changed.

## 4. Recompile and build

```powershell
.\scripts\build.ps1
```

It runs the recompiler on your `game\default.xex` (the result is in `generated\`) and builds `out\build\win-amd64-release\mcla.exe`. The first build compiles about 120 large files and takes several minutes; later ones only what changed.

## 5. Play

```powershell
.\scripts\play_loop.ps1
```

It starts the game in a window and waits. Should the game stop on a function the recompiler missed (one reached only through a pointer), the script adds that address to `config\runtime_discovered.toml`, rebuilds and starts the game again; if that happens, please send the new entry as an issue or pull request so the next person does not hit it. Close the window to end the session.

To start the game yourself:

```powershell
cd out\build\win-amd64-release
.\mcla.exe --game_data_root=..\..\..\game --gpu_plugin=xenos
```

Useful options: `--fullscreen=true`, `--mcla_fps=60` (30, 60, 120, or 0 for uncapped), `--mcla_intro=fast` (or `skip`), `--log_file=<path>`. An Xbox or PlayStation controller works; the button prompts follow the controller (`--mcla_button_prompts=xbox|playstation|auto`).

In the game, the pause menu has three extra entries: DISPLAY (frame-rate target, vsync, fullscreen, motion blur, depth of field), PERFORMANCE and CONTROLS (button prompts, intro speed). Changes are saved to `mcla.toml` next to `mcla.exe` when you leave the menu.

## Updating

```powershell
git pull
.\scripts\build.ps1
```

If the update changed `patches\`, the SDK has to be patched and built again: in `third_party\rexglue-sdk` run `git checkout .` and `git clean -fd` (and the same in `thirdparty\FFmpeg`), then repeat the patch commands of step 1 and all of step 2.

## If something goes wrong

| Symptom | What to do |
|---|---|
| `cmake` or `ninja` is not found | Run `. .\scripts\env.ps1` in this PowerShell window first (note the leading dot), and check the paths in that file. |
| The SDK build fails in `libmspack` with errors on a line like `../../libmspack/mspack/cab.h` | The symbolic-link repair in step 1 was skipped. |
| `codegen failed` | `game\default.xex` is missing or is not the supported release. The end of `out\codegen.log` says which. |
| `build failed` | The first errors are printed; the whole log is `out\build.log`. |
| The game window closes at once | Start it with `--log_file=run.log --log_level=info` and read the end of that file. |
