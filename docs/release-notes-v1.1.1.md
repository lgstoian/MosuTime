# MosuTime v1.1.1

Stability, power architecture and the freeze fix.

## Fixed

* **No more white third of the screen.** The app task called `lv_refr_now()`
  while holding the LVGL lock; the QSPI flush is asynchronous, so the display
  was switched on mid-frame and the untouched band kept the undefined GRAM.
  The app task now only invalidates; the 12 KB LVGL task renders.
* **No more freeze.** The same call ran the renderer on the 3584-byte app
  stack and held the LVGL lock. The app stack is 8192 bytes and rendering never
  runs there.
* **Panel sleep-in removed for now.** It lost the GRAM, so a wake could show
  white. Display-off keeps the last frame; wake is instant and clean. The
  deferred, guarded sleep design is documented for a later release.
* Wi-Fi failures no longer retry in five minutes. A failed sync waits at least
  the learned presence interval (1 h, growing to 6 h), and the icon follows the
  age of the last successful sync instead of every attempt.

## New architecture

* `mosu_hal` — basic hardware contract: display on/off, brightness, touch wake,
  battery, power-off, RTC save. The board implements one table; a port changes
  one component.
* `mosu_power` — dedicated power manager and single owner of the panel state,
  brightness, the no-light-sleep lock, DFS 160/80 MHz and shutdown. Both cores
  remain scheduled; frequency scales with load.
* `mosu_tasks` — one creation point for tasks (stack, priority, core, optional
  PSRAM stack) with a high-water report. The diag, net and audio tasks use it.
* Scene manager — `clock`, `blank` and the existing bring-up patterns. There is
  deliberately no window manager: the wearer must never see a window.
* `mem` on the console: task stacks and headroom, internal/PSRAM free, largest
  block and the reset reason.
* Wi-Fi/LWIP large buffers may spill into PSRAM; DMA buffers and task stacks
  stay in internal RAM.

## Verified

100 forced panel off/on cycles with no hang, a complete 410x502 screenshot
after them, 490 host checks with 0 failures, `mem` reporting the three
registered tasks, scenes switching, and the manager exe flashing this image.
The measured battery drain is still the next step.
