# Power

The watch is designed to spend almost all of its life with the panel off, the
LVGL task stopped, the radio off, and the CPU in automatic light sleep between
accelerometer samples. This page explains where the remaining current goes,
how to force each state, how to measure it, and what counts as acceptable.

**Every current on this page is an estimate until it has been measured.** The
repository does not contain a measured power figure yet. The numbers below come
from the first-order model in [`tools/power_budget.py`](../tools/power_budget.py);
they are there to size the battery and to catch a regression, not to promise a
runtime. Replace them with PPK2 values before telling anyone how long the watch
lasts.

## The model

```
I_avg = I_sleep + I_touch
      + motion_poll_hz * (imu_sample_ms / 1000) * I_imu
      + (wakes + raises) * screen_on_s * I_screen / 86400
      + announcements * speak_s * I_audio / 86400
      + (24 / sync_hours) * wifi_s * I_wifi / 86400

days = capacity_mAh / I_avg / 24
```

Run it with no arguments:

```sh
python tools/power_budget.py
python tools/power_budget.py --capacity 300 --capacity 400 --capacity 500
```

The script reads `brightness`, `screen_on_ms`, `motion_poll_hz` and
`sync_hours` from `sd_card/mosu.cfg` when that file is present, so the output
follows the shipped configuration. Every current is a flag (`--sleep-ma`,
`--touch-ma`, `--screen-ma`, ...), so measured values can be fed back in
without editing the script.

The screen term assumes every touch/boot wake and every wrist raise starts a
full `screen_on_s` window. A raise while the face is already on is counted
again, so heavy use is overestimated rather than understated. The model also
assumes every raise passes the angle and refractory filters; change `--raises`
to the number that actually reaches the screen if you have measured it.

With the shipped defaults (400 mAh, brightness 55, 11.25 s screen-on, 10 Hz
IMU, 6 h sync, 20 touch/boot wakes and 60 wrist raises per day, 8
announcements) the model gives:

| Term | Average | Share |
| --- | --- | --- |
| Light sleep + touch controller idle | 2.10 mA | 66% |
| IMU polling at 10 Hz | 0.60 mA | 19% |
| Screen-on windows (11.25 s) | 0.37 mA | 12% |
| Speaking | 0.07 mA | 2% |
| Wi-Fi sync | 0.04 mA | 1% |
| **Total** | **3.18 mA** | **100%** |

That is about **5.2 days from the fitted 400 mAh cell** (3.9 days from
300 mAh and 6.6 days from 500 mAh), by the model only. The 2.1 mA floor comes
from two assumptions: 0.6 mA in light sleep and 1.5 mA for the touch
controller. If the measured floor is higher, the runtime drops proportionally;
the touch controller is the first thing to measure.

## What owners of this board measured

Waveshare issue #6 ("High Baseline Power Consumption During Idle") collected
meter-backed measurements from several owners of this exact board:

* **ALDO2 is the AMOLED supply switch**, not a pull-up (GPIO13 is TE). SLPIN
  alone leaves the external ELVDD/ELVSS boost running, costing about 30 mA.
  Gating ALDO2 on screen-off and restoring it before SLPOUT is the single
  biggest saving. `display_off()` / `display_on()` now do this.
* **An initialised I2S channel prevents light sleep on ESP32-S3**, even when
  idle. `mosu_board_audio_idle()` now deletes the codec and the I2S channel
  and rebuilds them on the next announcement.
* **ES8311 REG0D bit2 (PDN_VREF)**: the vendor codec driver writes `0xFA`;
  keeping the reset value `0xFC` avoids roughly 12 mA of extra sleep current.
  We patch it after every codec init.
* **The supplied cell is not 400 mAh.** Two independent charge tests and a
  meter-backed estimate found roughly 100–200 mAh (the cell measures
  28.1 × 23.3 × 4.0 mm, not 402728). Runtime estimates based on 400 mAh are
  wrong by 2–4×. The drain line in `stats` still assumes 400 mAh and must be
  read as an index, not an absolute current.
