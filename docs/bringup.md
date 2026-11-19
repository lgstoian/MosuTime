# Bring-up

The first-hardware checklist. Work through it in order; each step has a
pass condition and a fallback. Do not skip the soak at the end: a watch that
works for ten minutes and resets after six hours is not ready for the wearer.

The commands in this page belong to the maintenance console. Some are old,
some come with the bring-up console extension now in the tree; `help` lists
exactly what the firmware in front of you has. If a command is missing, use the
boot log and the long-standing commands instead, and note it.

| Command | Availability | Use |
| --- | --- | --- |
| `help`, `status`, `time`, `sync`, `say`, `settime`, `bright`, `vol`, `tap`, `reboot` | long-standing | day-to-day maintenance |
| `i2c`, `pmu`, `rtc`, `imu`, `pm`, `heap`, `tasks`, `display`, `test screen`, `test touch`, `test audio`, `screenshot`, `sd remount`, `config`, `power` | bring-up extension | this checklist |
| `wifi <ssid> <pass>`, `wifi scan`, `wifi presence`, `wifi clear`, `wifi status` | bring-up / card-less provisioning | network tests |

**Session 1 (2026-11-19, COM11, no battery, no SD card).** The I2C inventory,
RTC, PMU (VBUS only), IMU, panel, touch, audio beeps, both wake sources,
native-USB console, Wi-Fi join, DHCP, Europe/Bucharest geolocation and SNTP
were checked on the real watch. The SD card, voice clips and battery/PPK2
steps are still **not run**; the checkboxes at the end reflect that.

## 0. Before applying power

* Inspect the board for solder bridges, bent pins and a correctly seated
  display FPC. Do not power a board that shows damage.
* Check the battery connector polarity twice. The AXP2101 expects a single
  LiPo cell. The firmware sets 4.2 V, 200 mA constant current, 25 mA
  termination and 50 mA precharge as a conservative default; adjust the
  current for the actual cell before the first full charge. Do not connect a
  small cell to an unconfigured 400 mA charger.
* Prepare a FAT32 microSD card (16 KB or 32 KB clusters) with `mosu.cfg` in
  the root and `voice/` beside it. The card is optional for boot, but the
  voice test needs it.
* Use a USB **data** cable for the console. A charge-only cable gives you a
  port and no output. The board's only reachable USB port is the ESP32-S3
  native USB (GPIO19/20), so the firmware uses USB Serial/JTAG as the primary
  console; UART0 is only on the reserved GPIO43/44 pads. This machine
  enumerates the watch as **COM11**.
* ESP-IDF 5.5.x must be the one in `PATH`. On Windows run `export.bat` first,
  or use `tools/build.ps1`, which sources the IDF at `C:\esp\v5.5.5\esp-idf`
  by default, puts the IDF Xtensa toolchain ahead of an espup/rustup one, and
  handles the normal stderr from the IDF environment.

## 1. Build, flash, monitor

```sh
idf.py set-target esp32s3     # only on a fresh checkout/build directory
idf.py build
idf.py -p COM11 flash monitor
```

On Windows the wrapper is equivalent:

```powershell
.\tools\build.ps1 -Port COM11 -Flash -Monitor
```

Pass: the build ends with `Project build complete`, the flash reports
`Hash of data verified`, and the monitor shows a boot log without a panic or a
brownout reset. Exit the monitor with `Ctrl+]`.

If the flash tool cannot open the port, close any other terminal using it.
The normal `default_reset` path enters download mode automatically. If the
board still reports `boot:0x23 (DOWNLOAD)` after a reset, release the BOOT
button and power-cycle the watch (long-press PWR ~6 s, then click PWR), or
unplug/replug USB without touching BOOT.

## 2. Expected boot log

The exact interleaving varies because the SD mount, the network task and the
console run in their own tasks, but every line below must appear within about
ten seconds. A missing line is a finding even if the watch appears to work.

