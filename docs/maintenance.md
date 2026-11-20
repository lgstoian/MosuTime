# Maintenance

Everything the watch needs at runtime lives in two places: the firmware in
flash (updated over USB) and the microSD card (`mosu.cfg` plus the voice
clips). The config and the clips are not compiled in: the brightness, colours,
network and wording can be changed on the card, and the face drawing code
stays in the firmware.

This page is for the person maintaining the watch, not for the wearer. The
wearer-facing guide is [`docs/family-ro.md`](family-ro.md).

## 1. Edit `mosu.cfg`

The template is [`sd_card/mosu.cfg`](../sd_card/mosu.cfg). The copy the watch
reads is `mosu.cfg` in the **root** of the SD card, next to the `voice/`
folder:

```
/
├── mosu.cfg
└── voice/
    ├── h_1.wav ... h_12.wav
    ├── m_1.wav ... m_59.wav
    └── d_0.wav ... d_6.wav
```

Practical rules, all enforced by `components/mosu_config`:

* `#` starts a comment at the beginning of a line, or after a space. A colour
  such as `#FFFFFF` is not a comment. `;` works the same way.
* Quotes around text values are optional (`name = "MosuTime"` and
  `name = MosuTime` are the same).
* An invalid line is reported on the console and skipped. The watch keeps the
  previous value for that key and boots normally. Numeric values outside their
  allowed range are clamped, not rejected.
* Save the file as ASCII or UTF-8. The parser skips a leading UTF-8 BOM, but
  it cannot read UTF-16; Notepad's plain "UTF-8" is fine.
* The file is read once at boot. After editing, reinsert the card and
  `reboot` (or press reset). `sd remount` only remounts the card; it does not
  re-parse `mosu.cfg`. `config` shows the active values, not the file on the
  card.
* A successfully parsed card config is cached in NVS. If the card is later
  missing, the watch uses that cache. If it has never seen a card, it uses the
  built-in defaults.

### Keys worth knowing

| Key | Range | Default | Effect |
| --- | --- | --- | --- |
| `device.name` | text | `MosuTime` | Console banner only. |
| `device.log_level` | `none`…`verbose` | `info` | Console noise; `debug`/`verbose` can cost power and should not ship. |
| `display.brightness` | 1…100 | 55 | Panel brightness. Applied at every screen-on. |
| `display.screen_on_ms` | 1000…300000 | 15000 | How long the face stays on after a wake. The main runtime lever. |
| `display.wake_on_touch` | bool | true | Touch INT wakes the CPU. Leave on. |
| `display.wake_on_motion` | bool | true | Wrist raise. Turning it off saves the IMU poll but removes the main way to see the time. |
| `display.motion_angle_deg` | 10…80 | 35 | Wrist-raise angle. Lower is more sensitive and causes more false wakes. |
| `display.motion_refractory_ms` | 300…20000 | 2500 | Minimum gap between motion wakes. |
| `display.motion_poll_hz` | 1…20 | 8 | IMU reads per second while the screen is off. 4–8 is the intended range. |
| `display.pixel_shift_sec` | 0…3600 | 180 | Burn-in guard interval; 0 disables it. |
| `display.tap_feedback_ms` | 0…2000 | 250 | Border flash after a double tap; 0 disables it. |
| `clock.timezone` | IANA or POSIX | `Europe/Bucharest` | Used when `auto_timezone` is false or the lookup fails. |
| `clock.auto_timezone` | bool | true | Ask the network for the zone by IP. An unknown name falls back to the returned UTC offset. |
| `clock.geo_url` | URL | `http://ip-api.com/line/?fields=timezone,offset` | Timezone service. Plain HTTP is cheaper; an explicit `https://` URL is still supported. |
| `clock.sync_hours` | 1…168 | 6 | Hours between successful syncs. The first three failures retry after 5, 15 and 30 minutes. |
| `clock.sync_per_day` | 0…24 | 0 | When 1…24, overrides `sync_hours` and syncs that many times a day (5 = every 4.8 h). 0 means "use `sync_hours`". |
| `clock.wifi_always_on` | bool | false | `true` keeps the radio on and costs much more. Leave it false. |
| `clock.manual_time` | `"YYYY-MM-DD HH:MM"` | empty | Fallback only when there is no valid RTC and no network. |
| `wifi.networkN_ssid` / `_pass` | text, N=1…8 | — | Tried in order, 15 s each. 2.4 GHz networks only. |
| `announce.enabled` | bool | true | Double-tap announcement. |
| `announce.volume` | 0…100 | 85 | Voice volume. |
| `announce.double_tap_ms` | 200…1500 | 650 | Maximum gap between the two taps. |
| `announce.max_hold_ms` | 80…1500 | 400 | A longer press is not a tap. |
| `announce.max_move_px` | 4…200 | 40 | A finger that slides further is not a tap. |
| `announce.cooldown_ms` | 300…10000 | 2000 | Ignore new announcements just after one. |
| `announce.voice_dir` | text | `/sdcard/voice` | Where the clips are. Keep it unless you move the folder. |
| `announce.clip_gap_ms` | 0…1000 | 60 | Silence between clips in a sentence. |

