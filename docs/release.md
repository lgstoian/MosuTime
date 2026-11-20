# Releases

Two artefacts are published together:

| File | What it is |
| --- | --- |
| `MosuTime-<tag>-merged.bin` | Bootloader + partition table + application, merged at offset `0x0`. Flash it with the manager's **Flash image** or with `esptool`. |
| `MosuTimeManager.exe` | Self-contained Windows 11 manager (console, dashboard, settings editor, SD card, flashing). No Python installation needed. |
| `MosuTime-<tag>-sd-card.zip` | `mosu.cfg` and the 79 voice clips for the card. |
| `SHA256SUMS.txt` | Hashes of the three files above. |

Nothing under `release/` is committed; the release is built from the tag.

## Build a release

```powershell
git tag -a v1.0.0 -m "MosuTime v1.0.0"
.\tools\release.ps1                    # firmware + SD zip + exe + hashes
.\tools\release.ps1 -SkipBuild         # repackage an existing build
```

`release.ps1` calls `tools/build.ps1 -MergeBin` (ESP-IDF 5.5.x), then
PyInstaller, then `Compress-Archive`. The firmware version string comes from
`git describe`, so tag before building: the watch then reports `v1.0.0` in
`version`.

The script refuses to package when `sd_card/voice` has no WAV files. Generate
them first with `python tools/gen_voice.py --out sd_card/voice`.

## Publish

```powershell
gh release create v1.0.0 `
  release/MosuTime-v1.0.0-merged.bin `
  release/MosuTimeManager.exe `
  release/MosuTime-v1.0.0-sd-card.zip `
  release/SHA256SUMS.txt `
  --title "MosuTime v1.0.0" --notes-file docs/release-notes-v1.0.0.md
```

Keep the real Wi-Fi password out of every tracked file: the shipped
`sd_card/mosu.cfg` uses the template `Acasa` / `parola`. The wearer's network is
entered in the manager's **Settings** tab or passed to `tools/push_sd.py`.

## Verify a download

```powershell
Get-FileHash .\MosuTimeManager.exe -Algorithm SHA256
```

Compare with `SHA256SUMS.txt`. The manager's **Firmware → Flash image** checks
the port, closes it, writes the image at `0x0` and reconnects; NVS and the SD
card are not touched.

## Installing the manager

Windows 11 blocks unsigned executables from the internet with SmartScreen.
Choose **More info → Run anyway**, or unblock the file first:

```powershell
Unblock-File .\MosuTimeManager.exe
```

The manager stores its editable `mosu.cfg` copy in
`%LOCALAPPDATA%\MosuTime\mosu.cfg`; the voice clips and the original template
are inside the exe. When the exe sits inside a checkout, it also offers the
source build and flash buttons.
