#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Check a captured /sdcard/power.log instead of trusting that it exists.

The firmware appends one row a minute while `diagnostics = true`:

    epoch,pct,mV,charging,screen,wakes,boots,sleep_entries

A file full of rows proves only that the writer ran.  This tool checks the
properties that matter: the samples are a continuous time series, the battery
only falls while uncharged, the face is mostly off, and the light-sleep counter
keeps rising.  The last check is the one that catches the old 110 mA regression
and the false green where opening the USB console reset the counter to zero.

Usage
-----
    python tools/check_power_log.py sd_card/power.log
    python tools/check_power_log.py --min-sleep-per-min 5 path/to/power.log
    python tools/check_power_log.py --min-rows 2 --allow-gaps power.log
    python tools/check_power_log.py --selftest
"""

from __future__ import annotations

import argparse
import sys
from statistics import median


# The app task samples once a minute.  Allow a little clock and SD jitter.
MIN_STEP_S = 50
MAX_STEP_S = 90  # an occasional slow SD write can stretch a sample past 60 s

Row = tuple[int, int, int, int, int, int, int, int]


def parse(text: str) -> tuple[list[Row], list[str]]:
    """Return numeric rows and parse errors (bad lines are never ignored)."""
    rows: list[Row] = []
    errors: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        fields = line.split(",")
        if len(fields) != 8:
            errors.append(f"line {number}: expected 8 fields, got {len(fields)}")
            continue
        try:
            row = tuple(int(field) for field in fields)
        except ValueError:
            errors.append(f"line {number}: non-numeric field")
            continue
        rows.append(row)  # type: ignore[arg-type]
    return rows, errors


def split_runs(rows: list[Row]) -> list[tuple[int, list[Row]]]:
    """Split a captured file at recharges, reboots and clock jumps.

    A card can hold several nights of logs.  Comparing the end of one run with
    the start of the next would invent battery rises and counter resets that
    never happened inside a measurement.
    """
    runs: list[tuple[int, list[Row]]] = []
    for index, row in enumerate(rows):
        boundary = not runs or row[3] == 1
        if runs:
            start, run = runs[-1]
            previous = run[-1]
            boundary |= (row[6] != previous[6] or
                         not MIN_STEP_S <= row[0] - previous[0] <= MAX_STEP_S or
                         row[1] > previous[1] + 1)
        if boundary:
            runs.append((index, [row]))
        else:
            runs[-1][1].append(row)
    return runs


def check_rows(rows: list[Row], min_sleep: int, min_rows: int = 10,
               allow_gaps: bool = False) -> list[str]:
    """Return every failed invariant; an empty list means the log is real."""
    errors: list[str] = []
    if len(rows) < min_rows:
        return [f"only {len(rows)} rows; need at least {min_rows} for a rate"]
    for index, row in enumerate(rows):
        if row[0] < 1600000000:
            errors.append(f"row {index}: implausible epoch {row[0]}")
        if row[3] not in (0, 1) or row[4] not in (0, 1):
            errors.append(f"row {index}: charging/screen must be 0 or 1")
        if index and row[3] != 1 and not allow_gaps:
            step = row[0] - rows[index - 1][0]
            if not MIN_STEP_S <= step <= MAX_STEP_S:
                errors.append(f"row {index}: {step} s since last sample")

    runs = split_runs(rows)
    discharge = max((run for _start, run in runs if len(run) >= min_rows),
                    key=len, default=[])
    if not discharge:
        return errors + [f"no discharge run with at least {min_rows} samples"]

    for start, run in runs:
        for offset, (before, now) in enumerate(zip(run, run[1:]), 1):
            index = start + offset
            if now[7] < before[7]:
                errors.append(
                    f"row {index}: sleep entries fell {before[7]} -> {now[7]} "
                    "inside one run (a zero is not proof of no sleep)"
                )
            if now[1] > before[1] + 1:
                errors.append(
                    f"row {index}: battery rose {before[1]}% -> {now[1]}% "
                    "inside one discharge run"
                )

    deltas = [now[7] - before[7]
              for before, now in zip(discharge, discharge[1:])]
    rate = median(deltas) if deltas else 0
    if rate < min_sleep:
        errors.append(
            f"median light-sleep rate {rate:g}/min below {min_sleep}/min; "
            "the CPU may be kept awake"
        )
    lit = sum(row[4] for row in discharge) / len(discharge)
    if lit > 0.25:
        errors.append(f"screen on in {lit:.0%} of discharge samples")
    return errors


def summarise(rows: list[Row]) -> str:
    span_h = (rows[-1][0] - rows[0][0]) / 3600.0
    discharge = [row for row in rows if row[3] == 0]
    deltas = [now[7] - before[7]
              for before, now in zip(discharge, discharge[1:])]
    rate = median(deltas) if deltas else 0
    lit = sum(row[4] for row in discharge) / max(1, len(discharge))
    return (f"{len(rows)} rows, {span_h:.1f} h, "
            f"{rows[0][1]}% -> {rows[-1][1]}%, "
            f"{rate:g} sleeps/min, screen {lit:.0%}, "
            f"{rows[-1][7]} total sleep entries")


def selftest() -> int:
    """Prove the gate fails on the four ways a green log can lie."""
    base: list[Row] = []
    for minute in range(30):
        base.append((1795000000 + minute * 60, 99 - minute // 3, 4100,
                     0, 0, minute, 1, minute * 20))
    stuck = [row[:7] + (0,) for row in base]
    reset = []
    for index, row in enumerate(base):
        sleep = 0 if index >= 20 else row[7]
        reset.append(row[:7] + (sleep,))
    lit = [row[:4] + (1,) + row[5:] for row in base]
    gap = []
    for index, row in enumerate(base):
        epoch = row[0] + 300 if index >= 20 else row[0]
        gap.append((epoch,) + row[1:])
    cases: list[tuple[str, list[Row], bool]] = [
        ("good log", base, True),
        ("sleep counter stuck at zero", stuck, False),
        ("sleep counter reset mid-run", reset, False),
        ("screen left on", lit, False),
        ("a five-minute gap in the samples", gap, False),
    ]
    failures = 0
    for name, rows, should_pass in cases:
        errors = check_rows(rows, 5)
        passed = not errors
        if passed != should_pass:
            print(f"FAIL {name}: {'passed' if passed else errors}")
            failures += 1
        else:
            print(f"OK   {name}")
    return 1 if failures else 0


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", help="captured power.log")
    parser.add_argument("--min-sleep-per-min", type=int, default=5,
                        help="minimum median light-sleep entries per minute")
    parser.add_argument("--min-rows", type=int, default=10,
                        help="shortest discharge run that can be graded")
    parser.add_argument("--allow-gaps", action="store_true",
                        help="tolerate gaps between runs on a multi-night card")
    parser.add_argument("--selftest", action="store_true",
                        help="check that the gate rejects known-bad logs")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.path:
        parser.print_help()
        return 2
    try:
        with open(args.path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        print(f"cannot read {args.path}: {error}", file=sys.stderr)
        return 2

    rows, errors = parse(text)
    errors += check_rows(rows, args.min_sleep_per_min, args.min_rows,
                         args.allow_gaps)
    if rows:
        print(summarise(rows))
    for error in errors:
        print(f"FAIL {error}")
    if errors:
        print(f"power log: {len(errors)} failure(s)")
        return 1
    print("power log: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