`status` prints the battery, network, audio and display state; `time` prints
the local time and zone. Both exist today and are the quickest way to check a
config change.

## 2. Change brightness or volume at runtime

```text
bright 40
vol 70
```

Both are runtime-only and are lost on reboot. Edit `mosu.cfg` to make them
permanent. Brightness is panel brightness, not a backlight: 1 is very dim but
not off, and 100 is full. Volume 0 is silent but the double tap still flashes
the border.

After a change, run `say` to check the new volume and look at the face at the
new brightness. Do not judge brightness under a desk lamp; the AMOLED looks
different in a dark room.

## 3. Change the Wi-Fi network

Edit the `[wifi]` section:

```ini
[wifi]
network1_ssid = "Acasa"
network1_pass = "parola"

network2_ssid = "Bunica"
network2_pass = "alta parola"
```

Up to eight networks are supported (`network1` … `network8`). They are tried
in order, 15 s per network, then the radio is switched off. The ESP32-S3 is
2.4 GHz only; a 5 GHz-only SSID will never connect. WPA2 and open networks are
accepted; WPA3 has not been tested on this build.

If the card slot is not reachable, the console can provision one network
without a card (it is saved to NVS; the card config still wins on the next
boot with a card):

```text
wifi camera parola
wifi status
wifi scan
wifi presence
wifi clear
```

`wifi status` prints SSIDs only, never passwords. `wifi scan` lists the
visible APs with channel, RSSI and auth mode. `wifi presence` checks whether a
configured SSID is in range without associating.

After a reboot, `sync` forces an immediate attempt:

```text
sync
```

Watch the console for the join, `network timezone: ...` and `clock synced`.
The Wi-Fi icon is grey before the first success, green after a success, and
red with a slash after a failure. A failure does not stop the clock; it keeps
running from the PCF85063 and retries after 5, 15 and 30 minutes before the
normal interval resumes. If no configured access point is visible, a
15-minute presence scan notices when it comes back and triggers a sync; a
wrong password keeps the slower backoff. No Wi-Fi failure resets or panics the
watch.

If the watch is used in a different country, leave `auto_timezone = true` so
the IP lookup can set the zone. If you prefer a fixed zone, set
`auto_timezone = false` and put an IANA name (for example `Europe/Bucharest`)
in `timezone`.

## 4. Regenerate the voice

The clips are generated on a PC with a local ONNX voice and copied to the card;
nothing is embedded in the firmware. The text and the grammar live in
[`tools/gen_voice.py`](../tools/gen_voice.py), the Romanian pronunciation in
[`tools/mosu_g2p.py`](../tools/mosu_g2p.py).

```sh
pip install onnxruntime
python tools/gen_voice.py --dry-run          # show the 78 clips and phonemes
python tools/gen_voice.py --out sd_card/voice
```

Useful flags:

* `--force` regenerates clips that already exist. Without it, existing files
  are skipped, which is how you keep a partly hand-edited set.
* `--models` changes where the `ro_RO-mihai-medium` voice is cached
  (default `tools/piper-models`).
* `--preview` also writes `preview.wav` with four complete announcements, so
  the result can be checked on a PC. The watch never plays that file.

