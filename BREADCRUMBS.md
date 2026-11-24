# Breadcrumbs

A hand-written navigation map for MosuTime. Use it when you know *what* you
want to change but not *where* it lives. Every path below is a real file in
this repository; if a link goes stale, fix it in the same commit that moved the
file.

For the product constraints and the rules of the road, read
[`AGENTS.md`](AGENTS.md). For how the pieces talk to each other, read
[`docs/architecture.md`](docs/architecture.md).

## I want to...

| I want to... | Start here |
| --- | --- |
| Understand what this watch is and why it is so plain | [`README.md`](README.md), [`AGENTS.md`](AGENTS.md) |
| See the data flow, boot order and screen state machine | [`docs/architecture.md`](docs/architecture.md) |
| Change the size, spacing or position of the digits | [`components/mosu_core/include/mosu_core/mosu_layout.h`](components/mosu_core/include/mosu_core/mosu_layout.h), then render in [`sim/mosu_sim.c`](sim/mosu_sim.c) |
| Change the shape of a digit | [`components/mosu_core/src/mosu_digits.c`](components/mosu_core/src/mosu_digits.c) |
| Change the face: colours, battery icon, Wi-Fi icon, pixel shift | [`components/mosu_ui/src/mosu_ui.c`](components/mosu_ui/src/mosu_ui.c), colours in [`sd_card/mosu.cfg`](sd_card/mosu.cfg) |
| Change 12-hour formatting, leading zeros or the `--` placeholder | [`components/mosu_core/src/mosu_timefmt.c`](components/mosu_core/src/mosu_timefmt.c), tests in [`test/host/test_core.c`](test/host/test_core.c) |
| Change what the watch says or which clips it joins | [`components/mosu_core/src/mosu_phrase.c`](components/mosu_core/src/mosu_phrase.c), [`docs/voice.md`](docs/voice.md) |
| Change tap, double-tap, hold or cooldown behaviour | [`components/mosu_session/src/mosu_session.c`](components/mosu_session/src/mosu_session.c), [`components/mosu_core/src/mosu_tap.c`](components/mosu_core/src/mosu_tap.c) |
| Change when the screen turns off, or how a touch extends it | [`components/mosu_session/src/mosu_session.c`](components/mosu_session/src/mosu_session.c), [`main/app_main.c`](main/app_main.c) |
| Change wrist-raise sensitivity or the IMU poll | [`components/mosu_core/src/mosu_wrist.c`](components/mosu_core/src/mosu_wrist.c), `display.motion_*` in [`sd_card/mosu.cfg`](sd_card/mosu.cfg), `motion [s]` on the console |
| Check or change a PMU rail | [`components/mosu_board/src/mosu_board_i2c.c`](components/mosu_board/src/mosu_board_i2c.c) (`s_rails`), `pmu` and `pmu rail <name> on|off` |
| Change power states, sleep or the CPU limits | [`components/mosu_power/src/mosu_power.c`](components/mosu_power/src/mosu_power.c), [`docs/power.md`](docs/power.md) |
| Quiet the watch or turn the power counters back on | `device.log_level` and `device.stats` in [`sd_card/mosu.cfg`](sd_card/mosu.cfg), `stats` on the console |
| Change panel sleep or the touch monitor rate | `display.panel_sleep`, `power panel on|off`, `touch` / `touch rate` on the console, [`components/mosu_board/src/mosu_board_display.c`](components/mosu_board/src/mosu_board_display.c) |
| Port to another board or change the hardware contract | [`components/mosu_hal/include/mosu_hal.h`](components/mosu_hal/include/mosu_hal.h), [`components/mosu_hal/src/mosu_hal_board.c`](components/mosu_hal/src/mosu_hal_board.c), [`components/mosu_board`](components/mosu_board) |
| Add a task or change a stack size | [`components/mosu_tasks/include/mosu_tasks.h`](components/mosu_tasks/include/mosu_tasks.h), `mem` on the console |
| Change what the display shows (clock / blank) | [`components/mosu_ui/include/mosu_ui.h`](components/mosu_ui/include/mosu_ui.h) (`mosu_ui_set_scene`), `scene clock|blank` |
| Change a config key, its range or its default | [`components/mosu_config/src/mosu_config.c`](components/mosu_config/src/mosu_config.c), [`sd_card/mosu.cfg`](sd_card/mosu.cfg) |
| Understand the NVS "last good config" cache | [`components/mosu_config/src/mosu_config_store.c`](components/mosu_config/src/mosu_config_store.c) |
| Port to another panel or board | [`components/mosu_board`](components/mosu_board), "Porting to another board" in [`docs/architecture.md`](docs/architecture.md) |
| Debug a dead screen, touch, PMU or RTC on hardware | [`docs/bringup.md`](docs/bringup.md), [`components/mosu_board/src/mosu_board_display.c`](components/mosu_board/src/mosu_board_display.c), [`components/mosu_board/src/mosu_board_i2c.c`](components/mosu_board/src/mosu_board_i2c.c) |
| See how the SD card failure path works | [`components/mosu_board/src/mosu_board_storage.c`](components/mosu_board/src/mosu_board_storage.c), [`main/app_main.c`](main/app_main.c) |
| Change Wi-Fi, geolocation, SNTP or radio cycling | [`components/mosu_net/src/mosu_net.c`](components/mosu_net/src/mosu_net.c), [`components/mosu_policy/src/mosu_policy.c`](components/mosu_policy/src/mosu_policy.c) |
| Choose how often the time syncs per day | `clock.sync_per_day` in [`sd_card/mosu.cfg`](sd_card/mosu.cfg), [`components/mosu_net/src/mosu_net.c`](components/mosu_net/src/mosu_net.c) (`sync_interval_s`) |
| Provision Wi-Fi without an SD card | `wifi <ssid> <pass>` in [`components/mosu_diag/src/mosu_diag.c`](components/mosu_diag/src/mosu_diag.c), [`components/mosu_net/src/mosu_net.c`](components/mosu_net/src/mosu_net.c) (`mosu_net_set_network`) |
| Understand the return-home presence scan | [`components/mosu_net/src/mosu_net.c`](components/mosu_net/src/mosu_net.c) (`home_ap_visible`, `s_next_presence_s`), [`docs/architecture.md`](docs/architecture.md) |
| Understand the learned Wi-Fi presence interval or screen policy | [`components/mosu_policy/src/mosu_policy.c`](components/mosu_policy/src/mosu_policy.c) (`mosu_wifi_learn_*`, `mosu_power_learn_*`), [`main/app_main.c`](main/app_main.c) (`apply_power_learning`) |
| Measure the watch's own duty cycle and average current | `stats` / `stats reset` in [`components/mosu_diag/src/mosu_diag.c`](components/mosu_diag/src/mosu_diag.c), [`docs/power.md`](docs/power.md), one-hour A/B procedure in [`docs/audit.md`](docs/audit.md) |
| Change the IANA-to-POSIX timezone table or fallback | [`components/mosu_time/src/mosu_time.c`](components/mosu_time/src/mosu_time.c), [`components/mosu_policy/src/mosu_policy.c`](components/mosu_policy/src/mosu_policy.c) |
| Change announcement queueing or clip gaps | [`components/mosu_audio/src/mosu_audio.c`](components/mosu_audio/src/mosu_audio.c) |
| Add or change a maintenance-console command | [`components/mosu_diag/src/mosu_diag.c`](components/mosu_diag/src/mosu_diag.c), hooks in [`main/app_main.c`](main/app_main.c) |
| Format the SD card or push files onto it over USB | `python tools/push_sd.py COM11 --format`, then `sd format yes`, `receive <path> <size> /crc`, `md`, `ls`, `rm`, `rx bench` in [`components/mosu_diag/src/mosu_diag.c`](components/mosu_diag/src/mosu_diag.c) |
| Test without the watch | [`test/host`](test/host), [`sim/mosu_sim.c`](sim/mosu_sim.c), [`tools/emu/README.md`](tools/emu/README.md) |
| Reproduce a touch or wrist gesture in the emulator | [`tools/emu/smoke.sh`](tools/emu/smoke.sh), [`tools/emu/board-interactive.json`](tools/emu/board-interactive.json) |
| Capture the real firmware's face as a PNG | [`tools/emu/capture.sh`](tools/emu/capture.sh), [`tools/emu/reconstruct.py`](tools/emu/reconstruct.py) |
| Capture the real watch's face on hardware | `screenshot /sdcard/shot.bmp` on the console, [`components/mosu_ui/src/mosu_ui.c`](components/mosu_ui/src/mosu_ui.c) (`mosu_ui_snapshot`) |
| Generate or validate the Romanian voice clips | [`tools/gen_voice.py`](tools/gen_voice.py), [`tools/mosu_g2p.py`](tools/mosu_g2p.py), [`tools/check_voice.py`](tools/check_voice.py), [`docs/voice.md`](docs/voice.md) |
| Change how a word is pronounced | [`tools/mosu_g2p.py`](tools/mosu_g2p.py) (`LEXICON`), then `--selftest` |
| Estimate battery life, record a measurement or gate a captured power log | [`tools/power_budget.py`](tools/power_budget.py), [`tools/check_power_log.py`](tools/check_power_log.py), [`docs/power.md`](docs/power.md), [`docs/audit.md`](docs/audit.md) |
| Check accessibility, contrast or physical digit size | [`tools/contact_sheet.py`](tools/contact_sheet.py), [`docs/accessibility/README.md`](docs/accessibility/README.md) |
| Give the watch to the family / explain it in Romanian | [`docs/family-ro.md`](docs/family-ro.md) |
| Update firmware, config or a card in the field | [`docs/maintenance.md`](docs/maintenance.md) |
| Read a past bug's symptom, cause and fix | [`docs/findings.md`](docs/findings.md) |
| See what CI runs | [`.github/workflows/ci.yml`](.github/workflows/ci.yml) |

