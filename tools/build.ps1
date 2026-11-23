# Builds tools/undo-peek.exe with an llvm-mingw clang++.
# Usage: .\tools\build.ps1 [-Clang path\to\clang++.exe]
param([string]$Clang = "clang++")

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$windres = Join-Path (Split-Path -Parent (Get-Command $Clang).Source) "llvm-windres.exe"

& $windres "$here\undo-peek.rc" -O coff -o "$here\undo-peek.res.o"
& $Clang -std=c++20 -O2 -municode -Wall -Wno-unknown-pragmas `
    "$here\undo-peek.cpp" "$here\undo-peek.res.o" `
    -o "$here\undo-peek.exe" `
    -lole32 -loleaut32 -lshlwapi -luuid -static
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item "$here\undo-peek.res.o"
Write-Host "Built $here\undo-peek.exe"
