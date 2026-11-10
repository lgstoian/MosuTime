#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Generate the Romanian voice clips for MosuTime.

The watch never embeds a voice pack: it plays small WAV files from the SD
card and joins them at runtime:

    h_2.wav  "Este ora doua"        (hour, 1..12)
    m_30.wav "si treizeci de minute." (minute, 1..59)
    d_1.wav  "Marti."                (weekday, 0=Monday .. 6=Sunday)

This script downloads a free Piper voice and renders every clip.  Piper is
MIT licensed, and the voices in rhasspy/piper-voices are free to use; see
THIRD_PARTY_NOTICES.md.

Usage
-----
    pip install piper-tts
    python tools/gen_voice.py --out sd_card/voice

    # Only show what would be generated:
    python tools/gen_voice.py --dry-run

The script also accepts a standalone Piper executable with --piper.
"""

from __future__ import annotations

import argparse
import os
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
               destination: Path) -> None:
    if modern_flags:
        command = argv + [
            "--model",
            str(model),
            "--output-file",
            str(destination),
            "--length-scale",
            "1.12",
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
            "1.12",
            "--sentence_silence",
            "0.05",
        ]
    subprocess.run(command, input=(text + "\n").encode("utf-8"), check=True)


def check_wav(path: Path) -> tuple[int, int, int]:
    with wave.open(str(path), "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise ValueError(
                f"{path.name}: need mono 16-bit, got {handle.getnchannels()} ch "
                f"{handle.getsampwidth() * 8} bit"
            )
        return handle.getframerate(), handle.getnframes(), handle.getnchannels()


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
    parser.add_argument("--dry-run", action="store_true",
                        help="print the clips without synthesising them")
    parser.add_argument("--force", action="store_true",
                        help="regenerate clips that already exist")
    args = parser.parse_args()

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
    made = 0
    for name, text in plan:
        destination = out / name
        if destination.exists() and not args.force:
            continue
        synthesise(argv, modern_flags, model, text, destination)
        rate, frames, _ = check_wav(destination)
        made += 1
        print(f"  {name:10s} {rate} Hz  {frames / rate:5.2f} s  {text}")

    total = len(plan)
    print(f"\n{made} new clips, {total} total in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
