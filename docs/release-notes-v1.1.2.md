# MosuTime v1.1.2

Lift-to-wake, tuned to the wearer instead of a fixed guess.

## What changed

v1.1.1's raise gate (60°, 400 ms hold, 1.5 s of stillness, glass facing the
wearer at |gz| ≥ 0.45) was too strict for a real arm: normal movement never
satisfied it, so the face stayed dark.

* **Looser starting point:** 45°, 250 ms hold, 600 ms of stillness, face-up
  gate 0.20, 6 s between raises.
* **It learns both ways.** Once an hour the watch looks at who woke the face:
  * if the wearer touched the glass and no raise fired, the gates were too
    strict: angle −4°, hold −50 ms, stillness −250 ms, face gate −0.05;
  * if there are many raises or the screen is lit too much, it takes those
    back first, then raises the configured angle and shortens the lit window.
  * floors: 25°, 150 ms, 250 ms stillness, face 0.10. The gesture can never be
    disabled, only relaxed to a point.
* `stats` prints the live gates (`auto: raise 45 deg/250 ms, still 600 ms,
  face 200/1000`) so the tuning is visible.
* Teaching signal: a single tap on the glass when a raise fails is enough;
  the watch counts it and loosens at the next hourly evaluation.

Everything from v1.1.1 stays: the freeze fix, the HAL, the power manager, the
task registry, the scene manager, the hourly Wi-Fi retry and the age-based
Wi-Fi icon.

## Verified

499 host checks, 0 failures; the watch boots with the looser gates applied and
`stats` reports them. The 100-cycle off/on test from v1.1.1 still holds.
