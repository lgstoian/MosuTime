# Findings

An engineering log of the non-obvious issues found while testing MosuTime on the
desktop and in esp-emu, before the first hardware session. Each entry records
the symptom, the cause, the fix and the file that carries it.

**These are desk findings, not hardware sign-off.** The emulator does not model
light sleep, the SD card, the real panel or audio, so every item below still
needs its matching step in [`bringup.md`](bringup.md) on the real watch. Where a
fix was only exercised by host tests or the emulator, the entry says so.

Environment for the log: ESP-IDF 5.5.5, LVGL 9.6.0, esp-emu 1.0.0-rc2,
Waveshare ESP32-S3-Touch-AMOLED-2.06. The production configuration was not
changed to make the emulator work.

## Summary

| # | Finding | Fixed in tree | Hardware check still needed |
| --- | --- | --- | --- |
| 1 | SD mount blocks boot | yes | yes: no card, dirty card, card removed while asleep |
| 2 | Duplicate touch GPIO ISR | yes | yes: touch and touch wake after sleep |
| 3 | AXP2101 13-bit battery voltage layout | yes | yes: compare `pmu` with a meter |
| 4 | QMI8658 8192 LSB/g truncated to `uint8_t` | yes | yes: wrist-raise angle and false wakes |
| 5 | Touch wake level/edge conflict | yes | yes: touch after light sleep |
| 6 | Brightness flash at boot | yes | yes: power-cycle with the glass visible |
| 7 | Timezone fallback | yes | yes: no-network boot and travel-zone change |
| 8 | Session/tap race across LVGL and app tasks | yes | yes: double tap at the timeout boundary |
| 9 | Emulator rc2 I2C port shadowing | yes (emulator only) | n/a: no production impact |
| 10 | One failed sync left the clock stale for hours | yes | yes: pull the router for two minutes and watch the retries |
| 11 | Every voice clip said "marcă comercială" | yes | yes: listen to `preview.wav` on the watch |
| 12 | QMI8658 answers at 0x6B, not 0x6A | yes | done: `imu` shows WHO_AM_I 0x05 and 1.028 g |
| 13 | QMI8658 big-endian select vs little-endian decode | yes | done: `imu` shows a plausible 1 g |
| 14 | PCF85063A time registers start at 0x04, not 0x02 | yes | done: `rtc` and reboot restore are correct |
| 15 | I2C register reads need a repeated START | yes | done: RTC reads are no longer shifted |
| 16 | Native USB secondary console cannot accept input | yes | done: COM11 console commands answer |
| 17 | AXP2101 TS detection and a 4.1 V CV field | yes | pending: verify with the battery attached |
| 18 | I2S channel disabled twice after a tone | yes | done: `test audio` is clean, no E line |
| 19 | SH8601 logs an error for swap_xy(false) | yes (suppressed) | done: boot log has no false E line |
| 20 | Rounded AMOLED corners clipped the status icons | yes | done: user confirmed both icons visible |
| 21 | Geolocation ran before DHCP and could not resolve DNS | yes | done: `network timezone: Europe/Bucharest` |
| 22 | SD socket is SPI-wired; DAT3/CS is GPIO17 | yes (drive high) | done: card formatted and mounted at /sdcard |
| 23 | `tools/build.ps1` aborted on normal stderr and rustup's gcc | yes | done: clean build on this machine |
| 24 | PC-formatted FAT32 card rejected with `FR_NO_FILESYSTEM` | yes: format on the watch | done: `sd format: ESP_OK, mounted` |
| 25 | Console CR-to-LF input corrupted binary uploads | yes | done: 79 files pushed with a CRC-32 each |
| 26 | Binary upload capped at 6 KiB/s and lost the odd byte | yes | done: `rx bench` 455 KiB/s, 79 files in ~20 s |
| 27 | Display-off left the AMOLED driver biased all night | yes | pending: 6 h+ drain comparison |
| 28 | A 35 deg / 150 ms wrist raise woke the face far too often | yes | done: 50 deg / 300 ms / 5 s and learned steps |
| 29 | USB console open reset the CPU and wiped the counters | yes: RTC no-init | done: `stats` survives reconnects |
| 30 | Half a white screen and a freeze when the face woke | yes | done: 100 forced off/on cycles |
| 31 | Lift-to-wake only responded to a hard shake | yes | logic + host simulation; wrist test pending |
| 32 | AXP2101 left eight unused rails enabled | yes: rail dump | done: rails off, I2C/SD/audio still pass |
| 33 | Background logs and polling kept a task awake | yes | done: quiet profile, 100 panel cycles |
| 34 | Panel sleep-in loses the GRAM and can flash white | yes: 100 cycles | done: black fill before display-on |
| 35 | Watch ran ~110 mA with the face off: LVGL task kept core 1 awake | yes: SD log + sleep counter | done: suspend LVGL task while off |
| 36 | `sleep_entries = 0` in the log could mean "console opened", not "not sleeping" | yes: RTC no-init + log gate | done: counter survives reconnect; checker rejects a stuck counter |
| 37 | `luni` sounded foreign with the dictionary single palatal | yes: espeak-trained token | done: wearer confirmed the doubled-palatal clip |

## 1. SD mount blocking boot

**Symptom.** With no card, a badly seated card or a card that answers slowly,
the watch could sit in the boot path for a long time before the face appeared.
The panel was already initialised, so the wearer saw either a dark screen or a
frame that never updated, depending on how far the mount had got.

**Cause.** `esp_vfs_fat_sdmmc_mount()` is blocking and was called directly from
`load_config()` in the boot sequence, before the display and watch face were
brought up. A marginal card can hold the FAT mount for seconds; a completely
absent card can also make the SDMMC host wait for its timeout.

