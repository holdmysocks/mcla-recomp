# Play session with automatic function recovery.
# Launches the game and waits for it to exit. If it stopped on an unregistered
# indirect-call target, the address is added to config/runtime_discovered.toml,
# the game is rebuilt (about a minute) and relaunched. Close the game window
# normally to end the session.
param([string]$Preset = "win-amd64-release", [string[]]$ExtraArgs = @(), [int]$MaxRestarts = 40,
      [switch]$BigTextureCache)
# -BigTextureCache raises the GPU texture cache limits (SDK defaults: 384 MB soft,
# 768 MB hard, 24 MB render-to-texture) to test whether evictions cause hitches.
if ($BigTextureCache) {
    $ExtraArgs += "--texture_cache_memory_limit_soft=1536", "--texture_cache_memory_limit_hard=2048",
                  "--texture_cache_memory_limit_render_to_texture=64"
}
$root = Split-Path $PSScriptRoot -Parent
$build = "$root\out\build\$Preset"
$runs = "$root\out\runs"
$hints = "$root\config\runtime_discovered.toml"
New-Item -ItemType Directory -Force $runs | Out-Null
$plugin = "$root\third_party\rexglue-sdk\out\install\win-amd64\bin\rexgpu-xenos.dll"
if (Test-Path $plugin) { Copy-Item $plugin $build -Force }

for ($i = 0; $i -le $MaxRestarts; $i++) {
    $log = "$runs\play-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
    $argList = @("--game_data_root=$root\game", "--gpu_plugin=xenos", "--fullscreen=false",
                 "--log_file=$log", "--log_level=info") + $ExtraArgs
    # Per-frame counters for scripts/analyze_perf.py, one file per launch.
    $env:REX_PERF_LOG_CSV = $log -replace '\.log$', '.csv'
    $start = Get-Date
    $p = Start-Process "$build\mcla.exe" -ArgumentList $argList -WorkingDirectory $build -PassThru
    $p.WaitForExit()
    $mins = [Math]::Round(((Get-Date) - $start).TotalMinutes, 1)
    $fatal = Select-String -Path $log -Pattern "unregistered function at guest address 0x([0-9A-Fa-f]{8})" | Select-Object -First 1
    if (-not $fatal) {
        "session ended after $mins min, exit code $($p.ExitCode), log $log"
        Select-String -Path $log -Pattern "null-page|Unhandled guest|\[critical\]" | Select-Object -First 3 | ForEach-Object { $_.Line }
        break
    }
    $addr = $fatal.Matches[0].Groups[1].Value.ToUpper()
    if (Select-String -Path "$root\config\*.toml" -Pattern "0x$addr" -Quiet) { "0x$addr already hinted but still unregistered; stopping ($log)"; break }
    "after $mins min: missing function 0x$addr, rebuilding and relaunching"
    Add-Content $hints "[functions.`"0x$addr`"]`nname = `"sub_$addr`"`n"
    & "$PSScriptRoot\build.ps1" | Out-Null
}