```
I (xxx) mosu_board: AXP2101 ready (status 0x..
I (xxx) mosu_board: QMI8658 ready
I (xxx) mosu_sd: SD card mounted at /sdcard
I (xxx) mosu: config: <N> lines, <N> applied, 0 warnings, 0 errors
I (xxx) mosu: starting display
I (xxx) mosu_disp: panel object ready
I (xxx) mosu_disp: panel reset done
I (xxx) mosu_disp: panel init done
I (xxx) mosu_disp: panel on
I (xxx) mosu_disp: display ready (410x502)
I (xxx) mosu_ui: watch face ready
I (xxx) mosu_time: timezone: Europe/Bucharest (EET-2EEST,...
I (xxx) mosu_time: clock restored from RTC          (or "clock not set yet; waiting for network")
I (xxx) mosu_audio: audio ready
I (xxx) mosu_audio: voice clips in /sdcard/voice
I (xxx) mosu_net: ...
I (xxx) mosu_diag: console started
MosuTime console ready, type 'help'
```

With no card, the SD block instead reports a warning, not the old driver
error lines:

```
W (xxx) mosu_sd: SD card not mounted: ESP_ERR_TIMEOUT
W (xxx) mosu: using built-in defaults
```

Notes:

* `panel reset done` appears twice: the reset helper logs it, and
  `panel_create` logs it again after the helper returns. That is expected.
* `sdmmc_common`/`vfs_fat_sdmmc` errors are suppressed around the mount; the
  result is reported by `mosu_sd` as a warning. The same suppression is safe
  for a real card failure: the error code is still printed.
