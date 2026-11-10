#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Generate the Romanian voice clips for MosuTime.

The watch never embeds a voice pack: it plays small WAV files from the SD
card and joins them at runtime:

    h_2.wav  "Este ora două"        (hour, 1..12)
    m_30.wav "și treizeci de minute." (minute, 1..59)
    d_1.wav  "Marți."                (weekday, 0=Monday .. 6=Sunday)

This script downloads a free Piper voice and renders every clip.  Each clip
is post-processed in pure Python: leading and trailing silence is trimmed,
the result is peak-normalised to -3 dBFS and gets a 10 ms fade at both ends,
so the firmware can join clips without audible steps.

Piper is MIT licensed, and the voices in rhasspy/piper-voices are free to
use; see THIRD_PARTY_NOTICES.md and docs/voice.md.

Usage
-----
    pip install piper-tts
    python tools/gen_voice.py --out sd_card/voice

    # Also render sd_card/voice/preview.wav with a few announcements:
    python tools/gen_voice.py --out sd_card/voice --preview

    # Only show what would be generated:
    python tools/gen_voice.py --dry-run

The script also accepts a standalone Piper executable with --piper.
"""

from __future__ import annotations

import argparse
import array
import math
import shutil
import subprocess
import sys
import urllib.request
import wave
from pathlib import Path

VOICE_URL = (
    "https://huggingface.co/rhasspy/piper-voices/resolve/main/ro/ro_RO/mihai/"
    "medium/ro_RO-mihai-medium.onnx"
)
VOICE_CONFIG_URL = VOICE_URL + ".json"
VOICE_NAME = "ro_RO-mihai-medium"

DEFAULT_LENGTH_SCALE = 1.08
DEFAULT_CLIP_GAP_MS = 60  # mirrors clip_gap_ms in sd_card/mosu.cfg

FULL_SCALE = 32767.0
TRIM_THRESHOLD_DBFS = -45.0
TRIM_KEEP_MS = 20.0
PEAK_TARGET_DBFS = -3.0
FADE_MS = 10.0

HOURS = [
    "unu",
    "două",
    "trei",
    "patru",
    "cinci",
    "șase",
    "șapte",
    "opt",
    "nouă",
    "zece",
    "unsprezece",
    "douăsprezece",
]

UNITS_FEM = [
    "zero",
    "una",
    "două",
    "trei",
    "patru",
    "cinci",
    "șase",
    "șapte",
    "opt",
    "nouă",
    "zece",
    "unsprezece",
    "douăsprezece",
    "treisprezece",
    "paisprezece",
    "cincisprezece",
    "șaisprezece",
    "șaptesprezece",
    "optsprezece",
    "nouăsprezece",
]

TENS = {
    20: "douăzeci",
    30: "treizeci",
    40: "patruzeci",
    50: "cincizeci",
}

DAYS = [
    "Luni.",
    "Marți.",
    "Miercuri.",
    "Joi.",
    "Vineri.",
    "Sâmbătă.",
    "Duminică.",
]

# (hour24, minute, weekday): what --preview speaks, in firmware order.
PREVIEW_PHRASES = [
    (2, 30, 1),   # 02:30, Tuesday
    (9, 0, 1),    # 09:00, Tuesday
    (21, 57, 1),  # 21:57, Tuesday
    (12, 0, 1),   # 12:00, Tuesday
]


def number_fem(n: int) -> str:
    """1..59, with the feminine forms used for hours and minutes."""
    if n < 20:
        return UNITS_FEM[n]
    tens = (n // 10) * 10
    unit = n % 10
    if unit == 0:
        return TENS[tens]
    return f"{TENS[tens]} și {UNITS_FEM[unit]}"


def hour_text(hour12: int) -> str:
    # No final period: the same clip is used alone (exact hour) and followed
    # by the minute clause.  A statement contour sounds right in both cases.
    return f"Este ora {HOURS[hour12 - 1]}"


def minute_text(minute: int) -> str:
    if minute == 1:
        return "și un minut."
    if minute < 20:
        return f"și {number_fem(minute)} minute."
    return f"și {number_fem(minute)} de minute."


def clip_plan():
    """Return a list of (filename, spoken text)."""
    plan = []
    for hour in range(1, 13):
        plan.append((f"h_{hour}.wav", hour_text(hour)))
    for minute in range(1, 60):
        plan.append((f"m_{minute}.wav", minute_text(minute)))
    for day, name in enumerate(DAYS):
        plan.append((f"d_{day}.wav", name))
    return plan


def phrase_files(hour24: int, minute: int, weekday: int):
    """Clip names for one announcement, in the order the firmware plays them."""
    hour12 = hour24 % 12 or 12
    names = [f"h_{hour12}.wav"]
    if minute % 60:
        names.append(f"m_{minute % 60}.wav")
    names.append(f"d_{weekday % 7}.wav")
    return names


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.stat().st_size > 0:
        return

    print(f"downloading {url}")
    tmp = destination.with_suffix(destination.suffix + ".part")
    with urllib.request.urlopen(url, timeout=60) as response, open(tmp, "wb") as fh:
        total = int(response.headers.get("content-length") or 0)
        done = 0
        while True:
            block = response.read(1024 * 256)
            if not block:
                break
            fh.write(block)
            done += len(block)
            if total:
                percent = done * 100 // total
                print(f"\r  {percent:3d}%  {done // 1024} KiB", end="", flush=True)
    print()
    tmp.replace(destination)


def find_piper(explicit: str | None):
    """
    Return (argv, modern_flags) for the Piper CLI, or (None, False).

    Piper 1.2 uses --output_file / --length_scale; the newer piper1-gpl
    package uses --output-file / --length-scale.  Both read the sentence from
    standard input.
    """
    candidates = []
    if explicit:
        candidates.append([explicit])
    exe = shutil.which("piper")
    if exe:
        candidates.append([exe])
    candidates.append([sys.executable, "-m", "piper"])

    for argv in candidates:
        try:
            help_run = subprocess.run(
                argv + ["--help"], capture_output=True, text=True, timeout=30
            )
        except (OSError, subprocess.SubprocessError):
            continue
        help_text = help_run.stdout + help_run.stderr
        if help_run.returncode != 0:
            continue
        if "--output_file" in help_text:
            return argv, False
        if "--output-file" in help_text:
            return argv, True
    return None, False


def synthesise(argv, modern_flags: bool, model: Path, text: str,
               destination: Path, length_scale: float) -> None:
    scale = f"{length_scale:.2f}"
    if modern_flags:
        command = argv + [
            "--model",
            str(model),
            "--output-file",
            str(destination),
            "--length-scale",
            scale,
            "--sentence-silence",
            "0.05",
        ]
    else:
        command = argv + [
            "--model",
            str(model),
            "--output_file",
            str(destination),
            "--length_scale",
            scale,
            "--sentence_silence",
            "0.05",
        ]
    subprocess.run(command, input=(text + "\n").encode("utf-8"), check=True)


def dbfs_to_peak(dbfs: float) -> int:
    """Full-scale sample value for a level in dBFS."""
    return int(round(FULL_SCALE * (10.0 ** (dbfs / 20.0))))


def read_mono(path: Path) -> tuple[int, array.array]:
    """Read a mono 16-bit WAV into a signed-short array."""
    with wave.open(str(path), "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        frames = handle.readframes(handle.getnframes())
    if channels != 1 or width != 2:
        raise ValueError(
            f"{path.name}: need mono 16-bit, got {channels} ch {width * 8} bit"
        )
    samples = array.array("h")
    samples.frombytes(frames)
    if sys.byteorder != "little":
        samples.byteswap()
    return rate, samples


def write_mono(path: Path, rate: int, samples: array.array) -> None:
    data = array.array("h", samples)
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(data.tobytes())


def peak_level(samples: array.array) -> int:
    peak = 0
    for sample in samples:
        value = sample if sample >= 0 else -sample
        if value > peak:
            peak = value
    return peak


def level_dbfs(peak: int) -> float:
    if peak <= 0:
        return float("-inf")
    return 20.0 * math.log10(peak / FULL_SCALE)


def rms_dbfs(samples: array.array) -> float:
    if not samples:
        return float("-inf")
    total = 0
    for sample in samples:
        total += sample * sample
    rms = math.sqrt(total / len(samples))
    if rms <= 0:
        return float("-inf")
    return 20.0 * math.log10(rms / FULL_SCALE)


def trim_silence(samples: array.array, rate: int, threshold: int,
                 keep_ms: float) -> tuple[array.array, bool]:
    """Cut leading/trailing silence, keeping keep_ms of padding."""
    keep = int(rate * keep_ms / 1000.0)
    total = len(samples)

    first = 0
    while first < total and -threshold <= samples[first] <= threshold:
        first += 1
    if first == total:
        return samples, False  # silent clip: leave it alone for the checker

    last = total - 1
    while last > first and -threshold <= samples[last] <= threshold:
        last -= 1

    start = max(0, first - keep)
    end = min(total, last + 1 + keep)
    return samples[start:end], True


def normalise(samples: array.array, target_peak: int,
              fade: int) -> array.array:
    """Peak-normalise, then fade the first and last `fade` samples."""
    peak = peak_level(samples)
    if peak == 0:
        return array.array("h", samples)

    gain = target_peak / peak
    out = array.array("h", [int(round(sample * gain)) for sample in samples])

    fade = min(fade, len(out) // 2)
    for index in range(fade):
        weight = index / fade
        out[index] = int(round(out[index] * weight))
        out[-1 - index] = int(round(out[-1 - index] * weight))
    return out


def postprocess(path: Path, peak_dbfs: float = PEAK_TARGET_DBFS) -> dict:
    """Trim, normalise and fade one clip in place; return its statistics."""
    rate, samples = read_mono(path)
    before = len(samples) / rate if rate else 0.0

    threshold = dbfs_to_peak(TRIM_THRESHOLD_DBFS)
    trimmed, found = trim_silence(samples, rate, threshold, TRIM_KEEP_MS)
    if not found:
        trimmed = samples

    fade = int(rate * FADE_MS / 1000.0)
    out = normalise(trimmed, dbfs_to_peak(peak_dbfs), fade)
    write_mono(path, rate, out)

    duration = len(out) / rate if rate else 0.0
    return {
        "rate": rate,
        "duration": duration,
        "trimmed_ms": (before - duration) * 1000.0,
        "peak_dbfs": level_dbfs(peak_level(out)),
        "rms_dbfs": rms_dbfs(out),
    }


def build_preview(out: Path, gap_ms: int) -> int:
    """Concatenate a few complete announcements into out/preview.wav."""
    missing = []
    rate = None
    gap = array.array("h")
    pieces = []
    total_frames = 0.0

    print("\npreview")
    for hour24, minute, weekday in PREVIEW_PHRASES:
        names = phrase_files(hour24, minute, weekday)
        phrase_frames = 0
        used = []
        for name in names:
            path = out / name
            if not path.exists():
                missing.append(name)
                continue
            try:
                clip_rate, samples = read_mono(path)
            except ValueError as error:
                print(f"cannot preview {name}: {error}", file=sys.stderr)
                return 1
            if rate is None:
                rate = clip_rate
                gap = array.array("h", [0] * int(rate * gap_ms / 1000.0))
            if clip_rate != rate:
                print(f"  {name}: {clip_rate} Hz differs from {rate} Hz",
                      file=sys.stderr)
                return 1
            if used:
                pieces.append(gap)
            pieces.append(samples)
            total_frames += len(samples)
            phrase_frames += len(samples)
            used.append(name)

        if used:
            print(f"  {hour24:02d}:{minute:02d}  {' + '.join(used):28s} "
                  f"{phrase_frames / rate:5.2f} s")

    if missing:
        for name in dict.fromkeys(missing):
            print(f"missing clip: {name}", file=sys.stderr)
        return 1
    if rate is None or not pieces:
        print("no clips to preview", file=sys.stderr)
        return 1

    merged = array.array("h")
    for piece in pieces:
        merged.extend(piece)

    preview = out / "preview.wav"
    write_mono(preview, rate, merged)
    seconds = len(merged) / rate
    print(f"wrote {preview}  ({len(merged)} frames, {seconds:.2f} s, "
          f"{gap_ms} ms gaps)")
    return 0


def main() -> int:
    # Romanian text on a Windows console: do not let cp1252 abort the run.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="sd_card/voice",
                        help="output folder (default: sd_card/voice)")
    parser.add_argument("--models", default="tools/piper-models",
                        help="where the Piper voice is cached")
    parser.add_argument("--piper", default=None,
                        help="path to a piper executable")
    parser.add_argument("--length-scale", type=float,
                        default=DEFAULT_LENGTH_SCALE,
                        help="Piper speed: higher is slower "
                             f"(default: {DEFAULT_LENGTH_SCALE})")
    parser.add_argument("--clip-gap-ms", type=int, default=DEFAULT_CLIP_GAP_MS,
                        help="silence between preview clips, in ms "
                             f"(default: {DEFAULT_CLIP_GAP_MS})")
    parser.add_argument("--preview", action="store_true",
                        help="also write <out>/preview.wav with a few "
                             "complete announcements")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the clips without synthesising them")
    parser.add_argument("--force", action="store_true",
                        help="regenerate clips that already exist")
    args = parser.parse_args()

    if args.length_scale <= 0:
        print("--length-scale must be greater than zero", file=sys.stderr)
        return 2
    if args.clip_gap_ms < 0:
        print("--clip-gap-ms must not be negative", file=sys.stderr)
        return 2

    out = Path(args.out)
    models = Path(args.models)
    plan = clip_plan()

    if args.dry_run:
        for name, text in plan:
            print(f"{name:10s} {text}")
        print(f"\n{len(plan)} clips")
        return 0
    out.mkdir(parents=True, exist_ok=True)

    model = models / f"{VOICE_NAME}.onnx"
    config = models / f"{VOICE_NAME}.onnx.json"
    download(VOICE_URL, model)
    download(VOICE_CONFIG_URL, config)

    argv, modern_flags = find_piper(args.piper)
    if argv is None:
        print(
            "Piper was not found.\n"
            "  pip install piper-tts\n"
            "or pass --piper C:\\path\\to\\piper.exe",
            file=sys.stderr,
        )
        return 2

    print(f"using {' '.join(argv)}")
    print(f"length-scale {args.length_scale:.2f}, trim {TRIM_THRESHOLD_DBFS:g} "
          f"dBFS, peak {PEAK_TARGET_DBFS:g} dBFS, fade {FADE_MS:g} ms")
    made = 0
    spoken = 0.0
    for name, text in plan:
        destination = out / name
        if destination.exists() and not args.force:
            continue
        synthesise(argv, modern_flags, model, text, destination,
                   args.length_scale)
        info = postprocess(destination)
        made += 1
        spoken += info["duration"]
        print(f"  {name:10s} {info['rate']} Hz  {info['duration']:5.2f} s  "
              f"peak {info['peak_dbfs']:5.1f} dBFS  {text}")

    print(f"\n{made} new clips, {len(plan)} total in {out}")
    if made:
        print(f"{spoken:.2f} s of speech generated")

    if args.preview:
        return build_preview(out, args.clip_gap_ms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
