# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# Build a release: firmware image, SD card bundle and the Windows manager.
#
#   .\tools\release.ps1                 # tag from git describe
#   .\tools\release.ps1 -Tag v1.0.0
#   .\tools\release.ps1 -SkipBuild      # reuse build/, only package
#
# Output lands in release/: the merged firmware image, the SD card zip, the
# manager exe and SHA256SUMS.txt.  Nothing in release/ is committed.

param(
    [string]$IdfPath = "C:\esp\v5.5.5\esp-idf",
    [string]$Tag = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not $Tag) {
        $Tag = (git describe --tags --always)
        if ($LASTEXITCODE -ne 0) { throw "git describe failed" }
    }
    $name = "MosuTime-$Tag"
    $out = Join-Path $root "release"
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build.ps1") -IdfPath $IdfPath -MergeBin
        if ($LASTEXITCODE -ne 0) { throw "firmware build failed" }
    }

    $merged = Join-Path $root "build\MosuTime.bin"
    if (-not (Test-Path $merged)) {
        throw "build/MosuTime.bin is missing; build with -MergeBin first"
    }
    Copy-Item $merged (Join-Path $out "$name-merged.bin") -Force

    $zip = Join-Path $out "$name-sd-card.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    if (-not (Get-ChildItem (Join-Path $root "sd_card\voice") -Filter *.wav `
                -ErrorAction SilentlyContinue)) {
        throw "sd_card/voice has no WAV files; run tools/gen_voice.py first"
    }
    Compress-Archive -Path (Join-Path $root "sd_card\mosu.cfg"),
        (Join-Path $root "sd_card\voice") -DestinationPath $zip -Force

    $guiWork = Join-Path $root "build\gui"
    New-Item -ItemType Directory -Force -Path $guiWork | Out-Null
    python -m PyInstaller --noconfirm --clean --onefile --windowed `
        --name MosuTimeManager `
        --add-data "sd_card/mosu.cfg;sd_card" `
        --add-data "sd_card/voice;sd_card/voice" `
        --collect-all esptool `
        --hidden-import serial `
        --hidden-import serial.tools.list_ports `
        --distpath (Join-Path $guiWork "dist") `
        --workpath (Join-Path $guiWork "work") `
        --specpath $guiWork tools/mosu_gui.py
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
    Copy-Item (Join-Path $guiWork "dist\MosuTimeManager.exe") $out -Force

    Get-ChildItem $out -File |
        Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
        Sort-Object Name |
        ForEach-Object {
            $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
            "$hash  $($_.Name)"
        } | Set-Content (Join-Path $out "SHA256SUMS.txt") -Encoding ascii

    Write-Host "release ready in $out"
    Get-ChildItem $out -File | Select-Object Name, Length
} finally {
    Pop-Location
}