The script downloads the free Romanian `ro_RO-mihai-medium` voice on first
run, renders 12 hour phrases, 59 minute phrases and 7 weekday phrases, and
writes them as mono 16-bit WAV at 22050 Hz. The player accepts 8–48 kHz mono
16-bit PCM; anything else is reported as `need mono 16 bit wav` and skipped.
`check_voice.py` is the tool that validates a folder.

To change the wording, edit `HOURS`, `minute_text()` or `DAYS` in
`tools/gen_voice.py` and regenerate. Keep the file names identical
(`h_1.wav` … `d_6.wav`); the firmware builds the path from the time. A new word
also needs an entry in `LEXICON` in `tools/mosu_g2p.py`, otherwise the render
stops with the missing word instead of guessing its pronunciation.

Copy the whole `voice/` folder to the card, overwriting the old one. With the
card in the watch, that is one command:

```sh
python tools/push_sd.py COM11 --no-voice              # config only
python tools/push_sd.py COM11                         # config + voice
```

The tool signs off with `PUSH PASS`; `ls /sdcard/voice` on the console then
lists 79 entries. A missing clip is not fatal: that fragment is silent and the
rest of the sentence still plays.

`python tools/check_voice.py sd_card/voice` compares each clip's duration with
the phrase it is supposed to say. A clip that is much longer than its own text
is speaking extra words (a watermark or a stale file from another tool) and
fails the check; run it before every copy to the card.

## 5. Update the firmware over USB

The partition table reserves two OTA slots, but the firmware has no OTA
client yet, so updating today means USB and `idf.py flash`, which is also the
recovery path if an experiment goes wrong.

```sh
idf.py build
idf.py -p COM11 flash monitor
```

Windows wrapper:

```powershell
.\tools\build.ps1 -Port COM11 -Flash -Monitor
```

The Windows app does the same thing without a terminal: `tools/mosu-gui.cmd`,
tab **Firmware**, **Build and flash**. It closes the console port first so
esptool can take it, streams the build output and reconnects afterwards. Its
**Settings** tab edits `sd_card/mosu.cfg` with the same ranges as the parser
and pushes the file; **SD card** pushes the voice clips and can format the
card.

The board's USB-C is the ESP32-S3 native USB, and the firmware console is
USB Serial/JTAG, so the same COM port carries flashing, logs and console
commands. UART0 is only on the reserved pads.

The build uses ESP-IDF 5.5.x and the 32 MB partition table in
`partitions.csv`. Flashing writes the bootloader, the partition table and the
application; NVS is preserved, so the cached config and the last timezone
survive. Only use `idf.py erase-flash` when you deliberately want to wipe NVS
(and then put the card back in so the config is reloaded).

After an update:

1. watch the boot log against [`docs/bringup.md`](bringup.md);
2. run `status`, `time`, `say`;
3. double-tap the glass and confirm the phrase;
4. check that the screen still goes off after `screen_on_ms`;
5. if the update touched power management, repeat the idle measurement in
   [`docs/power.md`](power.md).

## 6. Replace the SD card

The card is optional at boot, so a dead card is not an emergency: the watch
uses the NVS-cached config and stays silent.

1. Unplug USB. If the enclosure has a power switch, switch it off. Do not open
   the watch; if the card slot is not reachable from outside, stop and ask.
2. Eject the old card. If you can still read it, copy `mosu.cfg` and the
   `voice/` folder somewhere safe first.
3. Insert the new card in the watch (it does not need to be formatted) and
   upload everything over USB:

   ```sh
   python tools/push_sd.py COM11 --format
   ```

   `--format` runs `sd format yes` on the watch, which writes a fresh FAT32
   volume (MBR, 32 KB clusters) and mounts it. The tool then uploads
   `mosu.cfg` and the 78 voice clips with a CRC-32 per file. Without
   `--format`, the card must already be mounted and the tool only uploads.
4. `reboot`, then run:

   ```text
   status
   config
   say
   ls /sdcard/voice
   ```

   `status` must say `SD mounted`; `config` must show the values you put on
   the card (SSIDs are printed, passwords are not); `say` must speak; `ls`
   must list 79 entries in `voice/` (78 clips plus `preview.wav`). If the
   config reports errors, fix the file, push it again and `reboot`.

