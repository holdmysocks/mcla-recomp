# Regenerate code from the user's XEX and build.  Usage: .\scripts\build.ps1 [-Preset win-amd64-release]
param([string]$Preset = "win-amd64-release", [switch]$NoCodegen)
. "$PSScriptRoot\env.ps1"
$root = Split-Path $PSScriptRoot -Parent
# Windows PowerShell 5.1 turns a program's redirected stderr into errors, which
# a caller's "Stop" would end the script on; the exit codes are what count.
$ErrorActionPreference = "Continue"
Push-Location $root
try {
    New-Item -ItemType Directory -Force "$root\out" | Out-Null
    if (-not $NoCodegen) {
        & "$env:REXSDK\bin\rexglue.exe" codegen mcla_manifest.toml 2>&1 | ForEach-Object { "$_" } > "$root\out\codegen.log"
        if ($LASTEXITCODE -ne 0) { Get-Content "$root\out\codegen.log" | Select-Object -Last 20; throw "codegen failed" }
        Select-String -Path "$root\out\codegen.log" -Pattern "Codegen summary" | ForEach-Object { $_.Line }
    }
    if (-not (Test-Path "$root\out\build\$Preset\build.ninja")) {
        cmake --preset $Preset "-DCMAKE_PREFIX_PATH=$env:REXSDK" 2>&1 | ForEach-Object { "$_" } > "$root\out\configure.log"
        if ($LASTEXITCODE -ne 0) { Get-Content "$root\out\configure.log" | Select-Object -Last 20; throw "configure failed" }
    }
    cmake --build --preset $Preset 2>&1 | ForEach-Object { "$_" } > "$root\out\build.log"
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Select-String -Path "$root\out\build.log" -Pattern "error:|FAILED:|lld-link" | Select-Object -First 15 | ForEach-Object { $_.Line }
        throw "build failed"
    }
    Get-Content "$root\out\build.log" | Select-Object -Last 1
} finally { Pop-Location }
