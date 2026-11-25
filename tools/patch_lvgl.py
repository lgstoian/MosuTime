#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Apply the MosuTime 50 ms LVGL task wait floor to the managed component.

The upstream esp_lvgl_port 2.9.0 task waits as little as 1 ms between polls.
On this dual-core board that keeps core 1 awake and blocks automatic light
sleep.  The fix is tiny but essential, and `managed_components/` is ignored,
so it must be re-applied after every fresh dependency fetch instead of living
only in one developer's build tree.

Usage:
    python tools/patch_lvgl.py            # patch in place, idempotent
    python tools/patch_lvgl.py --selftest # prove the patch is still applicable
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = (ROOT / "managed_components" / "espressif__esp_lvgl_port" / "src" /
          "lvgl9" / "esp_lvgl_port.c")

UPSTREAM = (
    "        /* Wait for queue or timeout (sleep task) */\n"
    "        TickType_t wait = (pdMS_TO_TICKS(task_delay_ms) >= 1 ? "
    "pdMS_TO_TICKS(task_delay_ms) : 1);\n"
)

PATCHED = (
    "        /* Wait for queue or timeout (sleep task) */\n"
    "        /* MosuTime: a static clock face never needs a 1 ms LVGL tick.  The\n"
    "         * upstream code turns a 0 ms return from lv_timer_handler() into a\n"
    "         * 1 ms wait plus another 1 ms vTaskDelay below, which keeps core 1\n"
    "         * awake continuously and prevents light sleep on this dual-core\n"
    "         * board.  Enforce a 50 ms floor (20 Hz), still ample for the watch\n"
    "         * and its simple test patterns. */\n"
    "        int delay_ms = task_delay_ms;\n"
    "        if (delay_ms < 50) {\n"
    "            delay_ms = 50;\n"
    "        }\n"
    "        TickType_t wait = pdMS_TO_TICKS(delay_ms);\n"
)

MARKER = "MosuTime: a static clock face never needs a 1 ms LVGL tick."


def patch(text: str) -> tuple[str, str]:
    """Return (new text, action) for one source file."""
    if MARKER in text:
        return text, "already patched"
    if UPSTREAM not in text:
        raise ValueError("upstream wait block not found; component version changed")
    return text.replace(UPSTREAM, PATCHED, 1), "patched"


def selftest() -> int:
    before = "void f(void) {\n" + UPSTREAM + "    use(wait);\n}\n"
    try:
        after, action = patch(before)
        again, second = patch(after)
    except ValueError as error:
        print(f"FAIL {error}")
        return 1
    ok = (action == "patched" and second == "already patched" and
          "delay_ms < 50" in after and again == after and
          UPSTREAM not in after)
    print("selftest:", "OK" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true",
                        help="check that the patch applies and is idempotent")
    args = parser.parse_args()
    if args.selftest:
        return selftest()

    if not TARGET.is_file():
        print(f"{TARGET}: missing; run `idf.py reconfigure` once first",
              file=sys.stderr)
        return 2
    try:
        text = TARGET.read_text(encoding="utf-8")
        new, action = patch(text)
    except ValueError as error:
        print(f"{TARGET}: {error}", file=sys.stderr)
        return 1
    if action == "patched":
        TARGET.write_text(new, encoding="utf-8", newline="")
    print(f"{TARGET}: {action}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
