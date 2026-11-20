# MosuTime v1.1.0

Power, gesture and companion-app release.

## Firmware

* **The panel now sleeps.** Display-off (`0x28`) is followed by sleep-in
  (`0x10`), and waking does sleep-out, draws the first frame while the glass is
  still dark, then switches the display on. Display-off alone left the AMOLED
  driver and its bias powered all night, which is the main reason the cell did
  not last a day.
* **A calmer wrist raise.** The default is now 50 degrees held for 300 ms with
  a 5 s gap, instead of 35 degrees and 150 ms. Picking something up no longer
  lights the face.
* **Learned screen policy.** Each hour the watch measures its own screen-on
  duty and wake count. A busy hour raises the wrist threshold by 2 degrees and
  shortens the lit window by 1 s; three calm hours give a step back. Bounds:
  at most +14 degrees and -7 s, never below an 8 s window. The configured
  values stay the baseline.
* **Learned Wi-Fi presence.** The radio is still off between syncs, but the
  presence scan now follows a per-hour score learned from successes and
  failures: 15 minutes in hours when the watch is normally home, up to 4 hours
  in hours when it never is.
* `stats` / `stats reset` on the console: uptime, screen-on seconds, wakes,
  battery and an estimated average current. Counters live in RTC no-init
  memory, so opening the USB console (which resets the CPU) does not lose a
  window that started on battery.
* `config` defaults changed: `motion_angle_deg = 50`,
  `motion_refractory_ms = 5000`.

## Manager 1.1.0

* DPI-aware window, dark title bar, application icon.
* Fixed the unreadable port drop-down, the light notebook chrome, the white
  progress trough and the settings rows that could overlap at narrow widths.
* Dashboard now shows uptime, screen time, wakes and the active learned policy.
* Worker threads no longer touch Tk directly: callbacks go through a queue
  drained by the main loop, which removes the sporadic freezes.
* Remembers the last successful port.

## Verified

Firmware builds and flashes clean; host tests 471 checks, 0 failures; the
display off/on and screenshot paths work with the panel asleep; `stats`
survives a USB reconnect and reports the window; the manager connects, reads
the dashboard, pushes `mosu.cfg`, lists the card and flashes a merged image.

**Measured drain is still pending.** Use `stats reset`, unplug for six hours or
more, then reconnect and read `stats`; the first number for this release should
be recorded in `docs/power.md`.
