#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
MosuTime power budget.

This is a first-order MODEL, not a measurement.  Every current in it is an
assumption until somebody puts a PPK2 on the watch; docs/power.md explains how
to replace these numbers with real ones.  The point of the script is to show
which term matters and how the runtime moves when a config value changes.

Usage
-----
    python tools/power_budget.py
    python tools/power_budget.py --capacity 500
    python tools/power_budget.py --capacity 300 --capacity 500
    python tools/power_budget.py --wakes 40 --raises 120 --announcements 12
    python tools/power_budget.py --sleep-ma 0.4 --touch-ma 1.2 --screen-ma 31

Defaults for brightness, screen_on_ms, motion_poll_hz and sync_hours are read
from sd_card/mosu.cfg when that file exists, so the model follows the shipped
configuration.  Command-line flags always win.

The model
---------
    I_avg = I_sleep + I_touch
          + motion_poll_hz * (imu_sample_ms / 1000) * I_imu
          + (wakes + raises) * screen_on_s * I_screen / 86400
          + announcements * speak_s * I_audio / 86400
          + (24 / sync_hours) * wifi_s * I_wifi / 86400

    days = capacity_mAh / I_avg / 24

The screen term assumes every wake keeps the panel on for screen_on_s; the
boot face is counted as one of the wakes.  The IMU burst is added on top of the
sleep baseline, which makes the estimate slightly conservative.  The model
does not scale the panel current with brightness or with the number of lit
pixels: measure the brightness you actually ship.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

SECONDS_PER_DAY = 86400.0
REPO_ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = REPO_ROOT / "sd_card" / "mosu.cfg"

# Values used when mosu.cfg is missing or a key is unreadable.  These match
# mosu_config_defaults() in components/mosu_config/src/mosu_config.c.
CONFIG_DEFAULTS = {
    "brightness": 55.0,
    "screen_on_ms": 11250.0,
    "motion_poll_hz": 8.0,
    "sync_hours": 12.0,
}

# The firmware clamps these in mosu_config_sanitize(); mirror the limits so a
# hand-edited file cannot make the model nonsense.
CONFIG_LIMITS = {
    "brightness": (1.0, 100.0),
    "screen_on_ms": (1000.0, 300000.0),
    "motion_poll_hz": (1.0, 20.0),
    "sync_hours": (1.0, 168.0),
}


def _strip_comment(line: str) -> str:
    """Strip ' # ...' and ' ; ...', keeping '#RRGGBB' values intact."""
    in_quote = False
    quote = ""
    for index, ch in enumerate(line):
        if in_quote:
            if ch == quote:
                in_quote = False
            continue
        if ch in ("'", '"'):
            in_quote = True
            quote = ch
            continue
        if ch in ("#", ";"):
            at_start = index == 0
            spaced = (
                index > 0
                and line[index - 1].isspace()
                and (index + 1 == len(line) or line[index + 1].isspace())
            )
            if at_start or spaced:
                return line[:index]
    return line


def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def read_config(path: Path = CONFIG_PATH) -> tuple[dict, str]:
    """Return (values, source text) from mosu.cfg, or built-in defaults."""
    values = dict(CONFIG_DEFAULTS)
    if not path.is_file():
        return values, "built-in defaults (sd_card/mosu.cfg not found)"

    try:
        # utf-8-sig tolerates a UTF-8 BOM, like the firmware parser does.
        text = path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as exc:
        return values, f"built-in defaults ({exc})"

    section = ""
    for raw in text.splitlines():
        line = _strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("["):
            close = line.find("]")
            if close != -1:
                section = line[1:close].strip().lower()
            continue
        if "=" not in line:
            continue

        key, value = line.split("=", 1)
        key = key.strip().lower()
        value = _unquote(value)

        full_key = f"{section}.{key}" if section else key
        if full_key not in ("display.brightness", "display.screen_on_ms",
                            "display.motion_poll_hz", "clock.sync_hours"):
            continue

        name = key
        try:
            number = float(value)
        except ValueError:
            continue
        if not math.isfinite(number):
            continue

        low, high = CONFIG_LIMITS[name]
        values[name] = min(max(number, low), high)

    source_text = str(path)
    try:
        source_text = str(path.relative_to(REPO_ROOT))
    except ValueError:
        pass
    return values, source_text