* A separate port (SquachWatch) measured 60–100 mA for an idle Bluetooth
  controller and ~30 mA for an idle codec; after deinitialising the radios and
  parking the codec it reached ~23 h. Bluetooth is compiled out of MosuTime.

## Battery and charger

The fitted cell is a 402728 pouch (4.0 x 27 x 28 mm), 3.7 V nominal,
**400 mAh**, two wires with no thermistor. The AXP2101 profile is set in
`mosu_board_i2c.c` and read back by `pmu`:

| Setting | Register | Value | Meaning |
| --- | --- | --- | --- |
| Constant current | `0x62` | `0x08` | 200 mA (0.5 C) |
| Target voltage | `0x64` | `0x03` | 4.2 V |
| Termination | `0x63` | `0x01` | 25 mA (C/16) |
| Precharge | `0x61` | `0x02` | 50 mA (0.125 C) |
| TS pin | `0x50` | `0x10` | fixed input, no NTC |
| PWRKEY off time | `0x27` | bits 3:2 = `11` | hold 10 s to cut power |

0.5 C is the usual fast-charge ceiling for a small pouch cell and keeps the
charge under about two hours from empty. Change `0x62` to `0x04` (100 mA,
0.25 C) if the cell runs warm; do not exceed 1 C (400 mA, `0x10`).

### PMU rails

The AXP2101 comes out of reset with every rail enabled. Only a few are wired on
this board, so `mosu_board_pmu_init()` switches the rest off and logs the
result (`rails: dcdc 0x01, ldo 0x0B/0x00`). The map comes from the Waveshare
schematic:

| Rail | Voltage | Load | Kept |
| --- | --- | --- | --- |
| DCDC1 | 3.3 V | VCC3V3: ESP32-S3, AMOLED VCI/VDDIO, touch, IMU, SD, codec | yes |
| DCDC2 | 0.9 V | no inductor or load | no |
| DCDC3 | 1.2 V | no inductor or load | no |
| DCDC4 | 1.8 V | no inductor or load | no |
| DCDC5 | — | not connected | no |
| ALDO1 | 3.3 V | ALDO input / pull-ups | yes |
| ALDO2 | 3.3 V | DSI_PWR_EN pull-up (display enable) | yes |
| ALDO3 | 3.3 V | vibration motor (this firmware never drives it) | no |
| ALDO4 | 1.8 V | board pull-up | yes |
| BLDO1/2 | 1.2 / 2.8 V | pins left open | no |
| CPUSLDO | 1.2 V | pin left open | no |
| DLDO1/2 | switch | pins left open | no |

`pmu` prints every rail, and `pmu rail <name> on|off` changes one without a
rebuild, so an assumption can be undone on the spot. The saving is only the
quiescent current of seven idle regulators; it is real but it is not the
dominant term. It has not been measured with a meter yet.

Bluetooth is compiled out (`CONFIG_BT_ENABLED=n`, asserted in
`sdkconfig.defaults`); the radio is Wi-Fi only.

### What dominates

* **At low use**, the always-on housekeeping dominates: light sleep plus the
  touch controller. Nothing in the firmware can reduce the touch controller's
  idle current; only a different part or a different wake scheme can.
* **At high use**, the screen-on windows take over. 200 wakes of 11.25 s at
  35 mA is 0.9 mA average on its own. `screen_on_ms` and the number of wrist
  raises are the two biggest software levers.
* **IMU polling** costs about 0.06 mA at 1 Hz, 0.24 mA at 4 Hz, 0.6 mA at
  10 Hz and 1.2 mA at 20 Hz under the model's 1.5 ms / 40 mA burst assumption.
  It buys wrist-raise latency, nothing else. The QMI8658 also has a
  wake-on-motion interrupt (INT1 is wired to GPIO21) that would let the CPU
  sleep until the arm moves; that is the next step, not yet implemented.
* **Wi-Fi** is small on average because it is on for seconds every
  `sync_hours`, but a failed join can take 15 s per configured network, plus
  the geolocation and SNTP timeouts. A watch that cannot reach its network
  spends more energy failing than succeeding.
