# MosuTime v1.0.0

First release of the watch firmware and the Windows manager.

## Firmware

* One face: 12-hour time in large digits, no colon, no leading zero, AM/PM
  carried by colour (white in the morning, amber in the evening).
* Wrist raise and touch wake; the panel and the LVGL tick stay off otherwise,
  and the CPU uses automatic light sleep between IMU samples.
* Double tap speaks the exact hour in Romanian from the SD card.
* Wi-Fi time sync with a configurable cadence (`sync_hours` or
  `sync_per_day`), Europe/Bucharest default, geolocation when enabled, and a
  15-minute presence scan that notices when the watch is home again.
* SD card is optional: `mosu.cfg` on the card, cached in NVS, with built-in
  defaults if both are missing. A missing card never stops the boot.
* The watch formats its own card and receives files over USB
  (`sd format yes`, `receive`, `md`, `ls`, `rm`), used by the manager and
  `tools/push_sd.py`.
* AXP2101 charger set for the fitted 402728 cell (400 mAh): 200 mA, 4.2 V,
  25 mA termination, 50 mA precharge, no thermistor.
* Hold PWR for 10 s to switch off; shorter presses are ignored on purpose.
  `poweroff` and `nvs erase` are available on the console for maintenance.
* Diagnostic console over USB Serial/JTAG with `status`, `version`, `pmu`,
  `test screen`, `test audio`, `screenshot`, `sd dump` and more.

## Windows manager (MosuTimeManager.exe)

Self-contained; no Python needed.

* **Dashboard** - firmware version and build time, MAC, chip, clock, timezone,
  Wi-Fi state, battery, SD and audio.
* **Console** - the same commands as a terminal, with quick buttons and a
  transcript that can be saved.
* **Settings** - every key in `mosu.cfg`, validated against the firmware's
  ranges, with colour pickers; saves the file, pushes it to the card and can
  reboot the watch.
* **Firmware** - build and flash through `tools/build.ps1` when a checkout is
  present, or flash a merged release image directly with the bundled esptool.
* **SD card** - format the card on the watch, push the config and the 79 voice
  clips, run the voice checker.
* **Power** - battery voltage/percentage, charge state, graceful shutdown.

## Verified on the watch

I2C inventory, RTC, PMU, IMU (0x6B), panel and corners, touch and both wake
sources, audio, Wi-Fi join/DHCP/geolocation/SNTP, SD format and 79-file push,
config reload with 0 errors, charge termination at 4.2 V, the 3 s/10 s PWR
test, and the manager's full autotest (connect, version, status, PMU, settings
round-trip, config push, card listing, flash and reboot).

**Not run yet:** PPK2 current measurement, the 24 h soak, and the mechanical
fit. Treat battery life as the model estimate until then.

## Install

1. Copy `mosu.cfg` and `voice/` from the SD zip to a FAT32 card (or let the
   manager format and push them).
2. Put the card in the watch.
3. Flash the firmware:
   * Manager: **Firmware → Flash image**, choose
     `MosuTime-v1.0.0-merged.bin`;
   * or `esptool --chip esp32s3 -p COM11 -b 460800 write_flash 0x0
     MosuTime-v1.0.0-merged.bin`.
4. Enter the real Wi-Fi network in **Settings → Wi-Fi**, push, reboot.

The exe is unsigned: use **More info → Run anyway**, or
`Unblock-File .\MosuTimeManager.exe`.

Checksums are in `SHA256SUMS.txt`.
