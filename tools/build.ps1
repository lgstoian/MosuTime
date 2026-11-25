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
    [switch]$MergeBin,
    [switch]$Clean
)

# ESP-IDF and CMake write normal progress and warnings to stderr.  PowerShell
# 5.1 turns that into a terminating NativeCommandError under "Stop", so this
# wrapper keeps normal error handling and checks $LASTEXITCODE explicitly
# after every native command instead.
$ErrorActionPreference = "Continue"

if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    throw "ESP-IDF not found at '$IdfPath'. Pass -IdfPath <path to esp-idf>."
}

. (Join-Path $IdfPath "export.ps1")

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "ESP-IDF environment did not provide idf.py."
}

# An espup/rustup Xtensa toolchain earlier in PATH shadows the one ESP-IDF
# expects, and configure then fails the version check.  Put the IDF-managed
# toolchain for the recommended version at the front.
$toolsPath = if ($env:IDF_TOOLS_PATH) {
    $env:IDF_TOOLS_PATH
} else {
    Join-Path $env:USERPROFILE ".espressif"
}
$toolsJson = Get-Content (Join-Path $IdfPath "tools\tools.json") -Raw |
    ConvertFrom-Json
$xtensaTool = $toolsJson.tools | Where-Object { $_.name -eq "xtensa-esp-elf" }
$xtensaVersion =
    ($xtensaTool.versions | Where-Object { $_.status -eq "recommended" }).name
if ($xtensaVersion) {
    $xtensaBin = Join-Path $toolsPath "tools\xtensa-esp-elf\$xtensaVersion\xtensa-esp-elf\bin"
    if (Test-Path (Join-Path $xtensaBin "xtensa-esp32s3-elf-gcc.exe")) {
        $env:PATH = "$xtensaBin;$env:PATH"
    }
}

Push-Location (Join-Path $PSScriptRoot "..")
try {
    if ($Clean) {
        idf.py fullclean
    }
    # Fetch managed components first, then re-apply the 50 ms LVGL wait floor.
    # managed_components/ is ignored, so this patch must be reproducible from
    # a fresh checkout; a plain `idf.py build` alone would silently drop it.
    idf.py reconfigure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & python (Join-Path $PSScriptRoot "patch_lvgl.py")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    idf.py build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($MergeBin) {
        idf.py merge-bin
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

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