* **Audio** is a short, high-current burst. At eight announcements a day it is
  noise in the budget; at a hundred a day it is not.
* **Presence scans** are not in the model yet. When no configured SSID is
  visible, the net task scans for one every 15 minutes. A 2 s scan at about
  120 mA averages roughly 0.27 mA over the 15-minute window; that is not
  negligible against a 3.18 mA total. Measure it before promising runtime,
  and increase the interval or disable `wifi_always_on`-style retries if it
  is too high.

**Measured on the final firmware, 400 mAh cell:** 99% → 0% in **25.9 hours**
(≈15 mA average) with only 17 wakes in that time and 1.95 million light-sleep
entries. The first 21 hours used about 2.5%/hour (≈10 mA); the last five hours
were much steeper (sleep entries stop and the cell sags), which looks like the
PMU/DFS struggling near empty rather than a normal-load regression. That is a
five-fold improvement over the original ~5 hours.

`/sdcard/power.log` records every minute of this run and is the source of these
numbers.

## Quiet by default

Production is silent unless something fails:

* `log_level = error` in `mosu.cfg` and in the Kconfig default, so boot and
  background logs are not written over USB-Serial/JTAG. Writes can block when
  no host is draining the port. Set `info` while diagnosing.
* `stats = false`: the power counters and the `stats` command are off. They
  are RAM-only and cost nothing measurable, but the switch removes even that.
  Enable them for a drain test (`push_sd.py --stats`, or `stats = true`).
* `diagnostics = false`: no per-minute `/sdcard/power.log`, no 5-minute NVS
  battery record, no PM lock/task dumps and no light-sleep counter. Set it to
  `true` when measuring; the code stays in the firmware either way.
* Sync interval is 12 h. Each sync costs roughly 0.5 mAh, so 2/day instead of
  4/day saves a little and matches the wearer's "not several times a day"
  requirement; the trade-off is more RTC drift between corrections.
* The console blocks on the USB driver instead of polling every 200 ms, and
  the network task sleeps until the next scheduled sync or presence scan.
* `screen_on_ms = 11250` (25% below the old 15 s; the hourly learner can
  shorten it to 8 s).
* The FT3168 touch controller scans every 80 ms in monitor mode (register
  `0x89`) instead of the vendor's 40 ms, and enters monitor 1 s after the last
  contact. The first contact still switches it to active mode at 12 ms.
* `panel_sleep = true`: the panel gets `0x10` while off. On wake the driver
  paints the whole GRAM black in 8-line DMA chunks before `0x29`, so the
  undefined GRAM can never show as a white band.

## States

The state machine is driven by `main/app_main.c`, with the screen/tap state in
`components/mosu_session`.

| State | What is running | Model estimate | Notes |
| --- | --- | --- | --- |
| Screen off, idle | Panel off, LVGL tick stopped, radio off, CPU in automatic light sleep, touch INT armed as a GPIO wake source | ~2.1 mA (0.6 sleep + 1.5 touch) | The floor. No rendering, no polling of the panel. |
| Screen off, IMU polling | The above, plus one accelerometer read every `1000 / motion_poll_hz` ms | ~2.4 mA at 4 Hz, ~2.7 at 10 Hz, ~3.3 at 20 Hz | The IMU runs at a 21 Hz low-power ODR and is only read on the poll; the burst is short. |
| Touch wake | The touch INT line wakes the CPU; the screen-on window starts | Transient; see screen on | The touch controller itself was already powered. |
| Screen on | Panel on at `brightness`, LVGL running, CPU capped at 160 MHz with a `NO_LIGHT_SLEEP` lock, touch polled by LVGL | ~35 mA at brightness 55 for the normal face | AMOLED current depends on lit pixels: the face is mostly black, while the `test screen colors` pattern lights much more of the panel and draws more at the same brightness. That pattern is a panel check, not the budget figure. |
| Speaking | Screen on, ES8311 + PA enabled, I2S clocked, WAV read from SD | ~120 mA while a clip plays, plus the screen | Short bursts. Measure the whole phrase, including gaps. |
| Wi-Fi sync | Radio on, DHCP, timezone lookup (HTTP by default, HTTPS optional), SNTP | ~120 mA for roughly 8 s on a clean success | A failure is longer: up to 15 s per network, 8 s for the timezone, 15 s for SNTP. |