**Fix.** The mount now runs in a dedicated `mosu_sd` task. The boot waits for a
binary semaphore for `MOSU_SD_WAIT_MS` (3000 ms), then continues without the
card. On timeout or failure `load_config()` falls back to the last good config
in NVS, then to the built-in defaults. `mosu_board_sdcard_remount()` drains any
stale semaphore token so a mount that finished after its caller gave up cannot
be mistaken for the next result.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_storage.c`](../components/mosu_board/src/mosu_board_storage.c),
with the fallback order in [`main/app_main.c`](../main/app_main.c)
(`load_config`). The emulator build skips the mount entirely through
`CONFIG_MOSU_SKIP_SD`.

**Verification.** The emulator boots with the mount compiled out. The host tests
cover the parser and the shipped config, not the NVS fallback; the no-card,
dirty-card and card-removed paths still need the SD step in the bring-up
checklist.

## 2. Duplicate touch GPIO ISR

**Symptom.** Touch input was unreliable after the display started. Some edges
never reached the application, and the custom wake path and the LVGL input path
did not agree on who had seen the finger. It looked like a dead touch panel
rather than a software conflict.

**Cause.** `app_main` installed its own GPIO ISR on the FT3168 interrupt pin
(GPIO 38) and also configured the pin. `esp_lvgl_port_add_touch()` installs its
own handler for the same pin and puts the touch device into event mode. Two
owners for one interrupt pin means one handler replaces or competes with the
other; the GPIO ISR service is global, and the last registration wins.

**Fix.** The application no longer installs a touch ISR. `esp_lvgl_port` owns
the pin and the LVGL callback is the single source of touch edges. That callback
still runs while the face is off, because stopping the LVGL timers does not stop
the LVGL task, so a touch can still request a wake. The callback feeds
`mosu_session` and the session records the pending wake.

**Fix lives in.** [`main/app_main.c`](../main/app_main.c) (`on_touch`), with the
touch device registration in
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c)
(`touch_create`, `mosu_board_display_start`). There must be exactly one owner of
`MOSU_PIN_TOUCH_INT`.

**Verification.** `tools/emu/smoke.sh` drives a tap and a double tap through the
virtual FT3168 and the LVGL input path. The wake after real light sleep still
needs the touch step in the bring-up checklist.

## 3. AXP2101 13-bit battery voltage layout

**Symptom.** The battery voltage and percentage did not match a meter and could
jump by a volt or more between samples. The battery icon could show a healthy
pack when the cell was low, or the reverse, which is exactly the kind of
surprise the watch must avoid.

**Cause.** The AXP2101 VBAT ADC result at registers `0x34`/`0x35` is a 13-bit
value, not a plain 16-bit big-endian word: five bits live in `0x34` and eight in
`0x35`, at 1 mV/LSB. Reading `(raw[0] << 8) | raw[1]` includes the upper status
bits of `0x34` and inflates the result by up to roughly 1.8 V.

**Fix.** Mask the high byte:
`((raw[0] & 0x1F) << 8) | raw[1]`. The percentage then comes from the normal
battery curve, so the icon and the number agree.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c)
(`mosu_board_pmu_read`). `mosu_board_pmu_dump()` prints the raw status and the
decoded value side by side, so the next person can compare it with the
datasheet and a meter.

**Verification.** The low-battery emulator board exercises the percentage and
red bucket, not the ADC layout. Compare `pmu` against a meter during the
bring-up power step.

## 4. QMI8658 8192 LSB/g truncated to `uint8_t`

**Symptom.** The wrist raise did not fire, or the `imu` console dump showed a
magnitude that was not about 1 g at rest. A scale of zero turns the acceleration
into infinity/NaN, and no angle test can pass on that.

**Cause.** The QMI8658 is configured for ±4 g, which is 8192 LSB/g. The divisor
was stored in an 8-bit type. 8192 modulo 256 is 0, so the division did not
produce a small wrong number; it produced a non-finite one. Even a non-zero
8-bit divisor would have made every sample far too large.

**Fix.** Keep the divisor in a 16-bit type (`static uint16_t s_imu_lsb_div =
8192;`). The raw axes are still read as little-endian `int16_t` and divided by
that single constant, so the driver and the emulator part cannot drift.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c)
(`s_imu_lsb_div`, `mosu_board_imu_read`). The emulator's virtual QMI8658 uses
the same 8192 LSB/g in
[`tools/emu/imu/src/lib.rs`](../tools/emu/imu/src/lib.rs).

**Verification.** The `imu` console dump should show `|a|` near 1 g at rest.
`smoke.sh` proves the raise reaches the panel; the real angle and false-wake
rate still need the wrist test in the bring-up checklist.

## 5. Touch wake level/edge conflict

**Symptom.** Touch worked while the face was on, then became unreliable after
the first screen-off window: a touch did not wake the watch, or the next touch
was missed after the wake.

**Cause.** On the ESP32-S3, GPIO light-sleep wake only accepts a level trigger.
`gpio_wakeup_enable()` reprograms the pin to level-low. The FT3168/LVGL input
path expects the normal falling edge. Leaving the pin in level-low while the
face was on could hold the interrupt asserted; restoring the edge only later
left a window where the two configurations disagreed.

**Fix.** Arm the level wake source only while the panel is off. On wake,
disable the source and explicitly restore `GPIO_INTR_NEGEDGE` for the LVGL
touch input. The board logs a warning if the edge cannot be restored instead of
pretending the touch is healthy.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c)
(`mosu_board_enable_touch_wake`), called from `screen_on()` / `screen_off()` in
[`main/app_main.c`](../main/app_main.c). The emulator notes the same behaviour
in [`tools/emu/README.md`](../tools/emu/README.md); it does not model light
sleep, so only the real watch can prove the wake.

**Verification.** Not exercised by the emulator. The bring-up touch step checks
a touch after at least 10 s of light sleep.

## 6. Brightness flash at boot

**Symptom.** A visible white or very bright frame appeared at power-on before
the configured brightness took effect. On an AMOLED in a dark room this is a
flash in the wearer's eye, not a cosmetic detail.

**Cause.** The vendor panel initialisation sequence ended by writing `0xFF` to
the brightness command (`0x51`), i.e. full brightness. The panel was therefore
already lit at maximum when `app_main` got control and applied
`[display] brightness`.

**Fix.** The init sequence now ends with `0x51 0x00`, so the panel starts dark.
`app_main` writes the configured brightness immediately after
`mosu_board_display_start()` succeeds and before the watch face is created or
`screen_on()` runs. Every later wake restores the same value, because switching
the panel off zeroes it.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c)
(`s_init_cmds`) and [`main/app_main.c`](../main/app_main.c) (right after
`mosu_board_display_start`). The bring-up screen step checks power transitions
for the same flash.

**Verification.** The emulator captures the face after init but cannot judge a
flash in a dark room. Watch the glass through a real power cycle during
bring-up.

## 7. Timezone fallback

**Symptom.** After a boot without a usable network zone, the clock could show
UTC as if it were local time. The hour was wrong by the DST offset, while the
RTC itself was correct. An unknown geolocation answer could also be passed to
newlib as if it were a POSIX TZ string.

**Cause.** newlib on ESP-IDF understands POSIX TZ strings, not IANA names. The
geolocation service returns names such as `Europe/Bucharest`, and some services
return a numeric UTC offset instead of a name. The code had to map or spell
both, but the fallback path only set the configured zone on some branches. If
neither the stored zone nor the configured value produced a valid `setenv`,
newlib kept its default, which is effectively UTC. An unmapped IANA name with a
slash is not a valid POSIX string and must not be applied blindly.

**Fix.** `mosu_time_init()` now uses an explicit chain: stored auto-timezone,
then the configured timezone, then `UTC0`. It never leaves newlib without a
zone. `mosu_time_apply_timezone()` accepts an IANA name, a POSIX string or a
numeric UTC offset; offsets are spelled as POSIX by `mosu_policy` and stored
that way. `mosu_policy_parse_geo()` reads the plain-name, name-plus-offset and
JSON shapes. `mosu_net` prefers a name the table knows, then falls back to the
numeric offset, and never applies an unvalidated body. An unmapped IANA-looking
name containing `/` keeps the current zone. `mosu_config_sanitize()` replaces an
invalid configured zone with the known-good Romanian default, and the timezone
string is double-buffered so the network task can update it while the UI reads
it.

**Fix lives in.**
[`components/mosu_time/src/mosu_time.c`](../components/mosu_time/src/mosu_time.c)
(`set_zone`, `mosu_time_init`, `mosu_time_apply_timezone`), the offset and geo
rules in
[`components/mosu_policy/src/mosu_policy.c`](../components/mosu_policy/src/mosu_policy.c),
with supporting validation in
[`components/mosu_config/src/mosu_config.c`](../components/mosu_config/src/mosu_config.c)
and [`components/mosu_net/src/mosu_net.c`](../components/mosu_net/src/mosu_net.c).

**Verification.** `test_policy.c` covers the numeric offsets, POSIX spelling and
geolocation shapes; `test_config.c` covers the timezone syntax and sanitizer.
The runtime fallback chain is not host-testable (it needs newlib and NVS), so
the no-network boot and the travel-zone change still need the Wi-Fi step in the
bring-up checklist.

## 8. Session/tap race across LVGL and app tasks

**Symptom.** A rare lost double tap, or a tap that was counted twice, around
the moment the face timed out. It was timing-dependent and did not reproduce on
every run, which is typical of a shared-state race rather than a logic bug.

**Cause.** Touch edges arrive in the LVGL task. `screen_on()`/`screen_off()` and
the inactivity check run in the app task. The tap detector and the screen state
were shared between the two without a lock, and `screen_off()` reset the
detector while the LVGL callback could still be updating it. Stopping the LVGL
timers does not stop the task or its input callback.

**Fix.** The shared rules now live in the pure `mosu_session` state machine and
every access from `app_main` is wrapped in a short spinlock (`s_session_mux`):
touch feed, take-say, take-wake, activity, sleep decision, screen on and screen
off. The detector deliberately keeps its cooldown and any first tap across
`screen_off()`, so a gesture that began on a dark screen can finish after the
panel lights. The rules are covered by host tests, including a double tap from
the dark and a touch during speech.

**Fix lives in.** [`main/app_main.c`](../main/app_main.c) (`on_touch`,
`s_session_mux`, the main loop), the state itself in
[`components/mosu_session/src/mosu_session.c`](../components/mosu_session/src/mosu_session.c),
and the regression tests in
[`test/host/test_session.c`](../test/host/test_session.c). The real gesture at
the timeout boundary is part of the touch bring-up.

**Verification.** Host tests cover the transition rules; the emulator smoke test
covers a normal double tap. A deliberate tap at the screen-off boundary still
needs a person and the real watch.

## 9. Emulator rc2 I2C port shadowing

**Symptom.** With the interactive emulator board, touch and the IMU worked but
the PMU, RTC and codec were silent: no battery, no restored clock and no audio.
The same firmware on `board.json` (without virtual parts) configured those
devices normally.

**Cause.** esp-emu 1.0.0-rc2 answers an I2C port only through the parts wired to
it. If a port has a part, its `regfile` devices are not queried ("Parts win").
The FT3168 and QMI8658 need port 0, so adding those parts hid the AXP2101,
PCF85063 and ES8311 register files that `board.json` preloads on the same port.

**Fix.** `board-interactive.json` adds a small `chips` part that serves exactly
the registers the devices preload: AXP2101 at `0x34`, PCF85063 at `0x51` and
ES8311 at `0x18`. The `devices` entries remain as the description of the real
board. When a future esp-emu answers devices per address, remove the `chips`
part and let the device models take over.

**Fix lives in.** [`tools/emu/chips/src/lib.rs`](../tools/emu/chips/src/lib.rs),
[`tools/emu/board-interactive.json`](../tools/emu/board-interactive.json) and
the explanation in [`tools/emu/README.md`](../tools/emu/README.md). This is
emulator-only; the production build and `sdkconfig.defaults` are unaffected.

**Verification.** `tools/emu/smoke.sh` and `tools/emu/capture.sh` run against
`board-interactive.json`; both depend on the chip registers being present.

## 10. One failed sync left the clock stale for hours

**Symptom.** A router reboot, a DHCP hiccup or an association failure at the
wrong moment could leave the displayed time uncorrected until the next
`sync_hours` window - up to twelve hours by default.

**Cause.** The failure path scheduled the next attempt with the same interval
as a success. A transient failure and a permanently wrong password were treated
identically, so the watch paid the full interval for a problem that would have
cleared in a minute.

**Fix.** `mosu_policy_next_sync_s()` now returns a bounded backoff for the first
three consecutive failures (5, 15 and 30 minutes), then the configured
`sync_hours`. A success resets the counter. The timezone request also gets one
retry for a timeout, a connection error or an HTTP 5xx; a 4xx is never retried
because the URL is wrong. The radio is still switched off between attempts.

**Fix lives in.** [`components/mosu_policy/src/mosu_policy.c`](../components/mosu_policy/src/mosu_policy.c)
(`mosu_policy_next_sync_s`), the task loop and the HTTP retry in
[`components/mosu_net/src/mosu_net.c`](../components/mosu_net/src/mosu_net.c).

**Verification.** `test/host/test_policy.c` covers the schedule and the reset.
The emulator has no configured AP, so the real association/backoff path is part
of the Wi-Fi step in the bring-up checklist.

## 11. Every voice clip said "marcă comercială"

**Symptom.** Every announcement carried a spoken phrase that listeners heard as
"commercial mark". The words of the announcement were still there, buried after
it, so the clock *worked* and only sounded wrong. The hour and weekday clips
were also much shorter than the script in the tree produces.

**Cause.** The WAV files on the SD card were not produced by
`tools/gen_voice.py`. They were watermarked audio from a third-party service:
word timestamps show `m_7.wav` speaking *"e grav marca comerciale, e grav marca
comercială"* for the first 3.20 s and only then *"…șapte minute"*, and
`m_30.wav` carrying 1.8 s of the same notice. The durations gave it away — the
file was 3.98 s where the phrase it claims to say renders in 1.46 s — but
nothing in the repository checked that, so the folder was copied to the card
anyway.

The render pipeline itself was fine. Re-running it on the same voice produced
intelligible Romanian, which is how the folder and the tool were told apart.

**Fix.** The folder is regenerated from `tools/gen_voice.py`, and
`tools/check_voice.py` now compares each clip's duration with the phoneme count
of the phrase it is supposed to say (`0.289 s + 0.043 s` per phoneme). A clip
more than 2.5x longer than its own text fails with *"extra words?"*; the
watermarked set fails 43 clips, the current one passes with every clip between
0.71x and 1.21x.

The same pass removed the GPL render dependency: `piper-tts` is
`piper1-gpl`, GPL-3.0-or-later, so the pronunciation now comes from the MIT
lexicon in `tools/mosu_g2p.py` and the voice runs under onnxruntime (MIT).

**Fix lives in.** [`tools/check_voice.py`](../tools/check_voice.py)
(`expected_durations`, `check`), [`tools/gen_voice.py`](../tools/gen_voice.py),
[`tools/mosu_g2p.py`](../tools/mosu_g2p.py).

**Verification.** `check_voice.py` fails the old folder and passes the new one;
the ASR of the regenerated `preview.wav` returns *"Este ora două și treizeci de
minute. Marți…"*. A native listening check on the watch is still required — an
ASR is not a Romanian ear, and the emulator cannot hear audio at all.

## 12. QMI8658 answers at 0x6B, not 0x6A

**Symptom.** The first hardware boot logged `QMI8658 not found (who=0x00)`;
`imu` returned `WHO_AM_I read failed`. Wrist raise was unavailable.

**Cause.** The Waveshare wiki lists 0x6A, but the populated QMI8658C has SA0
high. The `i2c` scan on the real board answered at **0x6B**:

```
0x18 ES8311  0x34 AXP2101  0x38 FT3168  0x40 ES7210  0x51 PCF85063  0x6B QMI8658
```

**Fix.** `QMI8658_ADDR` is 0x6B. The emulator part and all four emulator
board files were moved to 0x6B so the desk tests match the watch.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c),
[`tools/emu/imu/src/lib.rs`](../tools/emu/imu/src/lib.rs),
[`tools/emu/board.json`](../tools/emu/board.json).

**Verification.** `imu` prints `WHO_AM_I 0x05 (QMI8658)` and
`accel 0.052 0.252 -0.995 g, |a| 1.028 g` at rest.

## 13. QMI8658 big-endian select vs little-endian decode

**Symptom.** With the address fixed, `imu` would have reported an almost
zero magnitude at rest, and the wrist detector would never fire.

**Cause.** CTRL1 was written as `0x60`: bit 6 is `ADDR_AI`, bit 5 is `BE`.
`mosu_board_imu_read()` decodes the six acceleration bytes as little-endian
`int16` pairs. Big-endian output byte-swaps every axis: 1 g becomes 0.0039 g.
The emulator model accepts and ignores CTRL writes, so the desk tests could
not catch it.

**Fix.** CTRL1 is `0x40` (address auto-increment, little-endian).

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c)
(`mosu_board_imu_init`).

**Verification.** `imu` reports `|a| 1.028 g`; a wrist raise while the face
is off logs `screen on (motion)`.

## 14. PCF85063A time registers start at 0x04, not 0x02

**Symptom.** `rtc` decoded nonsense (for example `2006-01-11 07:00:73`) and
the "hour" register counted like a seconds register. The year never changed
from `06`.

**Cause.** The code used the PCF8563 layout (seconds at 0x02). This board has
a **PCF85063A**: 0x02 is the offset register and 0x03 is RAM; seconds start
at 0x04. Every read and write was shifted by two registers.

**Fix.** Use the PCF85063A constants: seconds 0x04, minutes 0x05, hours 0x06,
day 0x07, weekday 0x08, month 0x09, year 0x0A. The oscillator-stop flag in
the seconds register is now also checked, so a stale RTC is not trusted.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c)
(`PCF85063_*`, `mosu_board_rtc_read`, `mosu_board_rtc_write`,
`mosu_board_rtc_valid`).

**Verification.** `settime 2026-11-19 22:15` then `rtc` shows
`UTC 2026-11-19 20:15:03, valid 1`; a reboot logs
`clock restored from RTC` and the same local time.

## 15. I2C register reads need a repeated START

**Symptom.** Even after the address fix, RTC reads still looked shifted: the
first byte behaved like the offset register and the count wrapped at the
wrong places.

**Cause.** `reg_read()` used `i2c_master_transmit()` followed by
`i2c_master_receive()`, two transactions with a STOP between them. The
PCF85063 resets its register pointer on STOP, so the read started at 0x00.
The AXP2101 and QMI8658 tolerated the split; the RTC did not.

**Fix.** `i2c_master_transmit_receive()` sends the register address and reads
the data in one transaction with a repeated START.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c)
(`reg_read`).

**Verification.** The RTC registers now read back exactly what was written,
and `rtc` advances one second per second.

## 16. Native USB secondary console cannot accept input

**Symptom.** `idf.py monitor` showed the boot log over COM11, but typing
`help`, `i2c` or `test screen` produced no answer.

**Cause.** The board's USB-C is the ESP32-S3 native USB (GPIO19/20), not a
UART bridge. `sdkconfig` had UART0 as the primary console and USB
Serial/JTAG as the secondary. ESP-IDF's secondary USB console is
output-only: it supports no REPL and no input. UART0 is only on reserved
pads.

**Fix.** Make `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` the primary console,
disable the secondary console, and enable
`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` so the USB link survives automatic
light sleep during bench work. The emulator overlay keeps UART0 because
esp-emu does not model USB Serial/JTAG.

**Fix lives in.** [`sdkconfig.defaults`](../sdkconfig.defaults),
[`tools/sdkconfig.emulator`](../tools/sdkconfig.emulator).

**Verification.** Every console command in this session answered over COM11;
the boot log and `help` output are in `hw-console1.log` (not committed).

## 17. AXP2101 TS detection breaks charging without an NTC

**Symptom.** Not observed yet because no battery is attached. The board has
no battery thermistor, and the AXP2101 charger looks at the TS pin unless it
is put in fixed-input mode.

**Cause.** Clearing ADC channel bit 1 is not enough; the TS pin control
register (0x50) also has to select the external fixed input. Waveshare's
example explicitly calls this out for boards without an NTC.

**Fix.** Write `0x50 = 0x10` during PMU init, before charging is enabled.
While auditing the charge registers, the CV field was found to be `0x02`,
which is 4.1 V, not 4.2 V (`0x03`). The first profile is now 4.2 V, 200 mA
constant current, 25 mA termination and 50 mA precharge: safe for a small
single-cell pack until the real capacity is known.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c)
(`mosu_board_pmu_init`).

**Verification.** Pending: attach the battery, run `pmu`, and confirm the
charge current and termination behave. Adjust 200 mA for the actual cell
before the first full charge.

## 18. I2S channel disabled twice after a tone

**Symptom.** `test audio` played correctly but logged
`E i2s_common: i2s_channel_disable(1262): the channel has not been enabled yet`
at the end of the beeps.

**Cause.** `mosu_board_audio` enabled the I2S channel before opening the
codec, then disabled it again after `esp_codec_dev_close()`. `esp_codec_dev`
already disables the channel in `close()`, so the second disable was an
error. Removing the manual enable entirely made the open path log the same
error because the IDF reconfig functions expect a running channel.

**Fix.** Keep a tracked enable before `esp_codec_dev_open()`, but never call
`i2s_channel_disable()` after `esp_codec_dev_close()`; clear the flag
instead. The codec owns the disable.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_audio.c`](../components/mosu_board/src/mosu_board_audio.c)
(`ensure_channel`, `open_stream`, `mosu_board_audio_idle`,
`mosu_board_audio_play_wav`).