* The QMI8658 is at **0x6B** on this board, not the wiki's 0x6A. `QMI8658
  ready` is the pass line.
* The face is shown for one `screen_on_ms` window at boot, then the screen
  goes off. This is normal, not a crash.
* After the first successful sync, the log gains `network timezone: ...` and
  `clock synced`. A first boot with no network legitimately shows
  `clock not set yet; waiting for network`.

Failure lines and what they mean:

| Line | Meaning |
| --- | --- |
| `AXP2101 not responding` | PMU absent or I2C problem. Battery icon will be grey. |
| `PCF85063 not responding: ...` | RTC absent; time depends on SNTP or `manual_time`. |
| `QMI8658 not found (who=0x..)` | IMU absent at 0x6B; wrist raise unavailable, touch wake still works. |
| `cannot join '...'` / `all configured networks failed` | Wrong SSID/password, 5 GHz-only AP or out of range. Warning only; the clock keeps running and the net task retries. |
| `display init failed` | Panel/QSPI problem; console stays available. |
| `audio unavailable, announcements will be silent` | Codec or I2S problem. |
| `SD card not mounted: ...` | Card missing/unreadable; cached config and no voice. |
| `using the cached config, SD card unavailable` | Expected when booting without a card. |
| `brownout` / `rst:0x...` repeating | Power supply or battery problem; stop and investigate. |

## 3. I2C inventory

The bus is I2C0, SDA GPIO 15, SCL GPIO 14, 400 kHz. Run `i2c` and
compare with this table. `status` already prints the PMU values, and `time`
already proves the RTC, so a missing scanner is not a blocker.

| 7-bit address | Part | Evidence without the scanner |
| --- | --- | --- |
| `0x18` | ES8311 codec | `audio ready`; `test audio` beeps |
| `0x34` | AXP2101 PMU | `status` prints mV, %, charging, vbus |
| `0x38` | FT3168 touch | touch coordinates respond; touch wakes the CPU |
| `0x40` | ES7210 microphone ADC | scan only; the watch does not use the microphones |
| `0x51` | PCF85063A RTC | `time` survives a power cycle |
| `0x6B` | QMI8658 IMU | `QMI8658 ready`; `imu` returns a sample |

The ES8311 appears as `0x30` in some vendor headers; `esp_codec_dev` shifts
that 8-bit write address right by one, so the bus scan shows `0x18`. The
FT3168 is driven by the FT5x06 driver; `0x38` is the correct 7-bit address.
The QMI8658C has SA0 high on this board: it answers at **0x6B**, not the
0x6A printed in the wiki. The scanner names all six devices on the current
build.

Pass: all six addresses answer, and `who=0x05` for the QMI8658. A missing
codec or IMU is not fatal for the clock, but it is a no-go for the wearer
until the reason is known.

## 4. Screen

1. `power screen on` if the face is off, then `test screen colors`: five
   full-width horizontal bands — red, green, blue, white, grey. Check for dead
   pixels, banding and a band that does not reach the edge.
2. `test screen grid`: a white 50 px grid with a border on the last row and
   column. Every line must be continuous and the same width.
3. `test screen corners`: white 20 px squares inset 48 px from each edge and
   a centre cross. The AMOLED has rounded corners; 48 px keeps the squares
   in the visible area. The cross must sit in the middle of the visible area;
   the driver compensates for the panel's 22 px X gap. The battery and Wi-Fi
   icons use the same 48 px side inset and a 30 px top inset, so they clear
   the rounded glass.
4. `test screen off` returns to the watch face. The pattern replaces the face
   while it is active and is drawn without the burn-in offset, so it can be
   used as a ruler against the glass.
5. `bright 10`, `bright 55`, `bright 100`: the panel should change brightness
   without flicker or a visible flash. Return to the config value with
   `reboot` or `bright 55`.
6. `power screen off` then `power screen on`: the panel must switch cleanly
   with no white flash. `power idle` returns to normal.
7. `tap`: the border flashes for `tap_feedback_ms` (250 ms by default).
8. Leave the face on for five minutes and confirm the pixel shift moves the
   whole scene by 1-2 px without a visible jump.
9. `display` prints the resolution, panel brightness, buffer configuration and
   the panel/touch driver names; confirm 410 x 502, double buffer, internal
   DMA and the configured brightness.
10. If the card is mounted, `screenshot /sdcard/shot.bmp` captures the live
    LVGL scene; open the BMP on a PC and compare it with the glass. The
    snapshot is what LVGL draws, the glass is what the panel shows - a
    difference is a panel/touch/init finding, not a UI finding.

Pass: the full 410 x 502 area is correct, black is black (not dark grey), the
numerals are crisp at brightness 55, and no command leaves the panel stuck on
or blank.

## 5. Touch

1. `test touch` toggles the touch logger and prints `touch test on`. Each edge
   now prints `touch down x=... y=...` / `touch up ...`; touch the glass and
   drag to all four corners: the screen response must track the finger without
   jumps, and the reported range must stay in 0-409 by 0-501. Run `test touch`
   again to turn the logger off.
2. Touch the glass while the screen is off. The watch must wake within one
   poll interval plus the touch ISR latency; there is no panel polling while
   the screen is off.
3. Double-tap with the default `double_tap_ms = 650`: one short border flash,
   then the time is spoken. A single tap only lights the screen.
4. Tap and hold for longer than `max_hold_ms = 400`: it must not count as a
   tap. Move the finger more than `max_move_px = 40`: it must not count
   either.
5. Let the screen time out, then tap again. The activity timer must reset on
   each touch and the screen must not switch off while a finger rests on the
   glass.

Pass: every deliberate tap registers, no ghost touches appear when nobody is
touching the watch, and touch wake works after at least 10 s of light sleep.
On the real glass the coordinates reached 25..409 and 8..500, a double tap
while the face was dark logged `screen on (touch)` and built
`h_10.wav`/`m_9.wav`/`d_3.wav` for 22:09, and a wrist raise logged
`screen on (motion)`. The clips were missing because no card was attached;
that is the expected no-card path. This wake test was from screen-off with
USB attached, so the CPU was not in light sleep; repeat it after unplugging
USB (or with the battery) to prove the GPIO wake path.

## 6. Audio

1. `test audio`: three short beeps, 440/880/1320 Hz at 120 ms each, at a
   fixed volume of 70 regardless of `announce.volume`. They must be audible
   and free of clicks. (The no-time fallback uses a different, single
   660 Hz / 140 ms beep.)
2. `say` at the configured volume (85): a full Romanian phrase, intelligible,
   with no clipping and no long gap between fragments.
3. `vol 50` and `vol 100`: the level changes; 100 must not distort. `vol` is
   runtime-only, so `reboot` restores the config value.
4. Wait 30 s after a phrase. There must be no hum, hiss or pop from the
   speaker: `mosu_board_audio_idle()` closes the stream and disables the I2S
   clock between announcements.
5. Remove the SD card and try `say`: the announcement is silent, the screen
   and clock keep working. That is the designed failure mode.

Pass: speech is clear at 85, the idle speaker is silent, and no combination
of `say`/`vol` produces a reboot.

Session 1 checked the codec/PA path with `test audio`: three clear beeps at
440/880/1320 Hz and silence afterwards. `say` needs the SD card and its voice
clips, so the phrase audio is still pending.

## 7. SD card, configuration, voice

1. Format the card FAT32 with 16 KB or 32 KB clusters. Copy
   `sd_card/mosu.cfg` to the root as `mosu.cfg`.
2. Edit `[wifi]` with a real SSID and password. Save the file as ASCII or
   UTF-8; the parser skips a leading UTF-8 BOM. Do not save it as UTF-16.
3. Generate the voice on the PC:

   ```sh
   pip install onnxruntime
   python tools/gen_voice.py --out sd_card/voice
   python tools/check_voice.py sd_card/voice
   ```

   Expect 78 clips (12 hours, 59 minutes, 7 weekdays), each mono 16-bit WAV.
   Copy the whole `voice/` folder to the card.
4. Insert the card. `sd remount` proves the card mounts without a reboot, but
   it does **not** re-read `mosu.cfg`; run `reboot` to apply the file. Expect
   `SD card mounted at /sdcard`, a config line with `0 errors`, and
   `voice clips in /sdcard/voice`.
5. `config` prints the active values: display, status thresholds, colours,
   clock, announcement, and the SSIDs of the configured networks (never the
   passwords). Check brightness, `screen_on_ms`, `motion_poll_hz`,
   `sync_hours`, the timezone and at least one SSID. It shows what the watch
   is using, not where the value came from.
6. Negative test: put `brightness = abc` in a copy of the file, reboot, and
   confirm the line is reported and the watch still boots with the previous
   value. Restore the good file afterwards.
7. Remove the card and reboot: expect `using the cached config, SD card
   unavailable`, the clock still working, and `say` silent.

Pass: the card mounts every time, the config has zero errors, the clips for the
tested times play in the right order (hour, then minute, then weekday), and a
missing card never prevents boot.

The TF socket is wired for SPI as well as SD: the SPI chip select is GPIO17,
which is DAT3 in SD mode. The firmware drives GPIO17 high before the 1-bit
SDMMC mount. If a known-good card still fails with `ESP_ERR_TIMEOUT`, record
the card model and switch the mount to SDSPI rather than guessing.

## 8. Wi-Fi sync and timezone

0. With a card, edit `[wifi]` in `mosu.cfg` and reboot. Without a card, run
   `wifi <ssid> <password>` on the console; it saves the network to NVS and
   requests a sync immediately. `wifi status` prints the SSID only, never the
   password. `wifi scan` lists the visible APs and `wifi presence` checks
   whether a configured SSID is in range without associating.

1. `sync` and watch the log. A clean run joins within the 15 s connect
   timeout, waits for DHCP (the geolocation request must not run before the
   IP exists), prints `network timezone: Europe/Bucharest` (or the zone for
   the current IP) and `clock synced`. `status` should show a valid clock, an
   IP and an RSSI; the on-screen icon should be green.

   On session 1, `camera` is a 2.4 GHz WPA2-PSK AP at about -80 dBm. It is a
   weak link: the first join can time out, but the retry schedule recovers.
   The test joined, got 192.168.1.252, printed
   `network timezone: Europe/Bucharest (offset UTC-2)` and synced.
2. With `auto_timezone = true`, change the network (for example a phone
   hotspot in another country) and confirm the zone follows the IP. Unknown
   zones are ignored, not guessed.
3. Set `auto_timezone = false` and `timezone = Europe/Bucharest`, reboot, and
   confirm the displayed hour is local Romanian time, not UTC.
4. Reboot and read the log: `clock restored from RTC` must appear before any
   network sync, and the displayed time must already be correct. The RTC is
   the reference between syncs; do not adjust it by hand.
5. Failure test: use a wrong password. The icon turns red with a slash, the
   clock keeps running from the RTC, and the next attempt is scheduled 5
   minutes later (then 15 and 30, before the normal `sync_hours`). The watch
   must not block the UI while trying.
6. `time` after a sync. The seconds must advance and the timezone name must be
   the one you expect.

7. `config` prints `sync_per_day` and `sync_hours`. `sync_per_day = 4` means
   every six hours; `sync_per_day = 5` means 4.8 hours; `sync_per_day = 0`
   falls back to `sync_hours`. Edit the card, reboot, and confirm the printed
   values. The next due time is not printed on the glass; `status` shows the
   last sync age.

8. Failure and return-home. Use a wrong password or move the watch out of
   range: the icon turns red, the clock keeps the RTC time, and the log has
   warnings but no panic. The first three failures retry after 5, 15 and
   30 minutes. If the failure reason is `NO_AP_FOUND`, a 15-minute presence
   scan looks for the configured SSID without associating; when the owner is
   back home it triggers a sync immediately instead of waiting for the normal
   interval. A wrong password keeps the normal backoff rather than scanning
   forever.

Pass: a successful sync sets both the clock and the RTC, a failed sync changes
only the icon, and the correct local hour survives a card-less, network-less
boot. A failed join never panics or resets the watch.

## 9. Power and 24 h soak

1. Run the state measurements in [`docs/power.md`](power.md). At minimum
   record screen-off idle, screen-off + IMU at 8 Hz, screen on with the normal
   face at brightness 55, one announcement, and one successful sync.
2. `pm` must report the expected CPU limits (160/80 MHz) and light
   sleep enabled. `power screen off`, `power wifi` and `power idle` must
   behave as described in `docs/power.md`.

   While the watch is on USB, `pm` shows `usb_serial_jtag NO_LIGHT_SLEEP`
   active because `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` keeps the native-USB
   console alive. Unplug USB (or use the UART0 pads) for a real light-sleep
   or PPK2 measurement.
3. `heap` and `tasks` at the start. Leave the watch running for 24 hours with
   the card inserted and the radio in its normal cycling mode. Keep a serial
   terminal logging to a file (PuTTY can log a session; do not rely on a
   scrollback buffer).
4. During the soak, exercise it like a wearer: wrist raises, single taps,
   double taps, at least one sync, one card removal and reinsertion followed
   by `sd remount` if the hardware allows it without opening the case.
5. After 24 hours: `status`, `time`, `heap`, `tasks`, `pmu`. Compare heap and
   stack high-water marks with the start, and compare the battery drop with
   the model's daily mAh. Check that the screen is off when untouched, that
   no `brownout` or `rst:` line appears, and that the clock is still within a
   few seconds of a reference.

Pass: no reset, no watchdog, no `cpu_start`/`rst:` in the log, heap within a
few kilobytes of the start, no task below its stack margin, the screen is off
at rest, and the measured 24 h average is at or below the acceptance target in
`docs/power.md`.

## 10. Go / no-go

Give the watch to the wearer only when every line is true.

- [x] All six I2C devices answer (0x18, 0x34, 0x38, 0x40, 0x51, **0x6B**).
- [x] Screen: full area, no dead pixels, colour/grid/corners pass, no flash
      on off/on, brightness 10/55/100 smooth. Pixel shift not timed yet.
- [x] Touch: corners track (25..409 x 8..500), double tap builds the right
      clips, no ghost touches seen, touch wake works. Light-sleep wake is
      still pending because USB holds the no-sleep lock.
- [ ] Audio: beeps verified; intelligible speech at 85 and clipping at 100
      are pending the SD card and voice clips.
- [ ] SD: pending a card; see the GPIO17/DAT3 note above.
- [x] Wi-Fi: joins `camera` (2.4 GHz WPA2, ~-80 dBm), waits for DHCP, sets
      Europe/Bucharest, writes the RTC, turns the radio off, and fails
      gracefully with a wrong password. Presence scan was checked both ways;
      the 15-minute automatic trigger has not been timed.
- [ ] Power: no battery, no PPK2; idle/24 h figures are still model estimates.
- [ ] Mechanical: not checked in this session.
- [ ] Recorded: board is ESP32-S3 rev v0.2, 8 MB PSRAM, 32 MB flash, MAC
      `44:bd:8d:77:8a:60`; battery capacity and serial/label are pending.

If any box is unchecked, fix it and repeat the affected step. A conditional
"go" is a no-go for this wearer.
