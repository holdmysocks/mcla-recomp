# Run the title screen at each frame-rate target and report measured frame times.
# Usage: .\scripts\fps_sweep.ps1 [-Targets 30,60,120] [-Seconds 60] [-ExtraArgs ...]
param([int[]]$Targets = @(30, 60, 120), [int]$Seconds = 60, [string[]]$ExtraArgs = @())
$root = Split-Path $PSScriptRoot -Parent
foreach ($fps in $Targets) {
    $csv = "$root\out\runs\sweep-$fps.csv"
    if (Test-Path -LiteralPath $csv) { Remove-Item -LiteralPath $csv -Confirm:$false }
    $env:REX_PERF_LOG_CSV = $csv
    & "$PSScriptRoot\smoke_run.ps1" -Seconds $Seconds -Shots 1 -ExtraArgs (@("--mcla_fps=$fps") + $ExtraArgs) | Out-Null
    $env:REX_PERF_LOG_CSV = $null
    Start-Sleep -Seconds 2
    if (-not (Test-Path -LiteralPath $csv)) { "target ${fps}: no data"; continue }
    $d = Import-Csv $csv
    # Skip boot and loading: keep the second half of the run.
    $ft = @($d | Select-Object -Skip ([int]($d.Count / 2)) | ForEach-Object { [double]$_.frame_time_us / 1000 } | Sort-Object)
    if ($ft.Count -lt 10) { "target ${fps}: too few frames ($($d.Count))"; continue }
    $med = $ft[[int]($ft.Count * 0.5)]
    "target {0,3}: {1,5} frames, median {2:N2} ms ({3:N1} FPS), p5 {4:N2}, p95 {5:N2}, p99 {6:N2}, max {7:N1}" -f `
        $fps, $ft.Count, $med, (1000 / $med), $ft[[int]($ft.Count * 0.05)], $ft[[int]($ft.Count * 0.95)], $ft[[int]($ft.Count * 0.99)], $ft[-1]
}
