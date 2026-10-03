# Launch the Windows build for a fixed time, capture window screenshots, then stop it.
# Usage: .\scripts\smoke_run.ps1 [-Seconds 60] [-Shots 3] [-ExtraArgs "--foo=bar"]
param(
    [int]$Seconds = 60,
    [int]$Shots = 3,
    [string]$Preset = "win-amd64-release",
    [string[]]$ExtraArgs = @()
)
$root = Split-Path $PSScriptRoot -Parent
$build = "$root\out\build\$Preset"
$runs = "$root\out\runs"
New-Item -ItemType Directory -Force $runs | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = "$runs\$stamp.log"

$plugin = "$root\third_party\rexglue-sdk\out\install\win-amd64\bin\rexgpu-xenos.dll"
if (Test-Path $plugin) { Copy-Item $plugin $build -Force }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[Win]::SetProcessDPIAware() | Out-Null

$argList = @("--game_data_root=$root\game", "--gpu_plugin=xenos", "--fullscreen=false",
             "--log_file=$log") + $ExtraArgs
if (-not ($ExtraArgs -match "--log_level")) { $argList += "--log_level=info" }
$p = Start-Process "$build\mcla.exe" -ArgumentList $argList -WorkingDirectory $build -PassThru
$interval = [Math]::Max(1, [int]($Seconds / $Shots))
for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep -Seconds $interval
    $p.Refresh()
    if ($p.HasExited) { "process exited early, code $($p.ExitCode)"; break }
    if ($p.MainWindowHandle -ne 0) { [Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 400 }
    $r = New-Object Win+RECT
    # The capture copies screen pixels, so only take it when the game is in front.
    if ($p.MainWindowHandle -ne 0 -and [Win]::GetForegroundWindow() -ne $p.MainWindowHandle) {
        "shot ${i}: skipped, game window is not in front"
    } elseif ($p.MainWindowHandle -ne 0 -and [Win]::GetWindowRect($p.MainWindowHandle, [ref]$r) -and ($r.R - $r.L) -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $bmp.Save("$runs\$stamp-$i.png"); $g.Dispose(); $bmp.Dispose()
        "shot $i at $($i * $interval)s: $runs\$stamp-$i.png"
    } else { "shot ${i}: no window" }
}
$p.Refresh()
if (-not $p.HasExited) {
    "threads=$($p.Threads.Count) workingset={0:N0}MB cpu={1:N1}s" -f ($p.WorkingSet64 / 1MB), $p.TotalProcessorTime.TotalSeconds
    $p.Kill()
}
"log: $log"
