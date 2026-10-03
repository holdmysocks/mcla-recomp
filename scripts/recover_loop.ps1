# Run the game repeatedly; each time it stops on an unregistered indirect-call
# target, record the address in config/runtime_discovered.toml, regenerate and rebuild.
# Stops when a run survives -Seconds or fails for another reason.
param([int]$Seconds = 60, [int]$MaxIterations = 20)
$root = Split-Path $PSScriptRoot -Parent
$hints = "$root\config\runtime_discovered.toml"
for ($i = 1; $i -le $MaxIterations; $i++) {
    $out = & "$PSScriptRoot\smoke_run.ps1" -Seconds $Seconds -Shots 2 | Out-String
    $log = ($out | Select-String -Pattern "log: (.+)").Matches[0].Groups[1].Value.Trim()
    $fatal = Select-String -Path $log -Pattern "unregistered function at guest address 0x([0-9A-Fa-f]{8})" | Select-Object -First 1
    if (-not $fatal) { "iteration ${i}: no unregistered-function stop"; $out; break }
    $addr = $fatal.Matches[0].Groups[1].Value.ToUpper()
    if (Select-String -Path "$root\config\*.toml" -Pattern "0x$addr" -Quiet) { "iteration ${i}: 0x$addr is already hinted but still unregistered; stopping"; break }
    "iteration ${i}: adding 0x$addr"
    Add-Content $hints "[functions.`"0x$addr`"]`nname = `"sub_$addr`"`n"
    & "$PSScriptRoot\build.ps1" | Out-Null
}
