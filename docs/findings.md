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
`sync_hours` window - up to six hours by default.

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
0.66x and 1.21x.

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

## Adding a finding

Add a row to the summary and a section with the same fields: **Symptom**
(what a person sees), **Cause** (the mechanism, not the commit), **Fix** (what
changed and why it is enough), **Fix lives in** (real paths) and
**Verification** (what was actually run, and what still needs the watch). Keep
it factual: the next maintainer needs the failure mode, not a verdict about the
person who hit it.
