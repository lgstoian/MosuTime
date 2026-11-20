# AGENTS.md

A short orientation page for a future AI agent or human contributor. Read this
first, then [`BREADCRUMBS.md`](BREADCRUMBS.md) when you need to find a file, and
[`docs/architecture.md`](docs/architecture.md) for the data flow. Before touching
hardware, read [`docs/bringup.md`](docs/bringup.md).

## What MosuTime is

MosuTime is an ESP32-S3 watch for an elderly man with dementia who has sight in
one eye. It is built to show **one thing**: the time, as large as the screen
allows. There are no menus, notifications, steps, watch faces or settings on the
glass. The hardware is a Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502 CO5300
AMOLED, FT3168 touch, AXP2101 PMU, PCF85063 RTC, QMI8658 IMU, ES8311 codec,
microSD), running ESP-IDF 5.5.5 and LVGL 9.6.0.

The voice clips live on the SD card, never in flash. A double tap speaks the
time in Romanian.

## Design constraints

These are product requirements, not style preferences. A change that violates
one of them is a regression even if it looks nicer:

1. **One decision per glance.** Huge numerals, 12-hour time, no leading zero,
   no colon, no secondary information competing with the hour and minute.
2. **AM/PM is carried by colour.** White numerals in the morning, warm amber in
   the evening. There is no AM/PM text and no 24-hour mode.
3. **Off by default.** The AMOLED and the LVGL tick are stopped unless the
   wearer raises his wrist or touches the glass. The CPU sits in automatic
   light sleep between IMU samples and the radio is off unless a sync is due.
4. **Never surprise him.** Brightness and layout are fixed by `mosu.cfg`; a
   failed time sync never blanks the clock; a missing SD card never stops the
   boot; the screen must not flash on power transitions.
5. **Speak only when asked.** A double tap is the only way to request the time.
   The announcement is the exact hour without "fix" wording: *"Este ora
   două."*
6. **One gesture, no UI to learn.** If a new feature needs an explanation, it
   probably does not belong on this watch.

## Build, flash and test

Run these from the repository root. On Windows, source `export.bat` from the
ESP-IDF 5.5.x installation first, or use `tools/build.ps1`.

```sh
# Firmware (production build)
idf.py set-target esp32s3        # only on a fresh checkout/build directory
idf.py build
idf.py -p COM11 flash monitor

# Windows wrapper
.\tools\build.ps1 -Port COM11 -Flash -Monitor
```

The board's only reachable USB port is the ESP32-S3 native USB, so the
production console is USB Serial/JTAG; UART0 is on reserved pads.
`tools/build.ps1` also puts the IDF-managed Xtensa toolchain ahead of an
espup/rustup one and does not abort on the normal stderr from `export.ps1`.

```sh
# Host tests: the fast, hardware-free safety net
cmake -S test/host -B test/host/build
cmake --build test/host/build
./test/host/build/mosu_tests

# Desktop simulator: renders sim/out/*.bmp
cmake -S sim -B sim/build
cmake --build sim/build
mkdir -p sim/out && ./sim/build/mosu_sim sim/out
```

On Windows use `-G "MinGW Makefiles"` for the two CMake projects. The host
suite prints a check count and exits non-zero on failure; do not report a pass
without reading that line.

```sh
# ESP32 emulator: boot the real merged firmware without the watch
idf.py -B build-emu \
  -D SDKCONFIG=build-emu/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;tools/sdkconfig.emulator" \
  build merge-bin

tools/emu/capture.sh    # four states -> docs/emulator/*.png
tools/emu/smoke.sh      # double tap -> phrase builder; IMU -> panel wake
```

The emulator is covered in [`tools/emu/README.md`](tools/emu/README.md); it
needs Linux/WSL, esp-emu 1.0.0-rc2, the part SDK and Rust. The emulator build
disables light sleep and the SD mount (`CONFIG_MOSU_SKIP_SD`) because esp-emu
models neither. The production configuration is untouched.

Other useful commands:

```sh
python tools/gen_voice.py --out sd_card/voice   # generate the 78 clips
python tools/check_voice.py sd_card/voice       # verify a voice folder
python tools/push_sd.py COM11 --format          # format the card and upload
python tools/mosu_gui.py                        # Windows manager app
python tools/mosu_g2p.py --selftest             # check the pronunciation rules
python tools/power_budget.py                    # first-order power model
python tools/contact_sheet.py                   # docs/accessibility/hours.png
.\tools\release.ps1                             # firmware + exe + SD zip
```

