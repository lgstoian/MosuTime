#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Rebuild the AMOLED framebuffer from an esp-emu co-simulation log.

The emulator records every SPI transfer that reaches a part.  The panel part
in tools/emu/panel claims the CO5300's chip select, so emu-events.jsonl holds
the exact byte stream the firmware sent to the panel:

    opcode transfer   32 00 2c 00   (write colour, 0x32 = LCD_OPCODE_WRITE_COLOR)
    data transfers    RGB565, big endian, in window order

Window commands (0x2a CASET / 0x2b RASET) arrive as a four byte opcode
transfer followed by a four byte parameter transfer.  The panel's own x
offset (22) is the set_gap() value from mosu_board_display.c.

Usage:
    python tools/emu/reconstruct.py emu-events.jsonl out.png [--window N]

Without --window the last complete frame is written.  With --window N the
N-th colour flush (0 based) is written instead, which is handy to look at the
first frame even when later ones are partial updates.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys

LCD_W = 410
LCD_H = 502
X_GAP = 22

WRITE_CMD = 0x02
WRITE_COLOR = 0x32

# PNG writer without Pillow: a single RGB image, no filtering.
import zlib


def write_png(path: str, pixels: bytearray, width: int, height: int) -> None:
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw.extend(pixels[y * stride : (y + 1) * stride])

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def rgb565(raw: bytes) -> tuple[int, int, int]:
    value = (raw[0] << 8) | raw[1]
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def transfers(path: str):
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            msg = event.get("msg", {})
            request = msg.get("BusRequest")
            if not request:
                continue
            op = request.get("op", {}).get("Spi")
            if not op:
                continue
            yield bytes(op.get("tx", []))


class Frame:
    def __init__(self) -> None:
        self.rgb = bytearray(LCD_W * LCD_H * 3)
        self.window = None  # (x0, y0, x1, y1), absolute panel coordinates
        self.remaining = 0  # bytes still expected for this window
        self.cursor_x = 0
        self.cursor_y = 0
        self.complete = False

    def start_window(self) -> None:
        if self.window:
            x0, y0, x1, y1 = self.window
            self.remaining = (x1 - x0 + 1) * (y1 - y0 + 1) * 2
            self.cursor_x, self.cursor_y = x0, y0
        self.complete = False

    def put(self, data: bytes) -> None:
        i = 0
        while i + 1 < len(data) and self.remaining > 0:
            if not self.window:
                return
            x0, y0, _, y1 = self.window
            x1 = self.window[2]
            if 0 <= self.cursor_x - X_GAP < LCD_W and 0 <= self.cursor_y < LCD_H:
                r, g, b = rgb565(data[i : i + 2])
                offset = (self.cursor_y * LCD_W + (self.cursor_x - X_GAP)) * 3
                self.rgb[offset : offset + 3] = bytes((r, g, b))
            i += 2
            self.remaining -= 2
            self.cursor_x += 1
            if self.cursor_x > x1:
                self.cursor_x = x0
                self.cursor_y += 1
                if self.cursor_y > y1:
                    self.cursor_y = y1
        if self.remaining == 0:
            self.complete = True


def parse(path: str, wanted_window: int | None) -> tuple[Frame, int]:
    frame = Frame()
    pending_cmd = None
    windows = 0
    last_complete = None

    for tx in transfers(path):
        # A four byte opcode transfer is one command or a colour start.
        if (
            len(tx) == 4
            and tx[0] in (WRITE_CMD, WRITE_COLOR)
            and tx[3] == 0x00
            and tx[2] != 0x00
        ):
            kind, cmd = tx[0], tx[2]
            if kind == WRITE_COLOR and cmd == 0x2C:
                frame.start_window()
                windows += 1
                pending_cmd = None
                continue
            pending_cmd = (kind, cmd, b"")
            # A command with no parameters is complete right here.
            if kind == WRITE_CMD and cmd not in (0x2A, 0x2B):
                pending_cmd = None
            continue

        if pending_cmd is not None:
            kind, cmd, _ = pending_cmd
            if len(tx) <= 16:
                if cmd == 0x2A and len(tx) >= 4:
                    x0 = (tx[0] << 8) | tx[1]
                    x1 = (tx[2] << 8) | tx[3]
                    y0, y1 = (frame.window[1], frame.window[3]) if frame.window else (0, 0)
                    frame.window = (x0, y0, x1, y1)
                elif cmd == 0x2B and len(tx) >= 4:
                    y0 = (tx[0] << 8) | tx[1]
                    y1 = (tx[2] << 8) | tx[3]
                    x0, x1 = (frame.window[0], frame.window[2]) if frame.window else (0, 0)
                    frame.window = (x0, y0, x1, y1)
                pending_cmd = None
                continue

        if frame.remaining > 0:
            frame.put(tx)
            if frame.complete:
                last_complete = Frame()
                last_complete.rgb = bytearray(frame.rgb)
                last_complete.complete = True
                if wanted_window is not None and windows - 1 == wanted_window:
                    return last_complete, windows

    if last_complete is not None:
        return last_complete, windows

    return frame, windows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="emu-events.jsonl from --cosim-log")
    parser.add_argument("output", help="PNG to write")
    parser.add_argument("--window", type=int, default=None,
                        help="0-based colour window to render")
    args = parser.parse_args()

    frame, windows = parse(args.log, args.window)
    if windows == 0:
        print("no colour windows in the log", file=sys.stderr)
        return 1
    write_png(args.output, frame.rgb, LCD_W, LCD_H)
    print(f"{windows} colour windows, wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