**Verification.** `test audio` returns `ESP_OK` with no E line; three clear
beeps were heard, and the speaker was silent afterwards.

## 19. SH8601 logs an error for swap_xy(false)

**Symptom.** Every boot logged
`E sh8601: swap_xy is not supported by this panel` even though the display
was configured with `swap_xy = false`.

**Cause.** `esp_lvgl_port_add_disp()` calls `esp_lcd_panel_swap_xy()`
unconditionally, and the Waveshare SH8601 driver logs E and returns
`ESP_ERR_NOT_SUPPORTED` even for `false`.

**Fix.** Silence the `sh8601` tag only around `lvgl_port_add_disp()`, then
restore its log level. The return value of `lvgl_port_add_disp()` remains
the real verdict.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c)
(`mosu_board_display_start`).

**Verification.** The boot log now has no `E (` line; the face is drawn and
the colour/grid/corner tests pass.

## 20. Rounded AMOLED corners clipped the status icons

**Symptom.** The battery outline and Wi-Fi icon were impossible to see on
the real glass even though they were correct in the simulator.

**Cause.** The panel has rounded corners; the status row was placed 20 px
from the sides and 16 px from the top, inside the cut-out.

**Fix.** `MOSU_SCREEN_MARGIN` is 48 px and `MOSU_STATUS_Y` is 30 px. The
corner test pattern now draws its 20 px squares at a 48 px inset so the test
measures the visible area, not the cut-out.

