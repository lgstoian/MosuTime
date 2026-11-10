# Voice

MosuTime has no audio in flash. Every announcement is assembled at runtime
from small WAV files on the SD card, so the wording and the voice can change
without rebuilding the firmware. `tools/gen_voice.py` renders all of them with
[Piper](https://github.com/rhasspy/piper), and `tools/check_voice.py` verifies
a folder before it is copied onto the card.

## The 78 clips

| Group | Files | Count | Spoken |
| --- | --- | --- | --- |
| Hour | `h_1.wav` - `h_12.wav` | 12 | *Este ora ...* |
| Minute | `m_1.wav` - `m_59.wav` | 59 | *și ... minute.* |
| Weekday | `d_0.wav` - `d_6.wav` | 7 | *Luni.* - *Duminică.* |

The minute clip starts with *și* on purpose: the firmware plays hour, minute
and day in order, and the conjunction belongs to the second clip. That is why
78 files cover every minute of the day.

### Hours

| Clip | Text |
| --- | --- |
| `h_1.wav` | *Este ora unu* |
| `h_2.wav` | *Este ora două* |
| `h_3.wav` | *Este ora trei* |
| `h_4.wav` | *Este ora patru* |
| `h_5.wav` | *Este ora cinci* |
| `h_6.wav` | *Este ora șase* |
| `h_7.wav` | *Este ora șapte* |
| `h_8.wav` | *Este ora opt* |
| `h_9.wav` | *Este ora nouă* |
| `h_10.wav` | *Este ora zece* |
| `h_11.wav` | *Este ora unsprezece* |
| `h_12.wav` | *Este ora douăsprezece* |

The hour clip has no closing period in the text given to Piper: the same file
is played alone for an exact hour and in the middle of a sentence when a
minute and a weekday follow. A statement contour is correct in both cases.

### Minutes

| Clip | Text |
| --- | --- |
| `m_1.wav` | *și un minut.* |
| `m_2.wav` | *și două minute.* |
| `m_3.wav` | *și trei minute.* |
| `m_4.wav` | *și patru minute.* |
| `m_5.wav` | *și cinci minute.* |
| `m_6.wav` | *și șase minute.* |
| `m_7.wav` | *și șapte minute.* |
| `m_8.wav` | *și opt minute.* |
| `m_9.wav` | *și nouă minute.* |
| `m_10.wav` | *și zece minute.* |
| `m_11.wav` | *și unsprezece minute.* |
| `m_12.wav` | *și douăsprezece minute.* |
| `m_13.wav` | *și treisprezece minute.* |
| `m_14.wav` | *și paisprezece minute.* |
| `m_15.wav` | *și cincisprezece minute.* |
| `m_16.wav` | *și șaisprezece minute.* |
| `m_17.wav` | *și șaptesprezece minute.* |
| `m_18.wav` | *și optsprezece minute.* |
| `m_19.wav` | *și nouăsprezece minute.* |
| `m_20.wav` | *și douăzeci de minute.* |
| `m_21.wav` | *și douăzeci și una de minute.* |
| `m_22.wav` | *și douăzeci și două de minute.* |
| `m_23.wav` | *și douăzeci și trei de minute.* |
| `m_24.wav` | *și douăzeci și patru de minute.* |
| `m_25.wav` | *și douăzeci și cinci de minute.* |
| `m_26.wav` | *și douăzeci și șase de minute.* |
| `m_27.wav` | *și douăzeci și șapte de minute.* |
| `m_28.wav` | *și douăzeci și opt de minute.* |
| `m_29.wav` | *și douăzeci și nouă de minute.* |
| `m_30.wav` | *și treizeci de minute.* |
| `m_31.wav` | *și treizeci și una de minute.* |
| `m_32.wav` | *și treizeci și două de minute.* |
| `m_33.wav` | *și treizeci și trei de minute.* |
| `m_34.wav` | *și treizeci și patru de minute.* |
| `m_35.wav` | *și treizeci și cinci de minute.* |
| `m_36.wav` | *și treizeci și șase de minute.* |
| `m_37.wav` | *și treizeci și șapte de minute.* |
| `m_38.wav` | *și treizeci și opt de minute.* |
| `m_39.wav` | *și treizeci și nouă de minute.* |
| `m_40.wav` | *și patruzeci de minute.* |
| `m_41.wav` | *și patruzeci și una de minute.* |
| `m_42.wav` | *și patruzeci și două de minute.* |
| `m_43.wav` | *și patruzeci și trei de minute.* |
| `m_44.wav` | *și patruzeci și patru de minute.* |
| `m_45.wav` | *și patruzeci și cinci de minute.* |
| `m_46.wav` | *și patruzeci și șase de minute.* |
| `m_47.wav` | *și patruzeci și șapte de minute.* |
| `m_48.wav` | *și patruzeci și opt de minute.* |
| `m_49.wav` | *și patruzeci și nouă de minute.* |
| `m_50.wav` | *și cincizeci de minute.* |
| `m_51.wav` | *și cincizeci și una de minute.* |
| `m_52.wav` | *și cincizeci și două de minute.* |
| `m_53.wav` | *și cincizeci și trei de minute.* |
| `m_54.wav` | *și cincizeci și patru de minute.* |
| `m_55.wav` | *și cincizeci și cinci de minute.* |
| `m_56.wav` | *și cincizeci și șase de minute.* |
| `m_57.wav` | *și cincizeci și șapte de minute.* |
| `m_58.wav` | *și cincizeci și opt de minute.* |
| `m_59.wav` | *și cincizeci și nouă de minute.* |

### Weekdays

| Clip | Text |
| --- | --- |
| `d_0.wav` | *Luni.* |
| `d_1.wav` | *Marți.* |
| `d_2.wav` | *Miercuri.* |
| `d_3.wav` | *Joi.* |
| `d_4.wav` | *Vineri.* |
| `d_5.wav` | *Sâmbătă.* |
| `d_6.wav` | *Duminică.* |

`d_0` is Monday, not Sunday: `struct tm` counts from Sunday, and
`mosu_weekday_from_tm()` converts it before the clip name is built.

## Romanian grammar rules used

| Rule | Example |
| --- | --- |
| One minute is singular, with *un* | *un minut*, not *unu minut* |
| 2 to 19 take the numeral directly | *două minute*, *nouăsprezece minute* |
| 20 to 59 need *de* before the noun | *douăzeci de minute*, *treizeci și cinci de minute* |
| *minut* is neuter, so 1 and 2 in the plural take the feminine form | *două minute*, *douăzeci și una de minute*, not *...și unu de minute* |
| The hour is told with the feminine *ora*, but *unu* stays invariable | *Este ora unu*, *Este ora două* |
| An exact hour has no minute clause, not even *zero minute* | *Este ora două. Marți.* |
| The weekday is always the last clip and ends the sentence | *...și treizeci de minute. Marți.* |

The *de* appears once, at the end of the number, even when the number is
compound: *douăzeci și una **de** minute*. The *și* between tens and units is
part of the number, not the conjunction that starts the minute clip, so
`m_21.wav` begins with *și* and then repeats it: *și douăzeci și una de
minute*.

Nothing is said about morning or evening. The face already shows that with
colour (`time_am` and `time_pm`), and *Este ora nouă* is what a Romanian
speaker says for both 09:00 and 21:00.

## Naming

`mosu_clip_filename()` builds the name with `"%s_%u.wav"`:

| Prefix | Index | Range |
| --- | --- | --- |
| `h` | hour on a 12-hour clock | 1 - 12 |
| `m` | minute | 1 - 59 |
| `d` | weekday, 0 = Monday | 0 - 6 |

Index 0 is invalid for hours and minutes; the firmware refuses it. The names
are case-sensitive and must match exactly.

## How the firmware joins the clips

`mosu_phrase_build()` (in `components/mosu_core`) decides what to say:

1. the hour is converted to 1 - 12: 00:xx and 12:xx use `h_12`, 13:xx uses
   `h_1`, and so on;
2. the minute clip is added only when the minute is not zero;
3. the weekday clip is added when `announce_day = true` in `mosu.cfg`.

`mosu_audio` then plays the files in that order, with `clip_gap_ms` of silence
between two clips (not after the last one). Nothing is mixed or crossfaded;
each file is opened, streamed and closed on its own.

| Time | Clips | What is heard |
| --- | --- | --- |
| 02:30, Tuesday | `h_2` `m_30` `d_1` | *Este ora două și treizeci de minute. Marți.* |
| 09:00, Tuesday | `h_9` `d_1` | *Este ora nouă. Marți.* |
| 21:57, Tuesday | `h_9` `m_57` `d_1` | *Este ora nouă și cincizeci și șapte de minute. Marți.* |
| 12:00, Sunday | `h_12` `d_6` | *Este ora douăsprezece. Duminică.* |

With `announce_day = false` the weekday clip is simply skipped.

## Generating the clips

```sh
pip install piper-tts
python tools/gen_voice.py --out sd_card/voice
```

The first run downloads the `ro_RO-mihai-medium` voice (63 MB) and its config
into `tools/piper-models/`. That folder and the generated WAVs are ignored by
git. Rendering all 78 clips takes several minutes on a laptop: every clip
starts a fresh Piper process, which reloads the model each time.

| Option | Default | Meaning |
| --- | --- | --- |
| `--out` | `sd_card/voice` | output folder |
| `--models` | `tools/piper-models` | where the voice is cached |
| `--piper` | auto | path to a `piper` executable |
| `--length-scale` | `1.08` | Piper speaking rate; higher is slower |
| `--clip-gap-ms` | `60` | gap used when building the preview |
| `--preview` | off | also write `<out>/preview.wav` |
| `--force` | off | re-render clips that already exist |
| `--dry-run` | off | print the 78 phrases and exit |

`--length-scale` is 8 % slower than Piper's default. For an elderly listener
the extra length helps more than it hurts; lower it towards `1.0` if the
result drags. Both Piper command-line styles are supported (`--length_scale`
for Piper 1.2 and `--length-scale` for `piper1-gpl`).

After each render the clip is post-processed in pure Python, without external
tools:

* leading and trailing silence below -45 dBFS is removed, keeping 20 ms of
  padding so a plosive is not chopped;
* the peak is normalised to -3 dBFS, leaving headroom for the speaker;
* a 10 ms linear fade is applied at both ends.

The fade matters because the watch concatenates files: without it, a clip cut
in the middle of a waveform starts and ends with a step that is heard as a
click. The output is always mono 16-bit at 22050 Hz, the native rate of this
voice.

Trimming also makes the pauses predictable. Piper leaves a variable amount of
silence after each sentence, and the firmware adds exactly `clip_gap_ms`
between clips, so the old untrimmed files produced uneven gaps.

## Preview

```sh
python tools/gen_voice.py --out sd_card/voice --preview
```

This writes `sd_card/voice/preview.wav`: the announcements for 02:30, 09:00,
21:57 and 12:00 on a Tuesday, cut from the real `h`/`m`/`d` clips and joined
with the same 60 ms gap the firmware uses. It is a listening aid only, the
firmware never opens it, and `check_voice.py` treats it as an allowed extra.
Delete it before copying the folder if unused space matters.

## Checking a folder

```sh
python tools/check_voice.py sd_card/voice
```

The checker requires the 78 names above, reads every file and reports
duration, peak and RMS per clip. It fails (exit status 1) when a clip is
missing, unreadable, not mono 16-bit, clipped (a sample reaches full scale) or
silent (peak below -60 dBFS). A sample rate other than 22050 Hz is a warning;
a rate outside the 8000 - 48000 Hz range that the firmware accepts is an
error.

```
sd_card/voice: 78 clips expected, 78 found
name       dur s peak dBFS rms dBFS   rate  note
h_1.wav     1.06      -3.0    -18.6  22050
...
groups: h 12/12, m 59/59, d 7/7
total 285.54 s, peak -3.0..-3.0 dBFS, rms -20.9..-16.4 dBFS
rates: 22050
OK: 78 clips, mono 16-bit, 22050 Hz
```

Very short clips (under 0.15 s) are listed as a warning: they are usually a
sign that something went wrong in the render.

## Replacing the voice

The firmware only knows the file names, not the voice, so any set of WAVs with
the right names works:

1. produce or record 78 clips with the texts in this document;
2. keep them mono 16-bit WAV, 22050 Hz recommended (8000 - 48000 Hz accepted);
3. avoid clipping and long silences at the ends;
4. run `python tools/check_voice.py <folder>`;
5. copy the folder to the card and point `voice_dir` in `[announce]` at it.

To keep Piper but change its sound, edit `VOICE_URL` and `VOICE_NAME` in
`tools/gen_voice.py` for another voice from
[rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices) and delete
`tools/piper-models/` so the new model is fetched. A Romanian voice is needed
for these texts; other languages would need the phrases translated as well.

The directory may contain other files, including `preview.wav`; the firmware
opens only the names it builds.

## Licences

* The original [rhasspy/piper](https://github.com/rhasspy/piper) code is MIT.
  The `piper-tts` package installed by pip today is
  [piper1-gpl](https://github.com/OHF-voice/piper1-gpl), which is
  GPL-3.0-or-later because it carries espeak-ng for phonemisation. That
  licence covers the tool, which runs on the developer's machine; nothing from
  it is linked into the firmware.
* The voices in `rhasspy/piper-voices` are MIT. Each voice directory has a
  `MODEL_CARD` describing the dataset it was trained on - `ro_RO-mihai-medium`
  was trained on the Romanian *mihai* dataset - so read that card before
  redistributing the generated audio.
* The generated WAV files are not committed to this repository. They are
  produced locally by `tools/gen_voice.py`.
* If you replace the voice with a recording of a real person, you need that
  person's permission before distributing the clips, and the recording keeps
  its own licence.

See also [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