## Components

| Component | Responsibility | Key files |
| --- | --- | --- |
| `mosu_core` | Pure C, no ESP-IDF: 12-hour formatting, phrase builder, battery curve, tap detector, wrist detector, seven-segment digit drawing, BMP writer for hardware screenshots. | [`include/mosu_core/mosu_core.h`](components/mosu_core/include/mosu_core/mosu_core.h), [`include/mosu_core/mosu_layout.h`](components/mosu_core/include/mosu_core/mosu_layout.h), [`include/mosu_core/mosu_bmp.h`](components/mosu_core/include/mosu_core/mosu_bmp.h), [`src/`](components/mosu_core/src) |
| `mosu_session` | Pure C screen/tap/battery state machine: screen-on deadline, wake request, say request, battery smoothing, Wi-Fi icon mapping. | [`include/mosu_session.h`](components/mosu_session/include/mosu_session.h), [`src/mosu_session.c`](components/mosu_session/src/mosu_session.c) |
| `mosu_policy` | Pure C network and power policy: numeric UTC offset to POSIX TZ, geolocation response parsing, plausible-zone check, failed-sync backoff, learned per-hour Wi-Fi presence intervals and the bounded screen-duty adaptation. | [`include/mosu_policy.h`](components/mosu_policy/include/mosu_policy.h), [`src/mosu_policy.c`](components/mosu_policy/src/mosu_policy.c) |
| `mosu_config` | INI parser, range checks and sanitisation; `mosu_config_store.c` is the only part that touches NVS and the last-good cache. | [`include/mosu_config.h`](components/mosu_config/include/mosu_config.h), [`include/mosu_config_store.h`](components/mosu_config/include/mosu_config_store.h), [`src/`](components/mosu_config/src) |
| `mosu_board` | The only board-specific component: QSPI CO5300 panel, FT3168 touch, AXP2101 PMU, PCF85063A RTC, QMI8658 IMU at 0x6B, ES8311 audio and the SD card. | [`include/mosu_board.h`](components/mosu_board/include/mosu_board.h), [`src/`](components/mosu_board/src) |
| `mosu_hal` | Basic hardware contract used by the power manager (display, brightness, touch wake, battery, power-off, RTC save); the board provides the implementation. | [`include/mosu_hal.h`](components/mosu_hal/include/mosu_hal.h), [`src/mosu_hal_board.c`](components/mosu_hal/src/mosu_hal_board.c) |
| `mosu_power` | Dedicated power manager: panel state, brightness, no-light-sleep lock, DFS 160/80 MHz, battery read and shutdown. | [`include/mosu_power.h`](components/mosu_power/include/mosu_power.h), [`src/mosu_power.c`](components/mosu_power/src/mosu_power.c) |
| `mosu_tasks` | One place to create and track tasks: stack, priority, core, optional PSRAM stack, high-water report. | [`include/mosu_tasks.h`](components/mosu_tasks/include/mosu_tasks.h), [`src/mosu_tasks.c`](components/mosu_tasks/src/mosu_tasks.c) |
| `mosu_time` | UTC system clock, RTC restore/persist, SNTP write-back, IANA-to-POSIX timezone table and numeric-offset fallback. | [`include/mosu_time.h`](components/mosu_time/include/mosu_time.h), [`src/mosu_time.c`](components/mosu_time/src/mosu_time.c) |
| `mosu_net` | Wi-Fi association, HTTP (or HTTPS) geolocation, SNTP and switching the radio off again. Runs in its own task so the UI never blocks. | [`include/mosu_net.h`](components/mosu_net/include/mosu_net.h), [`src/mosu_net.c`](components/mosu_net/src/mosu_net.c) |
| `mosu_audio` | Announcement queue and clip chaining; never interrupts a sentence in progress. | [`include/mosu_audio.h`](components/mosu_audio/include/mosu_audio.h), [`src/mosu_audio.c`](components/mosu_audio/src/mosu_audio.c) |
| `mosu_ui` | The single LVGL watch face, status icons, tap border flash, pixel shift, bring-up test patterns and the BMP screen snapshot. | [`include/mosu_ui.h`](components/mosu_ui/include/mosu_ui.h), [`src/mosu_ui.c`](components/mosu_ui/src/mosu_ui.c) |
| `mosu_diag` | USB/UART maintenance console: status, time, sync, say, settime, bright, vol, tap, test patterns, screenshot, power, I2C/PMU/RTC/IMU/display dumps, SD format/receive/ls. | [`include/mosu_diag.h`](components/mosu_diag/include/mosu_diag.h), [`src/mosu_diag.c`](components/mosu_diag/src/mosu_diag.c) |
| `main` | Boot order, power-management locks, screen state machine and the wiring between the components. | [`app_main.c`](main/app_main.c), [`CMakeLists.txt`](main/CMakeLists.txt), [`Kconfig.projbuild`](main/Kconfig.projbuild) |
| `sim` | Desktop preview that renders the real layout and digit code into BMP files. Not a test. | [`mosu_sim.c`](sim/mosu_sim.c), [`CMakeLists.txt`](sim/CMakeLists.txt) |
| `test/host` | The host test binary and its suites. | [`CMakeLists.txt`](test/host/CMakeLists.txt), [`test_core.c`](test/host/test_core.c), [`test_session.c`](test/host/test_session.c), [`test_config.c`](test/host/test_config.c), [`test_bmp.c`](test/host/test_bmp.c), [`test_policy.c`](test/host/test_policy.c) |
| `tools/emu` | esp-emu board files, virtual panel/touch/IMU/chip parts, reconstruction and smoke scripts. | [`README.md`](tools/emu/README.md), [`board.json`](tools/emu/board.json), [`board-interactive.json`](tools/emu/board-interactive.json) |

