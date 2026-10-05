# One command from your own disc image to the game running on Windows.
#
#   .\scripts\make_pc.ps1 -Iso C:\path\to\your.iso
#
# Needs Git, CMake, Ninja, Python 3, Visual Studio Build Tools 2022 (C++
# workload) and Clang 20; docs\pc-build-guide.md says where to get them.
# Everything is built on your machine from your own copy of the game: nothing
# of the game is in this repository, and what this builds is not for sharing.
#
# Each step is skipped when its result is already there, so after a failure,
# or after `git pull`, run the same command again and it continues.
#
#   -Iso PATH    your disc image (Midnight Club: Los Angeles Complete Edition,
#                USA/Europe, Xbox 360). Not needed once game\ is extracted.
#   -Play        start the game when the build is done
#   -Preset      CMake preset of the game build (default win-amd64-release)
param([string]$Iso = "", [switch]$Play, [string]$Preset = "win-amd64-release")

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$sdk = "$root\third_party\rexglue-sdk"
$sdkTag = "v0.10.0"
$sdkCommit = "f5337cdc947ff6d4c4196737e2c807a48f2a1fc2"
$install = "$sdk\out\install\win-amd64"
$logs = "$root\out\logs"
New-Item -ItemType Directory -Force $logs | Out-Null

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" }
function Fail([string]$text) { Write-Host ""; Write-Host "FAILED: $text" -ForegroundColor Red; exit 1 }
# Run a command with its output in a log; on failure show the end of the log.
function Logged([string]$name, [scriptblock]$command) {
    $log = "$logs\$name.log"
    & $command *> $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 25 | ForEach-Object { Write-Host $_ }
        Fail "$name (full log: $log)"
    }
}

Push-Location $root
try {
    # -----------------------------------------------------------------------
    Step "1/6 tools"
    . "$PSScriptRoot\env.ps1"
    $missing = @()
    foreach ($tool in "git", "cmake", "ninja", "python", "clang") {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { $missing += $tool }
    }
    if ($missing) { Fail "not found: $($missing -join ', '). See 'What you need' in docs\pc-build-guide.md" }
    "clang: $((clang --version | Select-Object -First 1))"

    # -----------------------------------------------------------------------
    Step "2/6 ReXGlue SDK $sdkTag with this project's patches"
    if (-not (Test-Path "$sdk\.mcla-patched")) {
        if (-not (Test-Path "$sdk\.git")) {
            Logged "sdk-clone" { git clone --recursive --branch $sdkTag https://github.com/rexglue/rexglue-sdk.git $sdk }
        }
        if ((git -C $sdk rev-parse HEAD) -ne $sdkCommit) { Fail "the SDK checkout is not $sdkTag ($sdkCommit)" }
        Logged "sdk-patch" { git -C $sdk apply "$root\patches\rexglue-v0.10.0-mcla.patch" }
        Logged "sdk-patch-ffmpeg" { git -C "$sdk\thirdparty\FFmpeg" apply "$root\patches\rexglue-ffmpeg-ps5-config.patch" }
        # Git on Windows checks out symbolic links as small text files holding
        # the link's target; one of the SDK's libraries has some. Replace them
        # with copies of what they point to.
        $links = "$sdk\thirdparty\libmspack\cabextract\mspack"
        Get-ChildItem $links -File | Where-Object { $_.Length -lt 200 } | ForEach-Object {
            $target = Join-Path $links (Get-Content $_.FullName -Raw).Trim()
            if (Test-Path $target) { Copy-Item $target $_.FullName -Force }
        }
        New-Item -ItemType File "$sdk\.mcla-patched" | Out-Null
    } else {
        "already patched: $sdk"
    }

    # -----------------------------------------------------------------------
    Step "3/6 build and install the SDK (the long step, once)"
    $marker = Get-Item "$sdk\.mcla-patched"
    $recompiler = Get-Item "$install\bin\rexglue.exe" -ErrorAction SilentlyContinue
    if (-not $recompiler -or $recompiler.LastWriteTime -lt $marker.LastWriteTime) {
        Push-Location $sdk
        try {
            Logged "sdk-configure" { cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF }
            Logged "sdk-build" { cmake --build out\build\win-amd64 --config Release }
            Logged "sdk-install" { cmake --install out\build\win-amd64 --config Release }
        } finally { Pop-Location }
    } else {
        "already installed: $install"
    }
    if (-not (Test-Path "$install\bin\rexglue.exe")) { Fail "the SDK was not installed" }

    # -----------------------------------------------------------------------
    Step "4/6 your game: extract from the disc image"
    if (-not (Test-Path "$root\game\default.xex")) {
        if (-not $Iso) { Fail "game\ is empty: give your disc image with -Iso" }
        if (-not (Test-Path -LiteralPath $Iso)) { Fail "no such file: $Iso" }
        Logged "extract" { python "$PSScriptRoot\extract_game.py" $Iso "$root\game" }
    } else {
        "already extracted: $root\game"
    }
    if (-not (Test-Path "$root\game\default.xex")) { Fail "game\default.xex is missing after extraction" }

    # -----------------------------------------------------------------------
    Step "5/6 recompile the game's code and build it (several minutes the first time)"
    & "$PSScriptRoot\build.ps1" -Preset $Preset
    $build = "$root\out\build\$Preset"
    if (-not (Test-Path "$build\mcla.exe")) { Fail "mcla.exe was not built (log: $root\out\build.log)" }
    Copy-Item "$install\bin\rexgpu-xenos.dll" $build -Force

    # -----------------------------------------------------------------------
    Step "6/6 done"
    "game: $build\mcla.exe"
    "play: .\scripts\play_loop.ps1     (or see 'Play' in docs\pc-build-guide.md)"
    if ($Play) { & "$PSScriptRoot\play_loop.ps1" -Preset $Preset }
} finally { Pop-Location }