**Fix lives in.**
[`components/mosu_core/include/mosu_core/mosu_layout.h`](../components/mosu_core/include/mosu_core/mosu_layout.h),
[`components/mosu_ui/src/mosu_ui.c`](../components/mosu_ui/src/mosu_ui.c)
(`draw_test_corners`).

**Verification.** The wearer confirmed both icons are fully visible and the
four corner squares are clear of the glass edge.

## 21. Geolocation ran before DHCP and could not resolve DNS

**Symptom.** After joining `camera`, the log showed
`esp-tls: couldn't get hostname for :ip-api.com: getaddrinfo() returns 202`
and `timezone lookup failed`. The clock later synced, but the zone stayed at
the configured fallback.

**Cause.** `connect_to()` waited for `WIFI_EVENT_STA_CONNECTED`, which is
association, not an IP address. The HTTP geolocation request ran before DHCP
finished, so DNS failed.

**Fix.** Wait for `IP_EVENT_STA_GOT_IP` before returning from
`connect_to()`. The geolocation request and SNTP now run with an address.

**Fix lives in.**
[`components/mosu_net/src/mosu_net.c`](../components/mosu_net/src/mosu_net.c)
(`BIT_GOT_IP`, `connect_to`).

**Verification.** `sync` logs
`network timezone: Europe/Bucharest (offset UTC-2)` and `clock synced`;
`status` shows the IP and RSSI.

