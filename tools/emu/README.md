# Running MosuTime in esp-emu

The watch hardware is a CO5300 AMOLED, an FT3168 touch controller, an AXP2101
PMU, a PCF85063 RTC and a QMI8658 IMU. [esp-emu](https://github.com/espressif/esp-emulator)
emulates the ESP32-S3 and its buses, and lets a host model answer on them. This
folder turns that into a real firmware test and a screenshot source.

What it gives us:

* the actual merged firmware boots (bootloader, ESP-IDF, LVGL, our `app_main`);
* the panel receives the real QSPI command and pixel stream;
* the three watch-face states can be captured as PNGs without a camera;
* the console can be driven (`settime`, `say`) and the result checked.

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
| AXP2101, PCF85063, QMI8658, FT3168, ES8311 I2C | `regfile` devices in `board.json` |
| SD card | not modelled; the emulator build has `CONFIG_MOSU_SKIP_SD=y` |
| Light sleep on S3 | not modelled; the emulator build disables PM |
| Touch gestures, audio | not modelled (the console still exercises the phrase builder) |

The production build is untouched: `tools/sdkconfig.emulator` and
`CONFIG_MOSU_SKIP_SD` only apply to the `build-emu` directory.

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

# 3. Rust for the panel part
rustup target add wasm32-unknown-unknown
```

`tools/emu/panel/Cargo.toml` points at `../../../../esp-emulator/esp-part`,
which is where step 2 puts it when the repository is `~/src/MosuTime` and the
clone is `~/src/esp-emulator`.

## Build and capture

```sh
. /path/to/esp-idf/export.sh

# emulator build: no SD, no light sleep
idf.py -B build-emu \
  -D SDKCONFIG=build-emu/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;tools/sdkconfig.emulator" \
  build merge-bin

cd tools/emu/panel
ESP_COSIM_PROTO_DIR=../../../esp-emulator/proto \
  cargo build --release --target wasm32-unknown-unknown
cd ../../..

tools/emu/capture.sh            # runs the three cases, writes docs/emulator/*.png
```

`capture.sh` also accepts `ESP_EMU` (default `esp-emu`) and `FW` (default
`build-emu/merged-binary.bin`).

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
```

The `regfile` devices answer with the values in `board.json`:

* `0x34` AXP2101: status `0x28` (battery present, VBUS good), 3.9 V battery;
* `0x51` PCF85063: 2026-11-10 07:41 UTC, which is 09:41 in Bucharest;
* `0x6A` QMI8658: `WHO_AM_I = 0x05`;
* `0x38` FT3168 and `0x18` ES8311: an ACK and zeroes are enough for init.

`board-unknown.json` is the same board with an invalid RTC year, which shows
the `--` placeholder.