## Tests and what they cover

| Test | Covers |
| --- | --- |
| [`test/host/test_core.c`](test/host/test_core.c) | 12-hour split and formatting (including midnight/noon and `--`), Romanian phrase selection and exact-hour rules, battery voltage-to-percent curve and buckets, tap state machine (double tap, hold, move, cooldown), wrist-raise detector. |
| [`test/host/test_session.c`](test/host/test_session.c) | Screen-on timing, touch/audio extending the window, double tap from a dark screen, tap detector resets, battery smoothing and immediate-on-wake, battery bucket edges, Wi-Fi icon rules, config clamping. |
| [`test/host/test_config.c`](test/host/test_config.c) | INI parser, sections, colours, booleans, invalid values keep the previous value, BOM handling, the shipped `sd_card/mosu.cfg`, long SSID/password fields, random and mutation fuzzing. |
| [`test/host/test_bmp.c`](test/host/test_bmp.c) | BMP header fields, row pitch, RGB565-to-BGR conversion and bit replication; the hardware screenshot writer. |
| [`test/host/test_policy.c`](test/host/test_policy.c) | Numeric UTC offsets (seconds/hours/half-hours/signs), POSIX TZ spelling, invalid inputs, geolocation parsing, the failed-sync backoff schedule, the learned Wi-Fi hour scores and the bounded screen-duty adaptation. |
| [`tools/emu/capture.sh`](tools/emu/capture.sh) | Boots the real merged firmware four times (AM, PM, unknown time, low battery), checks each reaches "watch face ready", and writes `docs/emulator/*.png`. |
| [`tools/emu/smoke.sh`](tools/emu/smoke.sh) | A double tap on the virtual FT3168 reaches the phrase builder (`h_9.wav`, `m_41.wav`, `d_1.wav` at 09:41 Tuesday); a wrist raise after the face sleeps produces panel `0x29`; no panic/assert. |
| [`.github/workflows/ci.yml`](.github/workflows/ci.yml) | Host tests, simulator render, voice-generator dry run, Python tool compilation, ESP-IDF firmware build and size summary, cppcheck on the pure C components. |