The point estimates above come from the model. As a sanity band for planning,
use these **ranges — also estimates, not measurements**: screen off idle
1.5–3 mA, screen off + IMU at 10 Hz 2.0–4 mA, screen on with the normal face
20–60 mA, speaking 80–200 mA, Wi-Fi sync 80–200 mA. They are deliberately wide:
the low end assumes a better touch controller and panel than the model, the
high end leaves room for a brighter panel, a louder PA and Wi-Fi retries. A
reading outside the band is a finding; a reading inside it still needs to be
recorded.

The CPU is configured for 160/80 MHz and light sleep in `app_main.c`; the
display's `NO_LIGHT_SLEEP` lock is held only while the panel is on. The
native-USB console adds a second lock while a host is connected (see the
measurement warnings). Deep sleep is not used: the touch wake and the
wrist-raise poll must stay live, and light sleep keeps the CPU, the I2C bus
and the panel state intact across a wake.

## Forcing a state

The maintenance console is the only sane way to measure a single state. The
bring-up console extension adds the commands below; `help` lists exactly what
the current build has. `status`, `time`, `sync`, `say`, `bright`, `vol`, `tap`
and `reboot` are the long-standing ones.

| Command | State it forces |
| --- | --- |
| `power screen off` | Panel off, brightness zero, display lock released. The normal off state, with the state machine still running. |
| `power screen on` | Panel on at the configured brightness and held on, independent of the activity timer. |
| `power idle` | Release any forced state and return to the normal state machine (screen follows touch/wrist, radio off). |
| `power wifi` | Run one Wi-Fi sync now, then switch the radio off again. |
| `pm` | Print the power-management state: CPU limits, light sleep, and which PM locks are held. |
| `pmu` | Print battery voltage, percent, charging and VBUS, straight from the AXP2101. |
| `stats` | Uptime, screen-on seconds, wakes and the estimated average current since `stats reset`. |
| `rtc` | Print the PCF85063 time and whether it is considered valid. |
| `imu` | Print `WHO_AM_I` and one accelerometer sample with its magnitude (1 g at rest). |
| `i2c` | Scan the I2C bus and list the addresses that answer. |
| `heap`, `tasks` | Free heap / PSRAM and the task list with stack high-water marks. |
| `sd remount`, `config` | Re-mount the card and print the active configuration (passwords are never printed). |
| `test screen` | Panel patterns `colors`, `grid`, `corners`, `off`; `off` returns to the face. |

The `power` commands leave a request for the app task, which performs it on
its next pass (the console task never touches LVGL or the PMU). `power screen
on` holds the face lit until `power screen off` or `power idle`; `power idle`
also restores the normal face and the activity timer.

Two practical warnings:

1. **The console shares the USB connector.** Plugging in USB asserts VBUS,
   which starts charging and hides the watch's own consumption. For any
   absolute current measurement, power the watch only from the PPK2 and use a
   data-only USB cable with the 5 V wire disconnected/blocked, or the board's
   UART pins with a 3.3 V USB-UART adapter. `power screen off` on a USB-powered
   watch is useful for relative comparisons, not for the table below.
2. `power screen on` holds the panel on until `power screen off` or
   `power idle`. Do not leave it on for a 24 h soak. If the build in front of
   you still applies the activity timer, set `screen_on_ms = 300000`
   temporarily for the measurement.
3. The native-USB console holds `NO_LIGHT_SLEEP` while a host is connected
   (`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`). `pm` shows the
   `usb_serial_jtag` lock active. Unplug USB, or use the UART0 pads, for a
   real light-sleep current. The option has no effect when the cable is out.

## Measuring from the watch itself

