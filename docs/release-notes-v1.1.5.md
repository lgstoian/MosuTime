# MosuTime v1.1.5

A correct *Joi* and a battery gauge that reaches 100%.

## `Joi` is pronounced correctly

The Piper Romanian voice rendered the weekday `joi` as a different word. No
phoneme spelling fixed it, so this one clip is now generated from the text by
the native Romanian `facebook/mms-tts-ron` voice. The wearer confirmed it on
the attached watch. The other 77 clips are unchanged.

`d_3.wav` must not be regenerated with `gen_voice.py --force`; see
[`docs/voice.md`](voice.md). The MMS model is CC-BY-NC-4.0 (personal,
non-commercial use).

## Battery reaches 100% after a full charge

When the AXP2101 finishes charging (status 4) the cell relaxes from about
4180 mV to 4140 mV. The voltage-only curve reported 97% even though the
charger had finished. The PMU "charge done" state is now authoritative while
VBUS is present, and the gauge reads 100%.

Verified on the watch: `status` reports `4138 mV, 100%, charging 0, vbus 1`
after a full charge.

## Release state

* `stats`: disabled; `device.diagnostics = false`; `log_level = error`.
* Audit kill switches are in their normal "on" state; no diagnostic logging
  task is writing to the card.
* Host tests: 720 checks, 0 failures. Voice folder: 78 clips valid.
* Firmware built and flashed; SD card voice folder updated.
