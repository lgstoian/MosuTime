# Third-party notices

MosuTime itself is MIT licensed (see `LICENSE`). It builds on the following
components, which keep their own licences.

## Firmware dependencies

| Component | Version | Licence |
| --- | --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | 5.5.5 | Apache-2.0 |
| [LVGL](https://github.com/lvgl/lvgl) | 9.6.0 | MIT |
| [esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port) | 2.9.0 | Apache-2.0 |
| [esp_lcd_sh8601 (Waveshare)](https://components.espressif.com/components/waveshare/esp_lcd_sh8601) | 2.0.0 | Apache-2.0 |
| [esp_lcd_touch](https://components.espressif.com/components/espressif/esp_lcd_touch) | 1.2.1 | Apache-2.0 |
| [esp_lcd_touch_ft5x06](https://components.espressif.com/components/espressif/esp_lcd_touch_ft5x06) | 1.1.1 | Apache-2.0 |
| [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) | 1.5.x | Apache-2.0 |
| [esp_lcd_panel_io_additions](https://components.espressif.com/components/espressif/esp_lcd_panel_io_additions) | 1.x | Apache-2.0 |
| [esp_io_expander](https://components.espressif.com/components/espressif/esp_io_expander) | 1.x | Apache-2.0 |
| [cmake_utilities](https://components.espressif.com/components/espressif/cmake_utilities) | 0.x | Apache-2.0 |

The exact resolved versions are pinned in `main/idf_component.yml` and recorded
by the component manager in `dependencies.lock` when the project is configured.

## Voice

The announcement clips are generated locally by `tools/gen_voice.py`, which runs
the
[`ro_RO-mihai-medium`](https://huggingface.co/rhasspy/piper-voices/tree/main/ro/ro_RO/mihai/medium)
voice from `rhasspy/piper-voices` through
[onnxruntime](https://github.com/microsoft/onnxruntime).

| Component | Licence | Used at |
| --- | --- | --- |
| `onnxruntime` | MIT | render time, on the developer's machine |
| `ro_RO-mihai-medium` ONNX voice | model card records a CC0 dataset | render time; the weights are downloaded, never committed |
| `tools/mosu_g2p.py` | MIT (part of MosuTime) | render time |
| Generated WAV files | not committed; see the voice's `MODEL_CARD` before redistributing | copied to the SD card |

The voice is not phonemised by espeak-ng: `tools/mosu_g2p.py` carries the
Romanian pronunciation for the 78 phrases as a plain lexicon, so the render
path contains no GPL code and no GPL process. The earlier `piper-tts`
(`piper1-gpl`, GPL-3.0-or-later) dependency has been removed for this reason.

The generated WAV files are not part of this repository. They are created on
the user's machine, which downloads the model from Hugging Face at that time.
The `MODEL_CARD` in the voice directory is the authority on the dataset; it
records CC0 for `ro_RO-mihai-medium`.

If you replace the voice, keep the licence of the new model with the copy of
MosuTime you distribute.

## Hardware references

The panel initialisation sequence in
`components/mosu_board/src/mosu_board_display.c` follows the public
Waveshare BSP for the ESP32-S3-Touch-AMOLED-2.06 (Apache-2.0). The AXP2101,
PCF85063 and QMI8658 register access is written against the public datasheets;
no vendor driver code is linked.

## Fonts and images

There are none. The digits are drawn as line segments in `mosu_digits.c`, and
the battery and Wi-Fi icons are drawn with LVGL primitives. No font file or
image asset is embedded in the firmware.
