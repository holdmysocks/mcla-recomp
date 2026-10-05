# One command from your own disc image to the game running on Windows.
#
#   .\scripts\make_pc.ps1 -Iso C:\path\to\your.iso
#
# Needs Git, CMake, Ninja, Python 3, Visual Studio Build Tools 2022 (C++
# workload) and Clang 20. Whatever is missing, the script offers to install
# (winget, and Clang from the LLVM project's releases); docs\pc-build-guide.md
# lists them for doing it by hand.
# Everything is built on your machine from your own copy of the game: nothing
# of the game is in this repository, and what this builds is not for sharing.
#
# Each step is skipped when its result is already there, so after a failure,
# or after `git pull`, run the same command again and it continues.
#
#   -Iso PATH    your disc image (Midnight Club: Los Angeles Complete Edition,
#                USA/Europe, Xbox 360). Not needed once game\ is extracted.
#   -Play        start the game when the build is done
#   -InstallTools  install missing tools without asking first
#   -Preset      CMake preset of the game build (default win-amd64-release)
param([string]$Iso = "", [switch]$Play, [switch]$InstallTools, [string]$Preset = "win-amd64-release")

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
    # Windows PowerShell 5.1 turns a program's redirected stderr into errors,
    # which "Stop" would end the script on; the exit code is what counts.
    $ErrorActionPreference = "Continue"
    & $command 2>&1 | ForEach-Object { "$_" } > $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 25 | ForEach-Object { Write-Host $_ }
        Fail "$name (full log: $log)"
    }
}

Push-Location $root
try {
    # -----------------------------------------------------------------------
    Step "1/6 tools"
    # What the build needs, how to tell that it is there, and how to get it.
    # Programs come from winget (part of Windows 10 and 11); Clang is the
    # portable build from the LLVM project, unpacked into tools\.
    $llvmVersion = "20.1.8"
    $llvmName = "clang+llvm-$llvmVersion-x86_64-pc-windows-msvc"
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    function HasBuildTools {
        if (-not (Test-Path $vswhere)) { return $false }
        [bool](& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    }
    function RefreshPath {
        $env:PATH = [Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" +
                    [Environment]::GetEnvironmentVariable("PATH", "User")
        . "$PSScriptRoot\env.ps1"
    }
    function MissingTools {
        RefreshPath
        $list = @()
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $list += "Git" }
        if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $list += "CMake" }
        if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { $list += "Ninja" }
        # The Microsoft Store's placeholder python.exe is not Python.
        $python = Get-Command python -ErrorAction SilentlyContinue
        if (-not $python -or $python.Source -like "*WindowsApps*") { $list += "Python" }
        if (-not (HasBuildTools)) { $list += "Visual Studio Build Tools" }
        if (-not (Get-Command clang -ErrorAction SilentlyContinue)) { $list += "Clang" }
        $list
    }
    $missing = @(MissingTools)
    if ($missing) {
        "missing: $($missing -join ', ')"
        if (-not $InstallTools) {
            $answer = Read-Host "Install them now? Programs come from winget, Clang from the LLVM project's releases; the Build Tools are several GB and ask for administrator rights. [y/N]"
            if ($answer -notmatch '^[yY]') { Fail "install them (see 'What you need' in docs\pc-build-guide.md) and run this again, or run with -InstallTools" }
        }
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is not available; install the tools by hand (docs\pc-build-guide.md)" }
        $ids = @{ "Git" = "Git.Git"; "CMake" = "Kitware.CMake"; "Ninja" = "Ninja-build.Ninja"; "Python" = "Python.Python.3.12" }
        foreach ($name in $missing) {
            if ($ids.ContainsKey($name)) {
                "installing $name"
                Logged "install-$name" { winget install --id $ids[$name] -e --accept-source-agreements --accept-package-agreements }
            } elseif ($name -eq "Visual Studio Build Tools") {
                "installing Visual Studio Build Tools with the C++ workload (this takes a while)"
                Logged "install-build-tools" {
                    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements `
                        --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
                }
            } elseif ($name -eq "Clang") {
                "downloading Clang $llvmVersion (about 900 MB) into tools\"
                New-Item -ItemType Directory -Force "$root\tools" | Out-Null
                $archive = "$root\tools\$llvmName.tar.xz"
                Logged "install-clang" {
                    curl.exe -L --fail -o $archive "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvmVersion/$llvmName.tar.xz"
                    if ($LASTEXITCODE -eq 0) { tar.exe -xf $archive -C "$root\tools" }
                }
                Remove-Item -LiteralPath $archive -ErrorAction SilentlyContinue
            }
        }
        $missing = @(MissingTools)
        if ($missing) { Fail "still missing after installing: $($missing -join ', '). A new PowerShell window may be needed; then run this again" }
    }
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
    # A shortcut in the repository folder, so that playing is a double-click.
    # mcla.exe finds the game folder and its graphics plugin by itself.
    $shortcutPath = "$root\Midnight Club Los Angeles.lnk"
    $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
    $shortcut.TargetPath = "$build\mcla.exe"
    $shortcut.WorkingDirectory = $build
    $shortcut.Save()
    "game:     $build\mcla.exe"
    "shortcut: $shortcutPath"
    "To play, double-click either one."
    if ($Play) { Start-Process "$build\mcla.exe" -WorkingDirectory $build }
} finally { Pop-Location }
