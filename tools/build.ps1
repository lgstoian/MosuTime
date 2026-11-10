# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# Build helper for a Windows development machine.
#
#   .\tools\build.ps1
#   .\tools\build.ps1 -Flash -Port COM5
#   .\tools\build.ps1 -Monitor -Port COM5

param(
    [string]$IdfPath = "C:\esp\v5.5.5\esp-idf",
    [string]$Port = "",
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    throw "ESP-IDF not found at '$IdfPath'. Pass -IdfPath <path to esp-idf>."
}

. (Join-Path $IdfPath "export.ps1")

Push-Location (Join-Path $PSScriptRoot "..")
try {
    if ($Clean) {
        idf.py fullclean
    }
    idf.py build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($Flash) {
        if ($Port) {
            idf.py -p $Port flash
        } else {
            idf.py flash
        }
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    if ($Monitor) {
        if ($Port) {
            idf.py -p $Port monitor
        } else {
            idf.py monitor
        }
    }
} finally {
    Pop-Location
}
