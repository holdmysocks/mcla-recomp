# Launch the game, wait for the title screen, then report how much CPU each
# thread used over a sampling window. Thread names come from the debug log.
# Usage: .\scripts\thread_profile.ps1 [-Fps 0] [-Warmup 40] [-Window 10] [-ExtraArgs ...]
param([int]$Fps = 0, [int]$Warmup = 40, [int]$Window = 10, [string]$Preset = "win-amd64-release",
      [string[]]$ExtraArgs = @())
$root = Split-Path $PSScriptRoot -Parent
$build = "$root\out\build\$Preset"
$log = "$root\out\runs\threads-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
$argList = @("--game_data_root=$root\game", "--gpu_plugin=xenos", "--fullscreen=false",
             "--log_file=$log", "--log_level=debug", "--mcla_fps=$Fps") + $ExtraArgs
$p = Start-Process "$build\mcla.exe" -ArgumentList $argList -WorkingDirectory $build -PassThru
Start-Sleep -Seconds $Warmup
$p.Refresh()
if ($p.HasExited) { "process exited early"; return }
$a = @{}; foreach ($t in $p.Threads) { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
Start-Sleep -Seconds $Window
$p.Refresh()
$b = @{}; foreach ($t in $p.Threads) { $b[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
$p.Kill()

$names = @{}
Select-String -Path $log -Pattern "XThread::Execute thid \d+ \(handle=\w+, '([^']*)', native=([0-9A-Fa-f]+)" | ForEach-Object {
    $names[[Convert]::ToInt32($_.Matches[0].Groups[2].Value, 16)] = $_.Matches[0].Groups[1].Value
}
$rows = foreach ($id in $b.Keys) {
    if ($a.ContainsKey($id)) {
        [pscustomobject]@{ Thread = $id; Name = $names[$id]; Percent = [Math]::Round(100 * ($b[$id] - $a[$id]) / ($Window * 1000), 1) }
    }
}
"CPU per thread over $Window s (100% = one full core), target $Fps FPS:"
$rows | Sort-Object Percent -Descending | Select-Object -First 12 | Format-Table -AutoSize | Out-String
"total: {0:N0}% of one core" -f (($rows | Measure-Object Percent -Sum).Sum)
