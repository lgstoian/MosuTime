# Battery drain audit

This is a temporary measurement procedure, not part of the product. The
firmware has a `audit` console command with kill switches for each major load
and a `stats` line that reports how long the radio and audio paths were on.

## Why this exists

The power model predicts about 3.2 mA and five days from the 400 mAh cell.
Reported runtime is a few hours, which implies roughly 80 mA average. That is a
continuous load, not a log line or a sync interval. The audit switches exist to
find it by subtraction.

## What the switches do

| Command | Effect |
| --- | --- |
| `audit` | Print the switch states and radio/audio on-times since boot. |
| `audit wifi off` | Stop and disarm all syncs and presence scans, park the radio. |
| `audit imu off` | Stop the 8 Hz accelerometer poll; wrist raise will not wake the face. |
| `audit touch off` | Ignore touch wake and disarm the touch GPIO wake source. |
| `audit audio off` | Refuse announcements and park the codec. |
| `audit panel off` | Disable panel sleep-in (`0x10`) while the face is off. |
| `audit <subsystem> on` | Restore it. |

The switches are runtime only. Opening the USB console resets the CPU, so set
the switch **every time** before starting a run. They only set flags: the
driver work is done by the task that owns that driver, never by the console
task. (Calling `esp_wifi_stop`, `gpio_set_intr_type` or `esp_codec_dev_close`
from the console task deadlocked the board in an earlier build.)

## Full discharge to the SD card

The firmware appends one line per minute to `/sdcard/power.log` **when
`device.diagnostics = true` in `mosu.cfg`** (it is off by default in
production):

```
epoch,pct,mV,charging,screen,wakes,boots,sleep_entries
```

The card survives a dead cell, so this is the definitive test:

1. Charge to 100%, connect USB, confirm `pmu` says `charging no` at 97–100%.
2. Clear the old run and leave the card ready:
   ```
   rm /sdcard/power.log
   ```
   (the file is recreated on the next sample)
3. Unplug and leave it alone until it dies.
4. Plug back in and read the curve:
   ```
   cat /sdcard/power.log
   ```
   Each minute's row shows the voltage and how many wakes happened. The slope
   from 100% to 0% is the honest average current for a 400 mAh cell.

5. Copy the log off the watch and **gate it** instead of trusting that rows
   exist:
   ```
   python tools/check_power_log.py path/to/power.log
   ```
   It splits the file at recharges and reboots, then fails on a stalled or
   falling `sleep_entries`, a screen left on for more than a quarter of the
   samples, a battery that rises inside one discharge run, or a sampling gap.
   A zero in `sleep_entries` used to mean "the console was opened": the counter
   is now kept in RTC no-init memory, so a zero means the chip really did not
   sleep. `stats` prints the same counter as `sleep : ... entries` even while
   `diagnostics` is off, provided `stats = true`.

## One-hour A/B runs (older procedure)

1. Charge the watch until `pmu` reports `charging no` at 97–100%.
2. Plug in, wait for the console prompt, then run:

   ```
   audit wifi off     (or the subsystem under test; omit for the baseline)
   stats reset
   audit
   ```

3. **Unplug the cable.** Leave the watch alone for exactly one hour. Do not
   open the console during the run: the USB lock keeps the CPU awake.
4. Plug back in and run:

   ```
   stats
   audit
   ```

5. Record: battery % at start (from step 2) and at the end, `screen on ...%`,
   `on : radio ... s, audio ... s`, and the switch states.

With 400 mAh, 80 mA for one hour is about 20% of the cell, so a load that large
is obvious in one run. A 1% drop in an hour is about 4 mA.

## Run order

| Run | Switch off | Question it answers |
| --- | --- | --- |
| 0 | none | Baseline: is the drain real and how large? |
| 1 | wifi | Is the radio stuck on or syncing too often? |
| 2 | imu | Is the 8 Hz poll keeping the CPU awake? |
| 3 | touch | Is the FT3168 the floor? |
| 4 | audio | Is the codec/PA leaking current after a phrase? |
| 5 | panel | Does sleep-in actually reduce panel current? |

Stop after the run that removes most of the drain; the remaining runs are only
needed if the first ones do not explain it.

## Interpreting the result

* `screen on` above ~20%: the session/touch path is holding the face on.
* `radio ... s` more than a few seconds in an hour: Wi-Fi is not parking.
* `audio ... s` more than a few seconds: the PA/codec is not being parked.
* All counters low but the battery drops fast: suspect a hardware load (panel
  supply or cell). Compare runs 3 and 5 first, then use a meter on the cell.
* Watch warm during the run: radio or CPU; cold: panel or cell.

## Known-good state from the desk

After the boot face times out (25 s), `pm` shows `mosu_display` and
`mosu_say` with Active 0, and `display` shows `off` with `panel sleep on`. The
only active lock while the console is connected is `usb_serial_jtag`, which is
expected. The radio was on for about 1 second during the boot sync attempt and
then parked.
