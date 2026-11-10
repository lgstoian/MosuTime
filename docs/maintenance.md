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
| `clock.auto_timezone` | bool | true | Ask the network for the zone by IP. Unknown zones are ignored. |
| `clock.sync_hours` | 1…168 | 6 | Hours between Wi-Fi syncs. |
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

After a reboot, `sync` forces an immediate attempt:

```text
sync
```

Watch the console for the join, `network timezone: ...` and `clock synced`.
The Wi-Fi icon is grey before the first success, green after a success, and
red with a slash after a failure. A failure does not stop the clock; it keeps
running from the PCF85063 and retries after `sync_hours`.

If the watch is used in a different country, leave `auto_timezone = true` so
the IP lookup can set the zone. If you prefer a fixed zone, set
`auto_timezone = false` and put an IANA name (for example `Europe/Bucharest`)
in `timezone`.

## 4. Regenerate the voice

The clips are generated on a PC with Piper and copied to the card; nothing is
embedded in the firmware. The text and the grammar live in
[`tools/gen_voice.py`](../tools/gen_voice.py).

```sh
pip install piper-tts
python tools/gen_voice.py --dry-run          # show the 78 clips and the text
python tools/gen_voice.py --out sd_card/voice
```

Useful flags:

* `--force` regenerates clips that already exist. Without it, existing files
  are skipped, which is how you keep a partly hand-edited set.
* `--piper C:\path\to\piper.exe` uses a standalone Piper instead of the Python
  package.
* `--models` changes where the `ro_RO-mihai-medium` model is cached
  (default `tools/piper-models`).
* `--preview` also writes `preview.wav` with four complete announcements, so
  the result can be checked on a PC. The watch never plays that file.

The script downloads the free Romanian `ro_RO-mihai-medium` voice on first
run, renders 12 hour phrases, 59 minute phrases and 7 weekday phrases, and
checks that every file is mono 16-bit WAV. The player accepts 8–48 kHz mono
16-bit PCM; anything else is reported as `need mono 16 bit wav` and skipped.

To change the wording, edit `HOURS`, `minute_text()` or `DAYS` in
`tools/gen_voice.py` and regenerate. Keep the file names identical
(`h_1.wav` … `d_6.wav`); the firmware builds the path from the time.

Copy the whole `voice/` folder to the card, overwriting the old one. A missing
clip is not fatal: that fragment is silent and the rest of the sentence still
plays.

## 5. Update the firmware over USB

The partition table reserves two OTA slots, but the firmware has no OTA
client yet, so updating today means USB and `idf.py flash`, which is also the
recovery path if an experiment goes wrong.

```sh
idf.py build
idf.py -p COM5 flash monitor
```

Windows wrapper:

```powershell
.\tools\build.ps1 -Port COM5 -Flash -Monitor
```

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
3. Format the new card as FAT32 with 16 KB or 32 KB clusters. Any normal card
   works; the firmware mounts it 1-bit at 20 MHz and reads only tiny files.
4. Copy `mosu.cfg` to the root and the `voice/` folder beside it. Keep the
   names exactly as they are.
5. Insert the card, apply power, and run:

   ```text
   status
   config
   say
   ```

   `status` must say `SD mounted`; `config` must show the values you put on
   the card (SSIDs are printed, passwords are not); `say` must speak. `config`
   reads the in-memory copy, so it only proves the new card after a boot:
   reinsert the card, `reboot`, and watch the boot log's config line. If it
   reports errors, fix the file and reboot again.

The mount is explicit: it happens at boot, or when you run `sd remount`. There
is no card-detect switch, so inserting a card while the watch is running does
nothing until one of those happens. Editing files while the watch is running
can corrupt FAT, so remove the card only when the watch is off.

## 7. Troubleshooting

| Symptom | Check |
| --- | --- |
| `config: invalid number for brightness` | The value is not a number. Ranges are clamped, text is an error. Fix that line. |
| Config changes do nothing | File must be `mosu.cfg` in the card root, card mounted, and the watch rebooted. |
| `--` instead of the time | No valid RTC and no sync yet. `time` shows validity; `sync` forces an attempt; `settime YYYY-MM-DD HH:MM` sets it manually. |
| Wi-Fi icon red with a slash | Wrong SSID/password, 5 GHz-only network, or out of range. `sync` retries; the clock keeps running. |
| Voice is silent | `voice/` missing, wrong file names, not mono 16-bit, or `volume = 0`. Look for `missing clip:` or `need mono 16 bit wav:` in the log. |
| Battery percentage jumps | The percentage comes from battery voltage, not a coulomb counter. It moves in steps and is less accurate under load. Use a PPK2 for energy measurements. |
| Watch will not charge | Cable, charger, battery connector. `status`/`pmu` show `charging` and `vbus`; a PMU that does not answer is a hardware fault. |
| Screen stays black | Touch it once; the face is off by default. If it stays black, check the boot log for `display init failed`. |
| Random resets | Check the log for `brownout`; suspect the battery, the connector or the charger. Do not ship it. |

When in doubt, change one thing at a time, reboot, and read the boot log. The
console is the source of truth, not the picture on the screen.