The host suite is the fast gate. The simulator checks proportions and colours
without hardware. The emulator is the only desk test that exercises LVGL, the
panel command sequence and the power state machine together. None of them
replaces [`docs/bringup.md`](docs/bringup.md).

## Tools

| Tool | What it does |
| --- | --- |
| [`tools/build.ps1`](tools/build.ps1) | Windows wrapper that sources ESP-IDF, builds, and optionally flashes/monitors. |
| [`tools/push_sd.py`](tools/push_sd.py) | Formats the card on the watch (`--format`) and uploads `mosu.cfg` plus the voice clips over the USB console with an ACK per 4 KB chunk and a CRC-32 per file. |
| [`tools/mosu_gui.py`](tools/mosu_gui.py), [`tools/mosu-gui.cmd`](tools/mosu-gui.cmd) | Windows manager: dashboard, console, a full `mosu.cfg` editor with the firmware's ranges, build/flash through `tools/build.ps1`, SD format and push, shutdown. |
| [`tools/gen_voice.py`](tools/gen_voice.py) | Runs the Romanian ONNX voice with onnxruntime and renders the 78 WAV clips into `sd_card/voice` (or `--out`). Trims silence, normalises, fades; `--preview` joins four announcements for a listening check. No GPL tool involved. |
| [`tools/mosu_g2p.py`](tools/mosu_g2p.py) | Romanian pronunciation: a 38-word IPA lexicon plus the `și` sandhi rule. `--selftest` checks the rules, `--plan` prints all 78 phrases with their phonemes. |
| [`tools/check_voice.py`](tools/check_voice.py) | Verifies a voice folder: exact names, mono 16-bit WAV, no clipping, no silent clips, and a duration that matches the phrase each clip says. |
| [`tools/power_budget.py`](tools/power_budget.py) | First-order current/runtime model. Reads the shipped config and lets every current be overridden with a flag. |
| [`tools/check_power_log.py`](tools/check_power_log.py) | Validates a captured `/sdcard/power.log`: continuous samples, battery falling only while uncharged, face mostly off and a rising light-sleep counter. `--selftest` supplies four known-bad logs. |
| [`tools/contact_sheet.py`](tools/contact_sheet.py) | Drives the simulator, builds the 16-scene accessibility sheet and regenerates `docs/accessibility/README.md` / `hours.png`. |
| [`tools/emu/capture.sh`](tools/emu/capture.sh) | Emulator capture cases: AM, PM with `settime`, unknown time, low battery. |
| [`tools/emu/smoke.sh`](tools/emu/smoke.sh) | Drives touch and IMU against the real firmware and asserts on the UART log and QSPI stream. |
| [`tools/emu/reconstruct.py`](tools/emu/reconstruct.py) | Turns the `--cosim-log` QSPI stream back into a 410x502 PNG. |
| [`tools/emu/board.json`](tools/emu/board.json) | Emulator board with register-file models of the AXP2101, PCF85063, ES8311 and a 3.9 V battery. |
| [`tools/emu/board-interactive.json`](tools/emu/board-interactive.json) | Adds the touch, IMU and `chips` parts so gestures and I2C reads both work on esp-emu rc2. |
| [`tools/emu/board-low.json`](tools/emu/board-low.json) | Low-battery variant used for the red battery capture. |
| [`tools/emu/board-unknown.json`](tools/emu/board-unknown.json) | Invalid RTC year variant that shows the `--` placeholder. |
| [`tools/sdkconfig.emulator`](tools/sdkconfig.emulator) | Emulator-only overlay: no SD mount, no light sleep. Never used by the production build. |