The mount is explicit: it happens at boot, or when you run `sd remount`. There
is no card-detect switch, so inserting a card while the watch is running does
nothing until one of those happens. Editing files while the watch is running
can corrupt FAT, so remove the card only when the watch is off.

A card formatted by a PC can fail with `failed to mount card (13)` even though
Windows opens it. That is FatFs rejecting an incomplete FAT32 table, not a
wiring fault: let the watch format it (`sd format yes`) and push again.

If the watch keeps using an old configuration, erase the NVS cache with
`nvs erase` (or **Factory reset (NVS)** in the app) and reboot; it reloads
`mosu.cfg` from the card, or the built-in defaults when there is no card.

## 7. Turn the watch off

The watch is meant to stay on. There are two ways to cut power:

* **Hold the side PWR button for 10 seconds.** The AXP2101 does this itself,
  so it works even if the firmware is hung. A short press, or a press that
  slips, does nothing; this is deliberate, because the wearer must not switch
  it off by accident. Press PWR briefly to power it on again.
* **`poweroff` on the console** saves the clock to the RTC, turns the panel
  off and asks the PMU to remove the rails. It works on USB as well as on the
  cell (verified on this board), which makes it the safe maintenance shutdown.

The PWRKEY off-time is set to 10 s at every boot (`AXP2101` register `0x27`,
bits 3:2 = `11`; `pmu` prints it). Do not lower it for a wearer with dementia:
4 s or 6 s is reachable by a fumbling press.

## 8. Troubleshooting

| Symptom | Check |
| --- | --- |
| `config: invalid number for brightness` | The value is not a number. Ranges are clamped, text is an error. Fix that line. |
| Config changes do nothing | File must be `mosu.cfg` in the card root, card mounted, and the watch rebooted. |
| `--` instead of the time | No valid RTC and no sync yet. `time` shows validity; `sync` forces an attempt; `settime YYYY-MM-DD HH:MM` sets it manually. |
| Wi-Fi icon red with a slash | Wrong SSID/password, 5 GHz-only network, or out of range. `sync` retries; the clock keeps running. If the AP was simply not visible, a 15-minute presence scan triggers a sync when it returns. |
| No console input over USB | The firmware must use USB Serial/JTAG as the primary console; the secondary port is output-only. Check `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and `wifi`/`help` after flashing. |
| Watch stays in download (`boot:0x23`) | Release BOOT and power-cycle (long-press PWR ~6 s, then click PWR), or unplug/replug USB without touching BOOT. |
| Status icons clipped by the glass | The AMOLED has rounded corners; keep `MOSU_SCREEN_MARGIN` at 48 px and `MOSU_STATUS_Y` at 30 px. |
| Voice is silent | `voice/` missing, wrong file names, not mono 16-bit, or `volume = 0`. Look for `missing clip:` or `need mono 16 bit wav:` in the log. |
| `failed to mount card (13)` | A PC-formatted FAT32 table is incomplete. Run `sd format yes` and push again with `tools/push_sd.py`. |
| `PUSH ... no ACK` or `input stalled` | The 4 KB chunk did not arrive intact; `push_sd.py` restarts that file automatically. If it repeats, run `rx bench 262144` and check the reported rate (400 KiB/s or more is healthy on USB Serial/JTAG). |
| Battery percentage jumps | The percentage comes from battery voltage, not a coulomb counter. It moves in steps and is less accurate under load. Use a PPK2 for energy measurements. |
| Watch will not charge | Cable, charger, battery connector. `status`/`pmu` show `charging` and `vbus`; a PMU that does not answer is a hardware fault. |
| Watch will not turn off | Hold PWR for a full 10 s; a short press is ignored by design. `pmu` must show `0x27 = 0x1C`. If the rails are still up on USB, use `poweroff`. |
| Screen stays black | Touch it once; the face is off by default. If it stays black, check the boot log for `display init failed`. |
| Random resets | Check the log for `brownout`; suspect the battery, the connector or the charger. Do not ship it. |

When in doubt, change one thing at a time, reboot, and read the boot log. The
console is the source of truth, not the picture on the screen.
