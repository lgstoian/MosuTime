# Running MosuTime in esp-emu

The watch hardware is a CO5300 AMOLED, an FT3168 touch controller, an AXP2101
PMU, a PCF85063 RTC and a QMI8658 IMU. [esp-emu](https://github.com/espressif/esp-emulator)
emulates the ESP32-S3 and its buses, and lets a host model answer on them. This
folder turns that into a real firmware test and a screenshot source.

What it gives us:

* the actual merged firmware boots (bootloader, ESP-IDF, LVGL, our `app_main`);
* the panel receives the real QSPI command and pixel stream;
* the four watch-face states can be captured as PNGs without a camera;
* the console can be driven (`settime`, `say`) and the result checked;
* a finger can tap the glass, and a wrist can turn: the FT3168 and QMI8658
  are virtual parts, so gestures reach the firmware's double-tap detector,
  phrase builder and wrist-raise detector.

The watch face is drawn by LVGL and sent to the panel over QSPI. The
reconstructor rebuilds the framebuffer from those transfers, so the PNGs below
are the firmware's output, not a re-implementation:

| AM | PM | time not set |
| --- | --- | --- |
| ![AM](../docs/emulator/am.png) | ![PM](../docs/emulator/pm.png) | ![unknown](../docs/emulator/unknown.png) |

`board-low.json` sets the AXP2101 ADC to 3.5 V, which exercises the red
battery bucket:

![low battery](../docs/emulator/low-battery.png)

## What works, and what does not

| Part | In esp-emu |
| --- | --- |
| ESP32-S3, PSRAM, flash, Wi-Fi | yes |
| CO5300 QSPI panel | observed with the part in `panel/`, pixels reconstructed from the log |
| FT3168 touch, 0x38, INT GPIO 38 | part in `touch/`: tap, double tap, hold, coordinates |
| QMI8658 accelerometer, 0x6A | part in `imu/`: `raise`, `lower`, `tilt:x,y,z` |
| AXP2101, PCF85063, ES8311 I2C | `regfile` in `board.json`; the `chips` part on the interactive board |
| SD card | not modelled; the emulator build has `CONFIG_MOSU_SKIP_SD=y` |
| Light sleep on S3 | not modelled; the emulator build disables PM |
| Audio output | not modelled; the phrase builder logs the missing clips instead |

The production build is untouched: `tools/sdkconfig.emulator` and
`CONFIG_MOSU_SKIP_SD` only apply to the `build-emu` directory.

## The interactive board

`board-interactive.json` is `board.json` plus the touch, IMU and chips parts.
It is the board `smoke.sh` runs, and the one to use by hand when the question
is "does this gesture reach the firmware?" rather than "what does the face
look like?".

| Part (board key) | What it is | Inputs |
| --- | --- | --- |
| `touch` | FT3168 at 0x38 on SCL 14 / SDA 15, driving INT on GPIO 38 low while pressed | `tap` (80 ms press), `double` (two 80 ms taps 250 ms apart), `hold`, `release`, `set:x=<n>`, `set:y=<n>`, `set:x=<n>,y=<n>` |
| `imu` | QMI8658 at 0x6A, WHO_AM_I `0x05`, ±4 g (8192 LSB/g) | `raise` (x=1,y=0,z=0), `lower` (x=0,y=0,z=1), `tilt:x,y,z` in g |
| `chips` | AXP2101 0x34, PCF85063 0x51 and ES8311 0x18 register files | none; mirrors the values `board.json` preloads |
| `panel` | the CO5300 QSPI panel, unchanged | none |

A gesture keeps the value it set until the next input: `set:x=120` moves the
finger, `raise` leaves gravity along +X across the firmware's 8 Hz samples.

```sh
# one command per line, one answer per line
python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 5556))
f = s.makefile("rwb")
for line in ["part list", "part touch input tap", "part touch input double",
             "part touch state", "part imu input raise", "part imu state"]:
    f.write((line + "\n").encode()); f.flush()
    print(line, "->", f.readline().decode().strip())
PY
```

**Why `chips` exists.** esp-emu 1.0.0-rc2 answers an I2C port only through the
parts wired to it: on a port that has a part, its `regfile` devices are not
asked (PARTS.md: "Parts win"). The touch and IMU need port 0, so the PMU, RTC
and codec would otherwise be silent and the firmware would boot without a
battery, a clock or audio. `chips` supplies exactly the registers `board.json`
preloads, and `board-interactive.json` keeps the `devices` entries as the
description of the real board. On a future esp-emu that answers devices per
address, remove the `chips` part and the `devices` models take over.

Two timing notes learned while making this work:

* the firmware enables the touch wake source only while the panel is off,
  because `gpio_wakeup_enable()` reprograms GPIO 38 to **level-low**. On wake
  it disables the source and restores the falling edge, so the LVGL touch ISR
  is a normal edge interrupt while the face is on. `smoke.sh` still lets the
  face settle for 3 s of emulated time and retries the double tap; a person
  would simply tap again.
* the emulator's TAP network backend made emulated time advance at roughly a
  third of wall time here. Smoke runs with `--net user,restrict=yes`, which is
  close to real time and hermetic.

## One-time setup (WSL or Linux)

```sh
# 1. emulator 1.0.0-rc2 (0.51.0 does not have virtual parts yet)
curl -fsSL \
  https://github.com/espressif/esp-emulator/releases/download/v1.0.0-rc2/esp-emu-1.0.0-rc2-x86_64-unknown-linux-gnu.tar.gz \
  | tar -xz -C /tmp
cp /tmp/esp-emu-1.0.0-rc2-x86_64-unknown-linux-gnu/esp-emu ~/.local/bin/esp-emu

# 2. the part SDK, cloned next to this repository
git clone --depth 1 --branch v1.0.0-rc2 \
  https://github.com/espressif/esp-emulator ../esp-emulator

# 3. Rust for the virtual parts
rustup target add wasm32-unknown-unknown
```

Every part's `Cargo.toml` points at `../../../../esp-emulator/esp-part`, which
is where step 2 puts it when the repository is `~/src/MosuTime` and the clone
is `~/src/esp-emulator`.

## Build and capture

```sh
. /path/to/esp-idf/export.sh

# emulator build: no SD, no light sleep
idf.py -B build-emu \
  -D SDKCONFIG=build-emu/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;tools/sdkconfig.emulator" \
  build merge-bin

# parts (panel, touch, imu, chips); run from the repository root
EMU_PROTO="$(cd ../esp-emulator && pwd)/proto"
for part in panel touch imu chips; do
  (cd "tools/emu/$part" && ESP_COSIM_PROTO_DIR="$EMU_PROTO" \
     cargo build --release --target wasm32-unknown-unknown)
done

tools/emu/capture.sh            # runs the four capture cases, writes docs/emulator/*.png
tools/emu/smoke.sh              # drives touch and IMU against the firmware
```

`smoke.sh` accepts `ESP_EMU`; without it, it picks the first `esp-emu` on
`PATH` or in `~/.local/bin` that advertises `--cosim-board` (the rc2 build),
so a stale 0.51.0 is skipped. The firmware is `FW` (default
`build-emu/merged-binary.bin`). `smoke.sh` builds a part whose `.wasm` is
missing or older than its source, using `ESP_COSIM_PROTO_DIR` if you set it.
`capture.sh` takes `ESP_EMU` and `FW` the same way, but defaults to `esp-emu`.

## What `smoke.sh` proves

It runs two real firmware sessions against `board-interactive.json` and fails
on the first thing that does not reach the application:

1. **Touch.** Boots with `--control-tcp 127.0.0.1:5556 --timeout 20s`, waits
   for `watch face ready`, settles for 3 s of emulated time, sends
   `part touch input tap`, then `part touch input double` (retrying the double
   until the phrase builder answers). It asserts that:
   * the FT3168 part pulled GPIO 38 low and released it, in the
     `--cosim-log` `GpioDrive` stream;
   * the UART log contains the phrase builder's missing-clip lines for the
     hour, minute and day — `h_9.wav`, `m_41.wav`, `d_1.wav` at 09:41 on a
     Tuesday:
     ```
     W (…) mosu_audio: missing clip: /sdcard/voice/h_9.wav
     W (…) mosu_audio: missing clip: /sdcard/voice/m_41.wav
     W (…) mosu_audio: missing clip: /sdcard/voice/d_1.wav
     ```
     That is proof the tap went through the FT5x06 driver, LVGL's input path,
     `mosu_tap`, `speak_now` and `mosu_phrase` — not just that the part saw a
     finger;
   * no panic, assert or `ESP_ERROR_CHECK` failure appears.

2. **IMU wake.** Boots with the control channel on `5557` and waits for the
   face to turn itself off (`screen_on_ms`, 15 s of emulated time), then sends
   `part imu input raise`. It asserts that:
   * the panel received `0x28` (display off) and then no transfers at all
     while the face slept;
   * the panel received `0x29` (display on) after the raise — the wrist-raise
     detector fired, `screen_on(false)` ran and the panel was woken;
   * no panic appears.

   The firmware now also logs `mosu: screen on (motion)` at info level, so the
   wake is visible in the UART log as well as in the QSPI stream.

Run it with:

```sh
tools/emu/smoke.sh
# or against another build/emulator:
ESP_EMU=/path/to/esp-emu FW=/path/to/merged-binary.bin tools/emu/smoke.sh
```

It leaves `smoke-touch.log`, `smoke-touch.jsonl`, `smoke-imu.log` and
`smoke-imu.jsonl` next to this README (all ignored by git).

## How the picture is recovered

`--cosim-log` records every SPI transfer that reaches the panel part. A QSPI
panel command is one four byte data transfer (`32 00 2c 00` for "write
colour"), with the pixels in the following transfers. `reconstruct.py`:

1. tracks CASET (`0x2a`) and RASET (`0x2b`) to know the window;
2. collects the big-endian RGB565 stream of the following `0x2c` transfers;
3. writes a 410 x 502 PNG, subtracting the panel's 22 pixel x offset.

The implementation follows the CO5300/SH8601 opcode layout used by
`esp_lcd_sh8601`: `0x02` is a command, `0x32` is a colour write, and
`LCD_OPCODE_WRITE_COLOR << 24 | cmd << 8` is byte-reversed by the SPI panel
IO, so the bytes on the wire are `32 00 <cmd> 00`.

## Useful checks

```sh
# Boot log only
esp-emu --chip esp32s3 --firmware build-emu/merged-binary.bin \
  --psram-size 8M --console uart0 --timeout 6s \
  --cosim-board tools/emu/board.json | tee boot.log

# The phrase builder at 09:41 on a Tuesday, without an SD card
esp-emu ... --inject-on "console ready" --inject "say\n"
#   W mosu_audio: missing clip: /sdcard/voice/h_9.wav
#   W mosu_audio: missing clip: /sdcard/voice/m_41.wav
#   W mosu_audio: missing clip: /sdcard/voice/d_1.wav

# The hardware screenshot path without an SD card: /dev/null still exercises
# the LVGL snapshot and the BMP row conversion.
esp-emu ... --inject-on "console ready" \
  --inject "display\nscreenshot /dev/null\n"
#   display: 410x502, on, panel brightness 55%, configured 55%
#   buffer: 20500 px, double 1, dma 1, spiram 0
#   panel: SH8601/CO5300, touch: FT3168
#   I mosu_ui: snapshot written to /dev/null (410x502)
#   screenshot /dev/null: ESP_OK

# Interactive board, by hand: 20 s of emulated time, control channel on 5556
esp-emu --chip esp32s3 --firmware build-emu/merged-binary.bin \
  --psram-size 8M --console uart0 --timeout 20s \
  --net user,restrict=yes \
  --cosim-board tools/emu/board-interactive.json \
  --control-tcp 127.0.0.1:5556 \
  --cosim-log emu-touch.jsonl --cosim-log-filter BusRequest,GpioDrive
# then, from another terminal:
#   part list
#   part touch input double
#   part touch state
#   part imu input raise
# and look for the missing-clip lines in the UART output.
```

The `regfile` devices answer with the values in `board.json`:

* `0x34` AXP2101: status `0x28` (battery present, VBUS good), 3.9 V battery;
* `0x51` PCF85063: 2026-11-10 07:41 UTC, which is 09:41 in Bucharest;
* `0x6A` QMI8658: `WHO_AM_I = 0x05`;
* `0x38` FT3168 and `0x18` ES8311: an ACK and zeroes are enough for init.

On `board-interactive.json` the same three register files are served by the
`chips` part, for the rc2 reason described above; the touch and IMU values are
served by their real models instead.

`board-unknown.json` is the same board with an invalid RTC year, which shows
the `--` placeholder.