## 22. SD socket is SPI-wired; DAT3/CS is GPIO17

**Symptom.** Not observed yet because no card is attached. The board's TF
socket is wired for SPI (MOSI GPIO1, SCK GPIO2, MISO GPIO3, CS GPIO17), and
the printer wiki lists it that way.

**Cause.** The firmware mounts 1-bit SDMMC on CMD/D0/CLK and does not drive
GPIO17. A floating DAT3/CS can make the card start in SPI mode, so the
SDMMC init fails.

**Fix.** Drive GPIO17 high before mounting, which deselects SPI mode and lets
the card enter SD mode. If a card still fails, switch the mount to SDSPI.

**Fix lives in.**
[`components/mosu_board/include/mosu_board.h`](../components/mosu_board/include/mosu_board.h)
(`MOSU_PIN_SD_CS`),
[`components/mosu_board/src/mosu_board_storage.c`](../components/mosu_board/src/mosu_board_storage.c)
(`mount_now`).

**Verification.** Done: with GPIO17 driven high, the 64 GB card initialised at
20 MHz 1-bit and the watch formatted and mounted it (`SD card formatted and
mounted at /sdcard`). The card was later pushed full of clips and read back
with `ls`. Whether the pin drive is strictly required was not tested by
removing it.

## 23. `tools/build.ps1` aborted on normal stderr and rustup's gcc

**Symptom.** `.\tools\build.ps1` stopped while sourcing `export.ps1` with
`NativeCommandError`, then configure failed because `xtensa-esp32s3-elf-gcc`
came from `C:\Users\...\.rustup\toolchains\esp\...`, not the IDF toolchain.

**Cause.** The wrapper set `$ErrorActionPreference = "Stop"`; PowerShell 5.1
turns normal native stderr (the IDF banner, CMake warnings) into terminating
errors. The espup/rustup Xtensa toolchain was earlier in `PATH`, so the IDF
version check rejected it.

**Fix.** Run the wrapper with normal error handling and check
`$LASTEXITCODE` explicitly after every native command; prepend the
IDF-managed `xtensa-esp-elf` version from `tools/tools.json` to `PATH`.

**Fix lives in.** [`tools/build.ps1`](../tools/build.ps1).

**Verification.** A clean `build` from an empty `build/` and `sdkconfig`
finishes with `Project build complete` and zero `warning:`/`error:` lines.

## 24. A PC-formatted FAT32 card is rejected with `FR_NO_FILESYSTEM`

**Symptom.** A 64 GB SDXC card that Windows reads and writes without complaint
failed every boot with `vfs_fat_sdmmc: failed to mount card (13)` and
`mosu_sd: SD card not mounted: ESP_FAIL`. `sd dump` showed a valid MBR
(type `0x0C`, start LBA 63) and a valid FAT32 VBR: 512-byte sectors, 64
sectors per cluster, 8212 reserved sectors, 2 FATs, 15232 FAT sectors,
signature `55AA`.

**Cause.** The formatter sized the data area for 1,949,730 clusters but wrote
15,232 FAT sectors, which hold 1,949,696 entries. FatFs validates the table
against the volume in `find_volume`: `fs->fsize < (szbfat + SS - 1) / SS`
becomes `15232 < 15233`, so it returns `FR_NO_FILESYSTEM` (13). Windows does
not run that check, which is why the card looked healthy on the PC.

