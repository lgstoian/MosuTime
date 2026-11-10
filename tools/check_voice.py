#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Check a MosuTime voice folder before copying it to the watch.

The firmware plays the clips from the SD card, so it needs:

  * exactly the 78 expected names: h_1..h_12, m_1..m_59, d_0..d_6
  * mono, 16-bit WAV at 22050 Hz (other rates are played but warned about)
  * no clipping (the loudest sample must stay below full scale)
  * no silent or empty clip

The checker prints one line per clip with duration, peak and RMS, and exits
with status 1 when anything is missing or unusable.

Usage
-----
    python tools/check_voice.py sd_card/voice
"""

from __future__ import annotations

import argparse
import array
import math
import sys
import wave
from pathlib import Path

EXPECTED_RATE = 22050
MIN_RATE = 8000     # mosu_board_audio_play_wav() rejects anything outside this
MAX_RATE = 48000
FULL_SCALE = 32767.0
CLIP_PEAK = 32767   # reaching full scale means the render clipped
SILENT_DBFS = -60.0

# A directory may also hold the optional preview rendered by gen_voice.py.
ALLOWED_EXTRA = {"preview.wav"}


def expected_names() -> list[str]:
    names = [f"h_{hour}.wav" for hour in range(1, 13)]
    names += [f"m_{minute}.wav" for minute in range(1, 60)]
    names += [f"d_{day}.wav" for day in range(7)]
    return names


def dbfs(peak: float) -> float:
    if peak <= 0:
        return float("-inf")
    return 20.0 * math.log10(peak / FULL_SCALE)


def analyse(path: Path) -> dict:
    """Return header facts and levels, or an error string.

    Levels are only measured for mono 16-bit data: reading a stereo or 8-bit
    file as signed shorts would produce numbers that mean nothing.
    """
    try:
        with wave.open(str(path), "rb") as handle:
            channels = handle.getnchannels()
            width = handle.getsampwidth()
            rate = handle.getframerate()
            frames = handle.getnframes()
            raw = handle.readframes(frames)
    except (wave.Error, EOFError, OSError) as error:
        return {"error": str(error)}

    stats = {
        "channels": channels,
        "width": width,
        "rate": rate,
        "frames": frames,
        "duration": frames / rate if rate else 0.0,
    }
    if channels != 1 or width != 2:
        return stats

    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()

    if samples:
        peak = max(max(samples), -min(samples))
        total = 0
        for sample in samples:
            total += sample * sample
        rms = math.sqrt(total / len(samples))
    else:
        peak = 0
        rms = 0.0

    stats.update({
        "peak": peak,
        "peak_db": dbfs(peak),
        "rms_db": dbfs(rms),
    })
    return stats


def check(path: Path) -> tuple[dict, list[str], list[str]]:
    """Return (stats, problems, warnings) for one clip."""
    stats = analyse(path)
    if "error" in stats:
        return stats, [f"unreadable WAV: {stats['error']}"], []

    problems = []
    warnings = []

    if stats["channels"] != 1:
        problems.append(f"{stats['channels']} channels, need 1")
    if stats["width"] != 2:
        problems.append(f"{stats['width'] * 8}-bit, need 16-bit")
    if stats["rate"] != EXPECTED_RATE:
        if stats["rate"] < MIN_RATE or stats["rate"] > MAX_RATE:
            problems.append(f"{stats['rate']} Hz outside "
                            f"{MIN_RATE}..{MAX_RATE} Hz")
        else:
            warnings.append(f"{stats['rate']} Hz, expected {EXPECTED_RATE}")
    if not stats["frames"]:
        problems.append("no samples")
    elif "peak" in stats:
        if stats["peak"] >= CLIP_PEAK:
            problems.append(f"clipped at {stats['peak']}")
        elif stats["peak_db"] < SILENT_DBFS:
            problems.append(f"silent, peak {stats['peak']}")

    return stats, problems, warnings


def group_counts(present: set[str]) -> str:
    def count(prefix: str, total: int, first: int) -> str:
        found = sum(1 for index in range(first, first + total)
                    if f"{prefix}_{index}.wav" in present)
        return f"{prefix} {found}/{total}"

    return (f"{count('h', 12, 1)}, {count('m', 59, 1)}, "
            f"{count('d', 7, 0)}")


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", default="sd_card/voice",
                        help="voice folder (default: sd_card/voice)")
    args = parser.parse_args()

    folder = Path(args.directory)
    if not folder.is_dir():
        print(f"{folder}: not a folder", file=sys.stderr)
        return 2

    expected = expected_names()
    expected_set = set(expected)
    found = {path.name for path in folder.glob("*.wav")
             if path.is_file()}

    missing = [name for name in expected if name not in found]
    extra = sorted(found - expected_set - ALLOWED_EXTRA)

    print(f"{folder}: {len(expected)} clips expected, "
          f"{len(expected) - len(missing)} found")
    print(f"{'name':9s} {'dur s':>6s} {'peak dBFS':>9s} {'rms dBFS':>8s} "
          f"{'rate':>6s}  note")

    unusable = 0
    shorts = []
    totals = 0.0
    rates = set()
    peaks = []
    rms_values = []

    for name in expected:
        if name not in found:
            print(f"{name:9s} {'-':>6s} {'-':>9s} {'-':>8s} {'-':>6s}  MISSING")
            continue

        stats, clip_problems, warnings = check(folder / name)
        if "error" in stats:
            print(f"{name:9s} {'-':>6s} {'-':>9s} {'-':>8s} {'-':>6s}  INVALID")
            for problem in clip_problems:
                print(f"  {problem}")
            unusable += 1
            continue

        notes = []
        if clip_problems:
            unusable += 1
            notes.extend(problem.upper() for problem in clip_problems)
        if warnings:
            notes.extend(warnings)
        if stats["duration"] < 0.15:
            shorts.append(name)

        if "peak" in stats:
            peak_text = ("-inf" if stats["peak_db"] == float("-inf")
                         else f"{stats['peak_db']:.1f}")
            rms_text = ("-inf" if stats["rms_db"] == float("-inf")
                        else f"{stats['rms_db']:.1f}")
        else:
            peak_text = "-"
            rms_text = "-"
        row = (f"{name:9s} {stats['duration']:6.2f} {peak_text:>9s} "
               f"{rms_text:>8s} {stats['rate']:6d}")
        if notes:
            row += f"  {'; '.join(notes)}"
        print(row)

        totals += stats["duration"]
        rates.add(stats["rate"])
        if "peak" in stats and stats["frames"]:
            peaks.append(stats["peak_db"])
            rms_values.append(stats["rms_db"])

    print()
    print(f"groups: {group_counts(found)}")
    if peaks:
        print(f"total {totals:.2f} s, peak "
              f"{min(peaks):.1f}..{max(peaks):.1f} dBFS, rms "
              f"{min(rms_values):.1f}..{max(rms_values):.1f} dBFS")
    print(f"rates: {', '.join(str(rate) for rate in sorted(rates)) or 'none'}")

    if extra:
        print(f"unexpected wav files (ignored by the firmware): "
              f"{', '.join(extra)}")
    if shorts:
        print(f"warning: very short clips (< 0.15 s): {', '.join(shorts)}")

    if missing:
        print(f"\nFAIL: {len(missing)} missing: {', '.join(missing)}",
              file=sys.stderr)
    if unusable:
        print(f"\nFAIL: {unusable} unusable clip(s)", file=sys.stderr)
    if missing or unusable:
        return 1

    print(f"OK: {len(expected)} clips, mono 16-bit, {EXPECTED_RATE} Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