## Layer and dependency map

The layering is deliberate. Dependencies point down; the board does not know
about the UI, the UI does not know about Wi-Fi, and the pure C code knows
nothing about ESP-IDF.

```
main/app_main.c                 boot order, power state machine, wiring
      |
mosu_time  mosu_net  mosu_audio  mosu_ui  mosu_diag     services
      |
mosu_power                    panel, DFS and sleep lock
      |
mosu_hal                      hardware contract (display, PMU, RTC)
      |
mosu_board                    the only board-specific component
      |
ESP-IDF drivers, LVGL, esp_lcd, esp_codec_dev

mosu_core     pure C: time formatting, phrases, battery curve, tap/wrist,
              seven-segment digit drawing, BMP writer
mosu_session  pure C: screen/tap/battery/wifi-icon state machine
mosu_policy   pure C: timezone offset/geo parsing, failed-sync backoff,
              learned Wi-Fi presence and screen-duty policy
mosu_config   pure C INI parser; only mosu_config_store.c owns NVS
mosu_tasks    task creation, stack policy and the high-water report
```

- **Pure C, host-testable:** `components/mosu_core`,
  `components/mosu_session`, `components/mosu_policy`, and
  `components/mosu_config` except `mosu_config_store.c`. These are compiled
  into the firmware, the host tests and the desktop simulator. They must not
  include ESP-IDF headers.
- **Board:** `components/mosu_board` is the only component that may mention this
  hardware. A port replaces it and nothing else.
- **Services:** `mosu_time`, `mosu_net`, `mosu_audio`, `mosu_ui`, `mosu_diag`.
  They use ESP-IDF/LVGL and call the board API.
- **Application:** `main/app_main.c` owns the boot sequence and the screen
  state machine.

## Rules for changes

1. **No fonts and no images in the firmware.** The numerals are drawn from
   geometry in `components/mosu_core/src/mosu_digits.c`; the voice is on the SD
   card. Adding a font, an icon asset or an embedded WAV is a design change,
   not a convenience.
2. **MIT and SPDX.** Every new source file starts with
   `SPDX-FileCopyrightText: 2026 Stoian Alexandru` and
   `SPDX-License-Identifier: MIT`. Third-party code goes in
   `THIRD_PARTY_NOTICES.md`.
3. **Keep host-testable code pure C.** No ESP-IDF headers, no FreeRTOS, no
   global mutable state in `mosu_core`/`mosu_session`/`mosu_policy`/
   `mosu_config`. If a rule matters to the wearer, put it in the pure layer and
   test it on the host.
4. **No false green.** Host tests, the simulator and esp-emu are not hardware
   proof. Only the checklist in `docs/bringup.md` is. If you have not run a
   step on the real watch, say "not run", not "works".
5. **Do not invent a measurement.** Power figures in `docs/power.md` are model
   estimates until a PPK2 value is recorded. The same applies to audio,
   brightness and touch claims.
6. **Hardware verification is required before shipping.** Touch wake, panel
   power transitions, audio, SD fallback, Wi-Fi/RTC and the 24 h soak all need
   the real board.
7. **Update the map.** When you add a component, tool or test, add it to
   `BREADCRUMBS.md`. When you fix a non-obvious bug, add an entry to
   [`docs/findings.md`](docs/findings.md) with the symptom, cause and the file
   that carries the fix.
8. **Keep generated artefacts out of commits.** `build/`, `build-emu/`, host
   test build directories and emulator logs are ignored; do not force-add them.

## Where to go next

- [`BREADCRUMBS.md`](BREADCRUMBS.md) - "I want to..." map, components, tests,
  tools and docs.
- [`docs/architecture.md`](docs/architecture.md) - layers, boot, screen state
  machine, time, voice, network, failure behaviour, porting.
- [`docs/bringup.md`](docs/bringup.md) - the first-hardware checklist and
  go/no-go criteria.
- [`docs/power.md`](docs/power.md) - power states, the model, PPK2 procedure
  and acceptance targets.
- [`tools/emu/README.md`](tools/emu/README.md) - running the firmware in
  esp-emu, capturing the panel and driving touch/IMU gestures.
- [`docs/findings.md`](docs/findings.md) - the engineering log of bugs found
  during desk and emulator testing.
