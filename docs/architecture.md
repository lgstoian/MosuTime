# Architecture

MosuTime is a small firmware with strict layering. The interesting design is
not in the amount of code, it is in what is *not* running.

## Layers

```
              main/app_main.c  (power state machine, wiring)
                     |
   +---------+---------+---------+---------+---------+
   |         |         |         |         |         |
 mosu_ui  mosu_audio mosu_net mosu_time mosu_diag |
   |         |         |         |         |         |
   +---------+---------+----+----+---------+---------+
                            |
                        mosu_board   (the only board-specific component)
                            |
                        ESP-IDF drivers, LVGL, esp_lcd, esp_codec_dev

 mosu_config (INI + NVS)      mosu_core (pure C, no ESP-IDF)
```

`mosu_core` is compiled three times: into the firmware, into the host test
binary and into the desktop simulator. It contains no ESP-IDF header and no
global state, so the three builds cannot drift apart in behaviour.

`mosu_config` also stays portable except for `mosu_config_store.c`, which owns
the NVS cache.

## Boot

1. `nvs_flash_init`, erasing on a version change.
2. `mosu_board_init`: I2C bus, AXP2101, PCF85063, QMI8658. A missing PMU or
   IMU is logged but not fatal - the watch still shows the time and touch
   still wakes it.
3. Mount the SD card and parse `mosu.cfg`. If the card is absent or the file
   is unreadable, load the last good config from NVS; if that is absent too,
   use the built-in defaults. A config loaded from the card is written back to
   NVS so the next boot without a card is identical.
4. Apply the log level, configure power management (160/80 MHz, light sleep).
5. Start the panel, touch and LVGL; create the watch face.
6. `mosu_time_init`: set the timezone, restore the clock from the RTC, apply
   `manual_time` if there is no valid RTC.
7. Initialise the codec once and park it (I2S disabled, codec closed).
8. Start the announcement task, the touch interrupt, the network task and the
   console.
9. Show the face for one `screen_on_ms` window, then behave normally.

## The screen state machine

`app_main` owns two states.

**Screen off** is the default:

```
wait for touch interrupt | IMU poll timeout
  touch line low            -> screen_on(touch)
  IMU sample -> wrist raise -> screen_on(motion)
```

The task sleeps on the touch binary semaphore with a timeout of
`1000 / motion_poll_hz` milliseconds. FreeRTOS tickless idle turns those waits
into light sleep, and the touch INT line is both a GPIO wake source and an ISR,
so a finger is noticed without polling the panel.

**Screen on**:

```
every 100 ms:
  refresh the displayed minute
  handle a pending "say" request
  battery every 60 s        (smoothed, 4-sample average)
  Wi-Fi icon every 2 s
  if no activity for screen_on_ms -> screen_off()
```

A touch edge resets the activity timer and feeds `mosu_tap`. Two short taps
inside `double_tap_ms` produce one announcement. The screen is never switched
off while audio is playing or while the glass is being touched.

`screen_on` restores the configured brightness (switching an AMOLED panel off
sets brightness to zero), takes the `NO_LIGHT_SLEEP` lock, forces a full
repaint and resumes LVGL. `screen_off` stops the LVGL task first, then switches
the panel off, then releases the lock, so no transfer is ever cut in half.

## Time

Everything inside the firmware is UTC. The timezone only changes what is
displayed and spoken.

* The PCF85063 runs from its own oscillator and keeps UTC across battery
  changes.
* SNTP is the authoritative source. `mosu_time_set_epoch` writes the system
  clock *and* the RTC, so the next boot without Wi-Fi is correct.
* `mosu_net` asks a geolocation service for the IANA zone (plain text from
  `ipapi.co/timezone/`, or JSON with a `timezone` key from `ip-api.com`).
  `mosu_time` maps the common IANA names to POSIX TZ strings because newlib
  does not read tzdata. Unknown zones are ignored rather than guessed.
* The chosen zone is stored in NVS as well as in the config, so a network zone
  survives a reboot even when the SD card is missing.

The AM/PM colours are derived from `hour24 >= 12`; no AM/PM string exists
anywhere in the UI.

## Voice

`mosu_phrase_build` turns the local time into an ordered clip list:

| Case | Clips |
| --- | --- |
| 14:30 Tuesday | `h_2.wav`, `m_30.wav`, `d_1.wav` |
| 09:00 Tuesday | `h_9.wav`, `d_1.wav` (no minute clip) |
| 12:01 Monday, no day | `h_12.wav`, `m_1.wav` |

The audio task plays each file with `mosu_board_audio_play_wav`, inserting
`clip_gap_ms` between fragments. A newer request replaces a waiting one but
never interrupts a sentence in progress. A `NO_LIGHT_SLEEP` lock is held for
the duration, and the I2S channel and codec are released afterwards.

Exact hours deliberately produce a single sentence. There is no "fix", no
"exactly", no "o'clock": *"Este ora două."*

## Network

The default `wifi_always_on = false` makes the radio a guest:

```
connect to network1..networkN (15 s each)
  -> geolocation (8 s timeout, certificate bundle for HTTPS)
  -> SNTP (15 s timeout, two servers)
  -> esp_wifi_disconnect() + esp_wifi_stop()
```

If every network fails, the state becomes `FAILED` and the icon turns red. The
next attempt is scheduled `sync_hours` later. The watch never blocks the UI on
the network: `mosu_net` runs in its own task and the clock keeps ticking from
the RTC.

## Failure behaviour

| Failure | Result |
| --- | --- |
| No SD card | cached config from NVS, no voice, clock still works |
| No `mosu.cfg` | same as above; defaults on first boot |
| No Wi-Fi | RTC keeps time; Wi-Fi icon red; retries later |
| No RTC and no network | `--:--` until `manual_time` or a sync |
| No voice clips | silent announcement, screen and clock unaffected |
| PMU missing | battery icon outline in grey |
| IMU missing | wrist raise unavailable; touch wake still works |
| Panel missing | firmware keeps running, console available |

## Porting to another board

Only `components/mosu_board` mentions this hardware. A port must provide:

* `mosu_board_init` and the I2C devices it needs;
* a 12-hour-capable display plus an LVGL display/touch registration
  (`mosu_board_display_start`, `mosu_board_display_on/off`,
  `mosu_board_brightness_set`, `mosu_board_touch_active`);
* an accelerometer read in g, a UTC RTC read/write, and a WAV player;
* the pin defines in `mosu_board.h`.

Everything above the board - layout, digits, colours, phrases, config, power
state machine - is board-independent. If the new panel is bigger or smaller,
edit `mosu_layout.h` and rerun the simulator to check the proportions before
flashing.
