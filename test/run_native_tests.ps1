# Run the host (native) unit tests.
#
# PlatformIO's `native` platform needs a host C++ compiler on PATH. This script
# prepends the MSYS2 MinGW toolchain (if present) for the duration of the run,
# then invokes `pio test -e native`. The firmware build does not need this.

$ErrorActionPreference = 'Stop'

$mingw = 'C:\msys64\mingw64\bin'
if (Test-Path $mingw) {
    $env:PATH = "$mingw;$env:PATH"
} else {
    Write-Warning "MinGW not found at $mingw - relying on whatever g++ is already on PATH."
}

# Locate the PlatformIO CLI: prefer one on PATH, else the default penv install.
$pio = (Get-Command pio -ErrorAction SilentlyContinue).Source
if (-not $pio) {
    $penv = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
    if (Test-Path $penv) { $pio = $penv }
}
if (-not $pio) { throw 'PlatformIO CLI not found (looked on PATH and in ~/.platformio/penv).' }

# Resolve the repo root (this script lives in <root>/test).
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    & $pio test -e native @args
} finally {
    Pop-Location
}