**Fix.** Format on the watch: `sd format yes` (or `--format` in
`tools/push_sd.py`) runs `f_mkfs` through
`esp_vfs_fat_sdmmc_mount(..., format_if_mount_failed = true)` with a 32 KB
allocation unit, producing MBR + FAT32 with a complete table. The same console
adds the upload side (`receive`, `md`, `ls`) so the files can be pushed over
USB without a card reader.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_storage.c`](../components/mosu_board/src/mosu_board_storage.c)
(`mosu_board_sdcard_format`, `mount_now`),
[`components/mosu_diag/src/mosu_diag.c`](../components/mosu_diag/src/mosu_diag.c)
(`sd format yes`, `receive`, `md`, `ls`),
[`tools/push_sd.py`](../tools/push_sd.py).

**Verification.** `sd format yes` printed `sd format: ESP_OK, mounted`; the
tool then pushed `mosu.cfg` and all voice clips, each accepted by the device's
CRC-32, and `ls /sdcard/voice` listed the files.

## 25. Console CR-to-LF conversion corrupted binary uploads

**Symptom.** The first upload attempt ended with
`receive: CRC mismatch (got 0x0D4A1185, want 0x0A4A1185)` for the payload
`hello world`: the device computed the right CRC but read the wrong trailer.
Every WAV with a `0x0D` byte was rejected.

**Cause.** `CONFIG_LIBC_STDIN_LINE_ENDING_CR` makes the USB Serial/JTAG VFS
translate every incoming `0x0D` to `0x0A` (`usb_serial_jtag_vfs.c`,
`rx_mode`). That is right for a terminal and wrong for a binary file.

**Fix.** Binary uploads no longer go through the console VFS: `read_exact()`
reads `usb_serial_jtag_read_bytes()` straight from the USB Serial/JTAG driver's
ring buffer, which has no CR translation. The console's own input stays in its
normal CR mode. The console task's stack grew to 8 KB to hold the 4 KB chunk
buffer. The host waits for `RX <cumulative>` after each 4 KB chunk and for
`RX DONE` after the CRC, and resynchronises with `help` if a transfer is
abandoned.

**Fix lives in.**
[`components/mosu_diag/src/mosu_diag.c`](../components/mosu_diag/src/mosu_diag.c)
(`receive_file`, `read_exact`), [`tools/push_sd.py`](../tools/push_sd.py).

**Verification.** A 1 KB file containing every byte value passed the CRC
check, and so did all 79 files in the full push.

## 26. Binary upload ran at 6 KiB/s and lost the odd byte

**Symptom.** The first full voice push took tens of minutes, and about one
4 KB chunk in 300 never arrived, leaving the watch waiting and the host
retrying the file. `rx bench` (the same read loop without a file) measured
6.3 KiB/s, so the SD card was not the cause.

**Cause.** Two problems in the bare console path. The USB Serial/JTAG VFS does
not block: when its 64-byte hardware FIFO is empty it returns
`-1`/`EWOULDBLOCK`, and `read_exact()` treated that as "no data yet" and slept
10 ms, so every 64-byte packet cost a full 10 ms (64 bytes / 10 ms = 6.4 KiB/s).
Polling the FIFO from a task also loses a byte if a higher-priority task runs
between the "bytes available" query and the reads.

**Fix.** Install the USB Serial/JTAG driver with an 8 KB receive ring in
`mosu_diag_start()` and feed `read_exact()` from
`usb_serial_jtag_read_bytes()`. The ISR drains the hardware FIFO into the ring
whatever the task priorities are, one call returns a whole block, and the VFS
CR-to-LF translation is bypassed. Measured rate on COM11 is ~450 KiB/s with no
losses in 1.5 MB. Using the driver *through the VFS* was tested first and was
slower (10 KiB/s) because that path still reads one character per call.
`rx bench <bytes>` reports the rate on the watch.

**Fix lives in.**
[`components/mosu_diag/src/mosu_diag.c`](../components/mosu_diag/src/mosu_diag.c)
(`read_exact`, `rx_bench`, `mosu_diag_start`).

**Verification.** `rx bench 524288` reports ~450 KiB/s three times in a row;
the full 79-file push completes in about 20 s at ~230-300 KiB/s. One push
retried a single file after a lost 4 KB chunk; the tool restarts that file and
the CRC-32 still guarantees what lands on the card.

## 27. Display-off left the AMOLED driver biased all night

**Symptom.** The 400 mAh cell lasted only a few hours even though the face was
off most of the time.

**Cause.** `mosu_board_display_off()` sent display-off (`0x28`) and brightness
zero, but never sleep-in (`0x10`). Display-off stops the scan while the panel's
driver, bias and DC-DC stay powered, which is a constant load of roughly
10-25 mA - most of a 400 mAh cell in a day.

**Fix.** Display-off is followed by a 5 ms wait and sleep-in; waking sends
sleep-out (`0x11`), waits 120 ms, draws the first frame while the display is
still off, then sends display-on and restores brightness. The reorder matters:
sleep-in loses the GRAM, so switching the display on first would flash
undefined pixels.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c)
(`mosu_board_display_prepare_wake`, `mosu_board_display_off`),
[`main/app_main.c`](../main/app_main.c) (`screen_on`).

**Verification.** Off/on and a screenshot after wake work with no E lines; the
measured drain comparison is pending.

## 28. A 35 deg / 150 ms wrist raise woke the face far too often

**Symptom.** The face lit on small hand movements and the battery paid for it;
the wearer found the raise too sensitive.

**Cause.** The detector fired on any 35 degree change from the baseline that
held for 150 ms. Picking something up or turning the wrist qualifies, and each
false wake costs a full `screen_on_ms` window.

**Fix.** Default threshold 50 degrees, hold 300 ms, refractory 5 s. On top of
that, the watch measures its own screen-on duty per hour and, when it is above
12% or there are more than 10 wakes, raises the threshold by 2 degrees and
shortens the lit window by 1 s (bounded +14 s and -7 s, never below 8 s).
Three calm hours give a step back. The configured values remain the baseline.

**Fix lives in.**
[`components/mosu_core/src/mosu_wrist.c`](../components/mosu_core/src/mosu_wrist.c),
[`components/mosu_policy/src/mosu_policy.c`](../components/mosu_policy/src/mosu_policy.c)
(`mosu_power_learn_*`), [`main/app_main.c`](../main/app_main.c)
(`apply_power_learning`), `sd_card/mosu.cfg`.

**Verification.** Host tests cover the bounds; the wrist test now requires the
longer hold. A 6 h+ drain comparison is pending.

## 29. USB console open reset the CPU and wiped the counters

**Symptom.** `stats` always reported `up 0h00m` and a fresh wake count, even
after a window that started hours earlier.

**Cause.** Opening the USB Serial/JTAG console resets the chip
(`rst:0x15 USB_UART_CHIP_RESET`). `RTC_DATA_ATTR` variables are re-initialised
by the C runtime on a normal reset; only `RTC_NOINIT_ATTR` survives it.

**Fix.** The power counters and baseline live in `.rtc.noinit`, guarded by a
magic value so a cold boot starts clean. The elapsed time comes from the
external RTC, so a reset does not distort the window.

**Fix lives in.** [`main/app_main.c`](../main/app_main.c) (`stats_init`,
`diag_stats`).

**Verification.** After `stats reset`, waiting two minutes and reconnecting
shows `up 0h02m` with the same baseline percentage.

## 30. Half a white screen and a freeze when the face woke

**Symptom.** After v1.1.0 the watch sometimes came back with roughly a third of
the panel white and the UI unresponsive, then recovered later or stayed stuck.

**Cause.** Three things combined. The wake path called `lv_refr_now()` from the
**app task** while holding the LVGL port lock; the QSPI flush is asynchronous,
so the call returned after a few of the 50-line buffers and the display was
switched on mid-frame — the untouched third still held the undefined GRAM left
by sleep-in. The renderer also ran on the app task's **3584-byte stack**, and
the LVGL task could not proceed while the app task held the lock.

**Fix.** The app task only invalidates; the 12 KB LVGL task renders. Sleep-in
is reverted: display-off keeps the last frame, so a wake can never show
undefined pixels. The main task stack is 8192 bytes, and a dedicated power
manager now owns the panel and the sleep lock.

**Fix lives in.** [`main/app_main.c`](../main/app_main.c) (`screen_on`),
[`components/mosu_power/src/mosu_power.c`](../components/mosu_power/src/mosu_power.c),
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c).

**Verification.** 100 forced `power screen off`/`power screen on` cycles
completed with no hang, and a screenshot taken after the cycles is a complete
410x502 face.

## 31. Lift-to-wake only responded to a hard shake

**Symptom.** After v1.1.2 the face stayed dark for a normal wrist raise; only a
deliberate shake woke it. Loosening the angle did not help.

**Cause.** The detector required the arm to have been still for `still_before`
milliseconds *at the sample where the orientation crossed the angle threshold*.
A real raise is continuous: the arm leaves the resting orientation, and by the
time it has turned 45 degrees the stillness timer has been reset by every
movement sample. The gate could only be satisfied if the whole 45 degrees
happened between two samples — which is exactly what a shake does. The
stillness was measured, but it was read at the wrong moment.

**Fix.** Stillness is no longer a gate. It is used only (a) to stop the resting
baseline from following the arm during a movement and (b) to decide when a new
resting orientation has settled. A raise now needs: a real orientation change,
the new orientation held for 250 ms, the screen normal not vertical, and the
refractory window. A candidate that fails those tests is adopted as the new
baseline after 1.5 s, so the detector can never get stuck measuring from an arm
position the wearer left. The IMU is sampled at 20 Hz to match the QMI8658's
21 Hz low-power ODR; 8 Hz gave only three samples per raise.

**Fix lives in.**
[`components/mosu_core/src/mosu_wrist.c`](../components/mosu_core/src/mosu_wrist.c),
[`components/mosu_core/include/mosu_core/mosu_core.h`](../components/mosu_core/include/mosu_core/mosu_core.h),
[`main/app_main.c`](../main/app_main.c) (poll rate, raise counters).

**Verification.** Host simulation: a continuous 300 ms raise, a raise that
starts while the arm is swinging, a vertical-glass reach that must not fire, the
baseline-recovery path, the refractory window and a 3-degree wobble. `stats`
prints `raise: N fired / M candidates, last DEG, nz` and the `motion [s]`
console command prints live samples and `RAISE` so the gesture can be checked
on the wrist.

## 32. The PMU powered eight rails nothing uses

**Symptom.** No failure; the battery simply drained faster than the power model
predicted.

**Cause.** The AXP2101 comes out of reset with `0x80 = 0x0F`, `0x90 = 0xFF` and
`0x91 = 0x01`: DCDC1-4, all four ALDOs, both BLDOs, CPUSLDO and both DLDOs on.
On this board the schematic connects only DCDC1 (VCC3V3) and ALDO1/2/4; DCDC2-4
have no inductor or load, BLDO1/2 and CPUSLDO are open pins, DLDO1/2 are switch
outputs with no wire, and ALDO3 is the vibration motor, which this firmware
never drives. Each idle rail still burns its quiescent current.

**Fix.** `mosu_board_pmu_init()` now switches the unused rails off explicitly
and logs the result (`rails: dcdc 0x01, ldo 0x0B/0x00`). The rail table is in
the driver, `pmu` prints every rail and `pmu rail <name> on|off` changes one, so
a wrong assumption can be undone at the console without a rebuild.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_i2c.c`](../components/mosu_board/src/mosu_board_i2c.c),
[`components/mosu_board/include/mosu_board.h`](../components/mosu_board/include/mosu_board.h),
[`components/mosu_diag/src/mosu_diag.c`](../components/mosu_diag/src/mosu_diag.c).