A meter is better, but the watch can measure its own duty cycle. The console's
`stats` command reports uptime, screen-on time, wake count, battery voltage and
an estimated average current since the last baseline:

```text
stats reset     # start a new window at the current percentage
# unplug USB, leave the watch alone, do not open the console
stats           # on reconnect: uptime, lit %, wakes, % used, mA estimate
```

Rules that make the number meaningful:

* the measurement must be on the cell: the USB console resets the CPU when it
  opens and holds `NO_LIGHT_SLEEP` while connected;
* the counters live in RTC no-init memory, so the reset from opening the
  console does not lose the window; the RTC clock supplies the elapsed time;
* the percentage is the AXP2101 voltage gauge, and one percent is 4 mAh on
  this cell. A one-hour window is very coarse; use six hours or a full day;
* `stats` also prints a `drain:` line once the window is longer than ten
  minutes and the watch is not charging.

The learned screen policy is visible in the same line (`auto: screen 12 s,
raise angle 56 deg`): a busy hour tightens the wrist threshold and shortens the
lit window by bounded steps, and three calm hours give a step back.

## Measurement with a Nordic PPK2

The PPK2 is the reference instrument here: it can source the watch and log
current from nanoamps to amps.

1. Charge the battery, then disconnect USB. Put a known-good FAT32 card with
   `mosu.cfg` and the voice clips in the watch. Set `log_level = info` unless
   you are deliberately measuring the logging cost.
2. Connect the PPK2 in **Source meter** mode at **3.8 V** to the battery
   connector (or in **Ampere meter** mode in series with the cell). Do not
   power the watch from USB. Check the polarity twice.
3. In nRF Connect Power Profiler, enable the power output, set the sample rate
   to **100 kS/s** for wake bursts and **1 kS/s** for long averages, and let
   the watch boot. The console must be reachable over the data-only cable.
4. Wait 60 s after boot. The first screen-on window, the first sync and the
   AXP2101 state changes all settle before the idle number is meaningful.
5. **Screen off, idle:** `power screen off`, wait 60 s, record the mean over at
   least 60 s. This is the floor and the most important number on the page.
6. **Screen off + IMU poll:** keep the screen off and record 3 minutes each at
   `motion_poll_hz` 1, 4, 8 and 16. The console cannot change it live, so edit
   `mosu.cfg` and `reboot` between runs. Compare the deltas with the model's
   0.06 / 0.24 / 0.48 / 0.96 mA.
7. **Screen on, normal face:** `power screen on`. Do not touch the glass. Record
   the mean over at least 3 minutes so the 180 s pixel shift is included. This
   is the number that belongs in the budget.
8. **Screen on, colours pattern:** `test screen colors` draws five horizontal
   bands (red, green, blue, white, grey). Record the mean for comparison with
   the normal face. It is a panel diagnostic, not a worst case: only one fifth
   of the panel is white at a time. Do not use it as the budget value.
9. **Speaking:** `say`, and record the burst mean and peak for a full phrase.
   Repeat at `vol 50` and `vol 100`; the PA current changes with volume.
10. **Wi-Fi sync:** `power wifi`, record the burst length and mean. Then break
    the password in `mosu.cfg`, reboot, and record a failed sync; that is the
    realistic worst case, and it can be several times longer.
11. **24 h average:** log the PPK2 for a full day of realistic use (wear it, or
    script the wake rate) and take the mean over the whole log. This is the
    only number that should be compared with the model's total.

Record the conditions with the numbers: commit, `mosu.cfg`, brightness,
`screen_on_ms`, `motion_poll_hz`, `sync_hours`, card model, battery capacity,
starting voltage, room temperature, PPK2 mode and sample rate. A current
without those is not reproducible.

### If you only have a USB power meter

A USB power meter measures what the charger delivers, which includes the
charging current. It cannot see the 2 mA sleep state, and with a charged cell
the charger current tapers to near zero while the watch is still running. Use
it only to confirm that the watch draws current, to see that a sync burst
exists, or to compare two brightness settings coarsely. **Do not put USB-meter
numbers in the measurement table** except as a charging-current note.