def positive(value: str) -> float:
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {value}")
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a positive number")
    return number


def non_negative(value: str) -> float:
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {value}")
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be zero or more")
    return number


def build_parser(cfg: dict, source: str) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="First-order MosuTime power budget. All output is an "
                    "estimate, not a measurement.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Example: python tools/power_budget.py --capacity 300 "
            "--capacity 500 --wakes 40 --raises 120\n"
            f"Config values read from: {source}"
        ),
    )
    parser.add_argument("--capacity", type=positive, action="append",
                        metavar="MAH",
                        help="battery capacity in mAh, repeatable "
                             "(default: 300)")
    parser.add_argument("--wakes", type=non_negative, default=20.0,
                        metavar="N",
                        help="touch (and one boot) wakes per day, default 20")
    parser.add_argument("--raises", type=non_negative, default=60.0,
                        metavar="N",
                        help="wrist raises per day, default 60")
    parser.add_argument("--announcements", type=non_negative, default=8.0,
                        metavar="N",
                        help="spoken announcements per day, default 8")

    parser.add_argument("--screen-on-s", type=positive, default=None,
                        metavar="S",
                        help="seconds the panel stays on per wake "
                             f"(default: from config, {cfg['screen_on_ms'] / 1000:g} s)")
    parser.add_argument("--brightness", type=non_negative,
                        default=cfg["brightness"], metavar="PCT",
                        help="brightness shown in the report; it does not "
                             "scale --screen-ma "
                             f"(default: from config, {cfg['brightness']:g})")
    parser.add_argument("--motion-poll-hz", type=positive,
                        default=cfg["motion_poll_hz"], metavar="HZ",
                        help="IMU polls per second while the screen is off "
                             f"(default: from config, {cfg['motion_poll_hz']:g})")
    parser.add_argument("--sync-hours", type=positive,
                        default=cfg["sync_hours"], metavar="H",
                        help="hours between Wi-Fi syncs "
                             f"(default: from config, {cfg['sync_hours']:g})")

    parser.add_argument("--sleep-ma", type=non_negative, default=0.6,
                        metavar="MA",
                        help="light-sleep baseline, default 0.6")
    parser.add_argument("--touch-ma", type=non_negative, default=1.5,
                        metavar="MA",
                        help="touch controller idle current, default 1.5")
    parser.add_argument("--imu-sample-ms", type=non_negative, default=1.5,
                        metavar="MS",
                        help="length of one IMU sample burst, default 1.5")
    parser.add_argument("--imu-sample-ma", type=non_negative, default=40.0,
                        metavar="MA",
                        help="current during the IMU burst, default 40")
    parser.add_argument("--screen-ma", type=non_negative, default=35.0,
                        metavar="MA",
                        help="panel + CPU while the face is lit, default 35")
    parser.add_argument("--speak-s", type=non_negative, default=6.0,
                        metavar="S",
                        help="seconds of audio per announcement, default 6")
    parser.add_argument("--audio-ma", type=non_negative, default=120.0,
                        metavar="MA",
                        help="current while speaking, default 120")
    parser.add_argument("--wifi-ma", type=non_negative, default=120.0,
                        metavar="MA",
                        help="current during a Wi-Fi sync, default 120")
    parser.add_argument("--wifi-s", type=non_negative, default=8.0,
                        metavar="S",
                        help="radio-on seconds per successful sync, default 8")
    return parser


def estimate(p: dict) -> dict:
    idle = p["sleep_ma"] + p["touch_ma"]
    imu = (p["motion_poll_hz"] * (p["imu_sample_ms"] / 1000.0)
           * p["imu_sample_ma"])
    screen_s = (p["wakes"] + p["raises"]) * p["screen_on_s"]
    screen = screen_s * p["screen_ma"] / SECONDS_PER_DAY
    audio = (p["announcements"] * p["speak_s"] * p["audio_ma"]
             / SECONDS_PER_DAY)
    wifi = ((24.0 / p["sync_hours"]) * p["wifi_s"] * p["wifi_ma"]
            / SECONDS_PER_DAY)
    total = idle + imu + screen + audio + wifi
    return {
        "idle": idle,
        "imu": imu,
        "screen": screen,
        "audio": audio,
        "wifi": wifi,
        "total": total,
        "screen_s": screen_s,
    }


