# Dot-source to put the project's toolchain on PATH:  . .\scripts\env.ps1
$root = Split-Path $PSScriptRoot -Parent
$llvm = Get-ChildItem "$root\tools" -Directory -Filter "clang+llvm-*" -ErrorAction SilentlyContinue | Select-Object -First 1
$ninja = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$paths = @()
if ($llvm) { $paths += "$($llvm.FullName)\bin" }
if ($ninja) { $paths += $ninja.DirectoryName }
$paths += "C:\Program Files\CMake\bin"
$env:PATH = ($paths -join ";") + ";" + $env:PATH
$env:REXSDK = "$root\third_party\rexglue-sdk\out\install\win-amd64"