## Documentation

| Document | Read it for |
| --- | --- |
| [`README.md`](README.md) | The product in one page, the face, the hardware, build/SD/voice overview. |
| [`AGENTS.md`](AGENTS.md) | Orientation, constraints, commands, layering and contribution rules. |
| [`BREADCRUMBS.md`](BREADCRUMBS.md) | This map. |
| [`docs/architecture.md`](docs/architecture.md) | Layers, boot sequence, screen state machine, time/voice/network flow, failure behaviour, porting. |
| [`docs/bringup.md`](docs/bringup.md) | First-hardware checklist, expected boot log, I2C inventory, screen/touch/audio/Wi-Fi tests, 24 h soak, go/no-go. |
| [`docs/power.md`](docs/power.md) | Power states, the current model, PPK2 measurement procedure, acceptance targets and open questions. |
| [`docs/audit.md`](docs/audit.md) | Runtime `audit` kill switches and the one-hour A/B discharge procedure. |
| [`docs/voice.md`](docs/voice.md) | The 78 clips, Romanian grammar, naming, generation, preview, replacement and licences. |
| [`docs/maintenance.md`](docs/maintenance.md) | Editing `mosu.cfg`, runtime brightness/volume, changing networks, regenerating voice, USB updates, SD replacement, troubleshooting. |
| [`docs/release.md`](docs/release.md) | Building and publishing releases: merged firmware image, manager exe, SD card zip, checksums. |
| [`docs/family-ro.md`](docs/family-ro.md) | The one-page Romanian guide for the wearer and family. |
| [`docs/accessibility/README.md`](docs/accessibility/README.md) | Physical size math, viewing distance, contrast and the generated contact sheet. |
| [`docs/findings.md`](docs/findings.md) | Engineering log of issues found during emulator/desk testing, with symptom, cause, fix and file. |
| [`tools/emu/README.md`](tools/emu/README.md) | esp-emu setup, interactive parts, capture/smoke, what is and is not modelled. |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | Licences for Espressif components, LVGL, onnxruntime, the voice model and other third-party work. |