def days(capacity_mah: float, average_ma: float) -> float:
    if average_ma <= 0:
        return float("inf")
    return capacity_mah / average_ma / 24.0


def fmt_days(capacity_mah: float, average_ma: float) -> str:
    value = days(capacity_mah, average_ma)
    if math.isinf(value):
        return "infinite"
    if value >= 1000:
        return f"{value:,.0f}"
    return f"{value:.1f}"


def print_header(p: dict, caps: list[float], source: str) -> None:
    print("=" * 72)
    print("MosuTime power budget  -  ESTIMATES ONLY, NOT MEASUREMENTS")
    print("=" * 72)
    print("Every current below is an assumption until it is measured with a")
    print("PPK2.  See docs/power.md for the measurement procedure.")
    print()
    print(f"Config source: {source}")
    print()
    print("Inputs")
    rows = [
        ("light sleep baseline", f"{p['sleep_ma']:.2f} mA", "assumed"),
        ("touch controller idle", f"{p['touch_ma']:.2f} mA", "assumed"),
        ("IMU burst", f"{p['imu_sample_ma']:.2f} mA x "
                      f"{p['imu_sample_ms']:g} ms", "assumed"),
        ("motion_poll_hz", f"{p['motion_poll_hz']:g} Hz", ""),
        ("screen on", f"{p['screen_ma']:.2f} mA at brightness "
                      f"{p['brightness']:.0f}", "assumed"),
        ("screen_on_ms", f"{p['screen_on_s'] * 1000:.0f} ms "
                         f"({p['screen_on_s']:g} s)", ""),
        ("touch/boot wakes per day", f"{p['wakes']:.0f}", ""),
        ("wrist raises per day", f"{p['raises']:.0f}", ""),
        ("announcements per day", f"{p['announcements']:.0f}", ""),
        ("speaking", f"{p['audio_ma']:.2f} mA x {p['speak_s']:g} s",
         "assumed"),
        ("Wi-Fi sync", f"{p['wifi_ma']:.2f} mA x {p['wifi_s']:g} s",
         "assumed"),
        ("sync_hours", f"{p['sync_hours']:g} h", ""),
        ("battery capacity", ", ".join(f"{c:g} mAh" for c in caps), ""),
    ]
    for label, value, note in rows:
        suffix = f"  {note}" if note else ""
        print(f"  {label:<28}{value:>28}{suffix}")
    print()


def print_formula() -> None:
    print("Formula")
    print("  I_avg = I_sleep + I_touch")
    print("        + motion_poll_hz * (imu_sample_ms / 1000) * I_imu")
    print("        + (wakes + raises) * screen_on_s * I_screen / 86400")
    print("        + announcements * speak_s * I_audio / 86400")
    print("        + (24 / sync_hours) * wifi_s * I_wifi / 86400")
    print("  days  = capacity_mAh / I_avg / 24")
    print()


def print_result(p: dict, result: dict, caps: list[float]) -> None:
    total = result["total"]
    print(f"Average current: {total:.2f} mA  (ESTIMATE)")
    if total > 0:
        print(f"Daily charge use: {total * 24.0:.1f} mAh/day  (ESTIMATE)")
    else:
        print("Daily charge use: 0.0 mAh/day")
    print()
    print("Battery life (ESTIMATE: capacity / average current)")
    for capacity in caps:
        print(f"  {capacity:6g} mAh -> {fmt_days(capacity, total):>8} days")
    print()

    print("Breakdown at this duty cycle")
    print(f"  {'component':<24} {'mA':>8} {'mAh/day':>9} {'share':>7}")
    rows = [
        ("sleep + touch idle", result["idle"]),
        ("IMU polling", result["imu"]),
        ("screen-on windows", result["screen"]),
        ("speaking", result["audio"]),
        ("Wi-Fi sync", result["wifi"]),
    ]
    for label, value in rows:
        share = (value / total * 100.0) if total > 0 else 0.0
        print(f"  {label:<24} {value:8.2f} {value * 24.0:9.1f} {share:6.1f}%")
    print()


