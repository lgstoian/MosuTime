# MosuTime v1.1.3

Lift-to-wake that works, and a PMU that stops powering empty pins.

## The raise detector was measuring the wrong instant

v1.1.2 was still built around a stillness gate: the arm had to have been still
for 600 ms **at the sample where the orientation crossed 45 degrees**. A real
raise is continuous, so by the time it turns 45 degrees the stillness timer has
just been reset by the movement. The only way to satisfy that gate is to change
45 degrees between two samples, which is exactly what a hard shake does.

Now stillness is not a gate. It is used only to keep the resting baseline from
following the arm and to recognise when a new position has settled. A raise is:

* a real orientation change (45 degrees by default),
* held for 250 ms,
* with the screen normal not vertical (the glass faces the wearer),
* outside the 6 s refractory window.

A candidate that does not qualify is adopted as the new baseline after 1.5 s,
so the detector cannot get stuck measuring from an arm position that was left
behind. A slow raise is protected from being absorbed into the baseline, and a
raise that starts while the arm is already swinging (walking) is accepted.

The IMU is sampled at 10 Hz, up from 8, matching the QMI8658's 21 Hz low-power
output rate without doubling the polling cost.

## New ways to see the gesture

* `motion [seconds]` prints live accelerometer vectors and `RAISE` when the
  detector fires. Run it, move the arm, read the result.
* `stats` now prints `raise: N fired / M candidates, last DEG, nz`, kept in RTC
  memory across USB resets. `stats reset` clears it with the drain baseline.

## Battery

* **Unused PMU rails are switched off.** The AXP2101 powers up with DCDC1-4,
  ALDO1-4, BLDO1-2, CPUSLDO and DLDO1-2 all enabled. On this board DCDC2-4 have
  no inductor or load, BLDO1/2, CPUSLDO and DLDO1/2 are open pins, and ALDO3
  feeds the vibration motor, which the firmware never drives. They are now
  explicitly off (`rails: dcdc 0x01, ldo 0x0B/0x00`), leaving DCDC1 (3.3 V for
  the ESP32, panel, touch, IMU, SD and codec) and ALDO1/2/4.
* `pmu` prints every rail; `pmu rail <name> on|off` changes one at the console.
* **Bluetooth is compiled out** (`CONFIG_BT_ENABLED=n`, asserted in
  `sdkconfig.defaults`). The radio is Wi-Fi only, for the time sync.

## Verified

* 714 host checks, 0 failures, including a continuous raise, a raise during
  arm swing, a slow raise, a vertical-glass reach that must not fire, the
  baseline-recovery path, the refractory window and a 3-degree wobble.
* On the watch: `rails: dcdc 0x01, ldo 0x0B/0x00`; I2C scan finds 7 devices,
  the IMU reads, the SD card mounts, a 410x502 screenshot is written, the audio
  test plays, and `motion` streams live samples.
* The wrist test itself is yours: `motion 10` before wearing it, then `stats`.