## Conventions and gotchas

- The project is C11. Host and simulator builds use `-Wall -Wextra -Werror`;
  keep them warning-free.
- Every source file carries an SPDX header. New files must too.
- The numerals are drawn, never typeset. `mosu_digits.c` is shared by the
  firmware and the simulator, so a layout change can be reviewed on a PC.
- `mosu_core`, `mosu_session`, `mosu_policy` and `mosu_config` must stay free
  of ESP-IDF headers. If a test needs a clock, pass a timestamp in; the pure
  layer has no global state.
- The panel is AMOLED: "off" means the panel's own off command and brightness
  zero, not just a black framebuffer. The glass has rounded corners; keep
  status icons and the corner test at a 48 px inset.
- The board's only reachable USB port is the ESP32-S3 native USB. The
  production console is USB Serial/JTAG; UART0 is on reserved pads.
- The QMI8658 answers at **0x6B** on this board (not 0x6A), and the
  PCF85063A time registers start at **0x04** (not the PCF8563's 0x02).
- `mosu.cfg` is read once at boot. `sd remount` remounts the card but does not
  reparse the file; `reboot` applies config changes.
- The watch formats its own card (`sd format yes`, FAT32/MBR/32 KB) and takes
  files over USB (`receive <path> <size> /crc`); `tools/push_sd.py` drives
  both. A card formatted only by Windows can be rejected with
  `FR_NO_FILESYSTEM` even though the PC opens it.
- `build/`, `build-emu/` and emulator logs are generated. Do not commit them.
- If a hardware claim has not been checked on the real watch, write "not run".
  The bring-up checklist is the only source of a green light.