def print_sensitivity(p: dict, caps: list[float]) -> None:
    base = estimate(p)
    print("Sensitivity  (every current here is still an ESTIMATE)")
    print()

    header = f"  {'value':>8}  {'avg mA':>8}"
    for capacity in caps:
        label = f"{capacity:g} mAh days"
        header += f"  {label:>15}"
    rule = "  " + "-" * 8 + "  " + "-" * 8
    for _ in caps:
        rule += "  " + "-" * 15

    def table(title: str, values: list[float], key: str) -> None:
        print(title)
        print(header)
        print(rule)
        base_value = p[key]
        for value in values:
            probe = dict(p)
            probe[key] = value
            row = estimate(probe)
            mark = "*" if abs(value - base_value) < 1e-6 else " "
            line = f"  {value:8g}  {row['total']:8.2f}"
            for capacity in caps:
                line += f"  {fmt_days(capacity, row['total']):>15}"
            print(line + (f" {mark}" if mark.strip() else ""))
        print()

    table("Sensitivity: motion_poll_hz (everything else fixed)",
          [1.0, 4.0, 8.0, 16.0], "motion_poll_hz")
    table("Sensitivity: screen_on_s (everything else fixed)",
          [5.0, 10.0, 15.0, 30.0], "screen_on_s")
    table("Sensitivity: sync_hours (everything else fixed)",
          [3.0, 6.0, 12.0, 24.0], "sync_hours")

    print(f"  * = the value used for the {base['total']:.2f} mA result above")
    print()


def print_notes() -> None:
    print("Notes")
    print("  - All currents are ESTIMATES.  Replace them with PPK2 measurements")
    print("    (docs/power.md) before promising a runtime to anyone.")
    print("  - Capacity is nominal mAh, not derated for age, cold or the")
    print("    4.2 V charge limit used by the AXP2101.")
    print("  - The IMU burst is counted on top of the sleep baseline, so the")
    print("    estimate is slightly conservative.")
    print("  - Panel current does not scale with brightness or lit area here;")
    print("    measure the brightness you actually ship.")
    print("  - Every touch/boot wake and wrist raise is assumed to start a full")
    print("    screen_on_s window; overlapping wakes are not merged, so heavy")
    print("    use is overestimated.")
    print("  - A failed sync can take up to 15 s per configured network plus")
    print("    the geolocation and SNTP timeouts; this model assumes successes.")
    print("  - The console USB cable charges the battery.  Measurements must")
    print("    be taken with VBUS disconnected (see docs/power.md).")
    print()


def main(argv: list[str] | None = None) -> int:
    cfg, source = read_config()
    parser = build_parser(cfg, source)
    args = parser.parse_args(argv)

    caps = args.capacity or [300.0]
    # Keep the order the user gave, drop exact repeats.
    unique_caps: list[float] = []
    for capacity in caps:
        if capacity not in unique_caps:
            unique_caps.append(capacity)
    caps = unique_caps

    screen_on_s = (args.screen_on_s if args.screen_on_s is not None
                   else cfg["screen_on_ms"] / 1000.0)

    p = {
        "sleep_ma": args.sleep_ma,
        "touch_ma": args.touch_ma,
        "imu_sample_ms": args.imu_sample_ms,
        "imu_sample_ma": args.imu_sample_ma,
        "motion_poll_hz": args.motion_poll_hz,
        "brightness": args.brightness,
        "screen_ma": args.screen_ma,
        "screen_on_s": screen_on_s,
        "wakes": args.wakes,
        "raises": args.raises,
        "announcements": args.announcements,
        "speak_s": args.speak_s,
        "audio_ma": args.audio_ma,
        "wifi_ma": args.wifi_ma,
        "wifi_s": args.wifi_s,
        "sync_hours": args.sync_hours,
    }

    for name in ("screen_on_s", "motion_poll_hz", "sync_hours"):
        if p[name] <= 0:
            parser.error(f"--{name.replace('_', '-')} must be greater than zero")

    result = estimate(p)
    print_header(p, caps, source)
    print_formula()
    print_result(p, result, caps)
    print_sensitivity(p, caps)
    print_notes()
    return 0


if __name__ == "__main__":
    sys.exit(main())