**Verification.** After the change: I2C scan still finds 7 devices, the IMU
reads, the SD card mounts, a 410x502 screenshot is written to it and the audio
test plays. Current saving is not measured yet; see `docs/power.md`.

## 33. Background logs and polling kept the CPU busy for nothing

**Symptom.** No visible failure, but the watch was harder to keep asleep than
the design implied, and a failed sync could stall for a moment.

**Cause.** Three small things added up. The log level defaulted to INFO, so
every wake and sync wrote lines to USB-Serial/JTAG; when no host is draining
the port, those writes block until the driver's buffer has room. The console
task polled `read(STDIN_FILENO)` every 200 ms even with no host. The network
task woke every 60 s to look at the clock, and the RTC was read ten times a
second while the face was on even though the minute had not changed.

**Fix.** The default log level is `error` (set in Kconfig and again at the top
of `app_main`), so a production watch is silent unless something fails; the
level remains a `mosu.cfg` setting. The console blocks on the USB driver
instead of polling; the network task sleeps until the next scheduled sync or
presence scan; the RTC is read once a second while the face is on. The power
counters have a `stats = true|false` switch (default off; the console says
`stats : disabled`).

**Fix lives in.**
[`main/app_main.c`](../main/app_main.c),
[`components/mosu_diag/src/mosu_diag.c`](../components/mosu_diag/src/mosu_diag.c),
[`components/mosu_net/src/mosu_net.c`](../components/mosu_net/src/mosu_net.c),
[`components/mosu_config/src/mosu_config.c`](../components/mosu_config/src/mosu_config.c).

**Verification.** Boot is quiet apart from the console banner; `stats` reports
the switch; the host suite still passes and the watch runs normally.

## 34. Sleep-in loses the GRAM, so wake paints it black first

**Symptom.** None yet, by design: panel sleep-in was reverted in v1.1.1 because
waking it showed the undefined GRAM (the white third).

**Cause.** `0x10` removes power from the panel's DC-DC and the GRAM contents are
undefined on the next `0x11`.

**Fix.** Sleep-in is back, behind `display.panel_sleep` (on in the shipped
config). On wake the display stays off while the driver sends `0x11`, waits
120 ms, and writes a full black frame to the GRAM in 8-line DMA chunks; only
then does it send `0x29` and restore brightness. The wearer sees black for
about one frame, never an undefined pixel. If a future panel revision still
flashes, set `panel_sleep = false` in `mosu.cfg`.