## Measurement table

Session 1 had no battery and no PPK2, so no current was measured; the table
below is still blank. The display, touch, audio and Wi-Fi states were exercised
functionally, not electrically.

Copy this table into the build record and fill it in. Leave a cell blank rather
than guessing.

| State | Forced by | Estimate (model) | Measured avg (mA) | Peak (mA) | Duration | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Screen off, idle | `power screen off` | 2.1 mA | | | 60 s | |
| Screen off + IMU, 1 Hz | config + `power screen off` | 2.2 mA | | | 3 min | |
| Screen off + IMU, 4 Hz | config + `power screen off` | 2.3 mA | | | 3 min | |
| Screen off + IMU, 8 Hz | config + `power screen off` | 2.6 mA | | | 3 min | shipped default |
| Screen off + IMU, 16 Hz | config + `power screen off` | 3.1 mA | | | 3 min | |
| Screen on, normal face, brightness 55 | `power screen on` | 35 mA | | | 3 min | budget value |
| Screen on, colours pattern | `test screen colors` | not modelled | | | 1 min | panel diagnostic, not the budget |
| Speaking, volume 85 | `say` | 120 mA | | | one phrase | |
| Wi-Fi sync, success | `power wifi` | 120 mA for ~8 s | | | burst | |
| Wi-Fi sync, failure | wrong password + `reboot` | 120 mA, much longer | | | burst | |
| 24 h, realistic use | none | 3.18 mA | | | 24 h | compare with model |

Run conditions: commit ____________, `mosu.cfg` ____________, brightness ____,
`screen_on_ms` ______, `motion_poll_hz` ____, `sync_hours` ____, card
____________, battery ______ mAh, start voltage ______ mV, temperature ______
°C, PPK2 mode ____________, sample rate ____________.

## Acceptance targets

These are design gates, not promises. They are chosen so that the model's
default duty cycle still leaves margin, and so that a regression is visible.

| Metric | Target | Why |
| --- | --- | --- |
| Screen off, idle | <= 3.0 mA | The model floor is 2.1 mA; more than 3 mA means something is still running. |
| Screen off + IMU at 8 Hz | <= 4.0 mA | Idle plus the modelled 0.48 mA burst, with room for measurement error. |
| Screen on, normal face at brightness 55 | <= 60 mA | The model uses 35 mA; the panel and CPU should not be far above it. |
| Speaking | <= 200 mA | 120 mA modelled; the PA and SD card add peaks. |
| Wi-Fi sync | <= 200 mA | 120 mA modelled; check the burst length separately. |
| 24 h average at the default duty cycle | <= 4.2 mA | Gives >= 5.2 days on the fitted 400 mAh cell; >= 3.0 days at 300 mAh. |
| 24 h soak | no reset, no watchdog, no stuck screen | A power bug usually shows up as a reset or a panel left on. |

`4.2 mA` is not a measured value and does not guarantee battery life. It is
`300 mAh / (3 days * 24 h)` and `500 mAh / (5 days * 24 h)` rounded, i.e. the
current at which the two capacities just meet the target. Age, cold, the
4.2 V charge limit and the battery icon's voltage-derived percentage all make
the real runtime shorter than the arithmetic.

## Open questions to measure

The model is only as good as its weakest assumption. These are unknown in the
repository today and should be filled in first:

* touch controller idle current (assumed 1.5 mA);
* light-sleep baseline with the radio off and the panel off (assumed 0.6 mA);
* panel current at brightness 55 for the actual mostly-black face (assumed
  35 mA), and how it scales with brightness;
* ES8311 + PA quiescent current when parked (assumed zero between clips);
* SD card idle current (assumed zero; cards differ);
* AXP2101 quiescent current and the accuracy of its battery percentage;
* the real length of an announcement and of a failed sync;
* the average cost of the 15-minute presence scan while away from home;
* the light-sleep floor once the USB console is unplugged (the bench build
  deliberately keeps the USB link alive while a host is attached).

Until those are measured, treat every runtime statement as a hypothesis.