**Fix lives in.**
[`components/mosu_board/src/mosu_board_display.c`](../components/mosu_board/src/mosu_board_display.c).

**Verification.** 130 forced off/on cycles with sleep-in enabled, no hang, and
a complete 410x502 screenshot afterwards. The absence of a white flash is a
visual check for the wearer.

## 35. The watch drew ~110 mA with the face off

**Symptom.** The SD power log showed 91% → 35% in 1.73 h (about 129 mA on a
400 mAh cell) with the screen off for every sample and only one wake in that
time. A later run showed 90% → 47% in 1.57 h (110 mA), again screen-off.

**Cause.** `sleep_entries` — counted by a PM light-sleep enter callback — stayed
at 0 for the whole discharge. Every `NO_LIGHT_SLEEP` lock was inactive and the
per-task run-time stats showed no runaway task, so this was not a lock or a
spinning loop in our code. Two contributors were found and removed:

1. `esp_wifi_init()` registers a skip-light-sleep callback
   (`esp_wifi_internal_is_tsf_active`) that `esp_wifi_stop()` does not remove;
   only `esp_wifi_deinit()` does. We now deinit the driver after every sync and
   reinit it before the next one.
2. The LVGL port task ("taskLVGL") wakes core 1 every millisecond even with
   `lvgl_port_stop()` and its tick timer stopped. On a dual-core ESP32-S3 that
   prevents automatic light sleep. The task is now suspended whenever the face
   is off and resumed before LVGL is resumed on wake. The touch GPIO wake is
   independent of that task and still wakes the app.

**Fix lives in.**
[`components/mosu_net/src/mosu_net.c`](../components/mosu_net/src/mosu_net.c),
[`main/app_main.c`](../main/app_main.c).

**Verification.** In a two-minute unplugged run after the LVGL change,
`sleep_entries` rose from 0 to 1203 (roughly 10 sleeps/s, matching the 100 ms
IMU poll) and the cell stayed at 99% with the voltage flat. After the final
LVGL wait-floor fix, a three-minute run reached 5131 sleeps with the cell still
at 99% and 4154 mV — at the old ~110 mA that window would have dropped ~1.5%.
A 90-second plugged-in test with the face off confirmed the console stays
responsive: the firmware holds a `NO_LIGHT_SLEEP` lock while VBUS is present
and releases it on battery (USB Serial/JTAG cannot wake the chip from light
sleep on this board). Ten off/on cycles and a screenshot confirmed the face
still wakes and draws correctly.

A 100% → 40% soak over 22.04 h was completed on the v1.1.4 candidate with the
face off in 99.6% of samples and 1536 light-sleep entries per minute; it did not
reach empty, so the earlier 25.9 h full discharge remains the end-to-end
runtime figure. `/sdcard/power.log` holds both runs.

## 36. A zero sleep count in the log was a false green

**Symptom.** The captured `/sdcard/power.log` ends a discharge run with
`sleep_entries = 0` after a USB reconnect, even though the preceding samples
show 1.95 million entries. A reader — or a CI check that only counts rows — can
take that zero as proof that the chip never slept, or take the mere existence of
the file as proof that the power fix held.

**Cause.** The counter was a RAM-only `volatile uint32_t`. Opening the USB
console resets the CPU, so every reconnect put it back to zero while the rest of
the power counters already lived in `.rtc.noinit`. The log format also has no
header or run marker: a card can hold several nights of samples, and comparing
across a recharge invents battery rises that never happened in one run.

**Fix.** `s_sleep_entries` now lives in RTC no-init memory with the other power
counters, and the stats magic was bumped so an old layout is never reinterpreted.
The PM callback is registered even when diagnostics are off, so a `sleep : 0`
line in `stats` means the chip really did not sleep. `stats` prints
`sleep : N light-sleep entries (survives console reconnect)`.
`tools/check_power_log.py` parses a captured log, splits it at recharges and
reboots, and fails on a stalled or falling sleep counter, a screen left on, a
battery that rises inside a discharge run, or a sampling gap. Its `--selftest`
feeds it four known-bad logs so the gate itself cannot be decorative.

**Fix lives in.** [`main/app_main.c`](../main/app_main.c),
[`components/mosu_diag/src/mosu_diag.c`](../components/mosu_diag/src/mosu_diag.c),
[`components/mosu_diag/include/mosu_diag.h`](../components/mosu_diag/include/mosu_diag.h),
[`tools/check_power_log.py`](../tools/check_power_log.py).

**Verification.** The checker's selftest rejects all four bad logs. On the
watch, a 2-minute unplugged run left `stats` reporting 2923 sleep entries after
the USB console was reopened — the old code would have shown 0. The 22 h soak
then produced 2,006,672 entries in `stats` and 1,989,660 across its discharge
rows; the 1017-row log passes `check_power_log.py --allow-gaps` (100% → 40% in
22.04 h, 0.4% screen-on samples). Temporary `stats` and `diagnostics` were
turned off again afterwards and `stats` now reports "disabled".

## 37. `luni` sounded foreign with the dictionary single palatal

**Symptom.** After the weekdays were corrected to dictionary pronunciation,
the listener reported that all seven were better but `luni` was the one that
still sounded off.

**Cause.** The `ro_RO-mihai-medium` model card declares `phoneme_type: espeak`
and the Romanian `ro` voice. Its training counts contain `ɾ` but never `r`, and
never the single-character affricate `ʦ` — so espeak-ng, not dictionary IPA, is
the authority for the strings this voice can say naturally. espeak-ng produced
a doubled palatal for `luni` (`lˈunʲʲ`); the dictionary form `/ˈlunʲ/` is
linguistically right but is not the token the model learned for this word.

**Fix.** Keep dictionary stress and the tap `ɾ` for all weekdays (the research
confirmed `miercuri` /ˈmjer.kurʲ/ and `duminică` /duˈminikə/), and restore the
espeak-trained doubled palatal only for `luni`. `luni_compare.wav` renders the
single palatal, the doubled palatal and a palatal glide for a listening check.

**Fix lives in.** [`tools/mosu_g2p.py`](../tools/mosu_g2p.py) (`LEXICON`).

**Verification.** `mosu_g2p.py --selftest` passes and `check_voice.py` accepts
all 78 clips. The wearer listened to `luni_compare.wav` on the watch and
confirmed the restored doubled-palatal variant sounds right; the clips were
pushed to the card and the voice gate passed again before release.

## Adding a finding

Add a row to the summary and a section with the same fields: **Symptom**
(what a person sees), **Cause** (the mechanism, not the commit), **Fix** (what
changed and why it is enough), **Fix lives in** (real paths) and
**Verification** (what was actually run, and what still needs the watch). Keep
it factual: the next maintainer needs the failure mode, not a verdict about the
person who hit it.
