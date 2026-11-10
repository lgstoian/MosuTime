#!/usr/bin/env python3
"""Build the accessibility contact sheet for the MosuTime watch face.

The script drives the desktop simulator once per time of day, reads the BMPs
back with the standard library only, and composes a labelled grid next to a
README that records the physical-size math for the 2.06" panel.

Run from anywhere inside the repository:

    python tools/contact_sheet.py
    python tools/contact_sheet.py --sim path/to/mosu_sim

No third-party packages are used on purpose: the sheet has to be rebuildable
on a bare Windows machine and on the CI runner alike.
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import textwrap
import zlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS_DIR = os.path.join(REPO_ROOT, "docs", "accessibility")
SHEET_PATH = os.path.join(DOCS_DIR, "hours.png")
README_PATH = os.path.join(DOCS_DIR, "README.md")

PANEL_W = 410
PANEL_H = 502
PANEL_DIAGONAL_IN = 2.06
DIGIT_HEIGHT_PX = 200
DIGIT_STROKE_PX = 22
DIGIT_GAP_PX = 22

# hour, minute, battery %, wifi (0 ok / 1 idle / 2 fail), slash, valid
SCENES = [
    (0, 0, 82, 0, 0, 1),
    (1, 5, 74, 1, 0, 1),
    (2, 30, 61, 0, 0, 1),
    (4, 56, 48, 2, 1, 1),
    (7, 45, 90, 0, 0, 1),
    (8, 0, 55, 1, 0, 1),
    (9, 41, 78, 0, 0, 1),
    (10, 23, 66, 1, 0, 1),
    (11, 59, 39, 2, 0, 1),
    (12, 0, 100, 0, 0, 1),
    (13, 37, 27, 1, 0, 1),
    (15, 45, 12, 2, 1, 1),
    (18, 18, 71, 0, 0, 1),
    (21, 57, 32, 1, 0, 1),
    (22, 22, 84, 0, 0, 1),
    (23, 59, 9, 2, 1, 1),
]

# 5x7 bitmap font.  Uppercase only: the labels are short and shouted.
FONT = {
    " ": (".....",) * 7,
    "-": (".....", ".....", ".....", "#####", ".....", ".....", "....."),
    ".": (".....", ".....", ".....", ".....", ".....", ".##..", ".##.."),
    ":": (".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."),
    "0": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "1": ("..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "2": (".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"),
    "3": ("####.", "....#", "....#", ".###.", "....#", "....#", "####."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
    "5": ("#####", "#....", "####.", "....#", "....#", "#...#", ".###."),
    "6": (".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."),
    "7": ("#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."),
    "8": (".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."),
    "9": (".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."),
    "A": (".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "B": ("####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."),
    "C": (".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."),
    "D": ("####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."),
    "E": ("#####", "#....", "#....", "####.", "#....", "#....", "#####"),
    "F": ("#####", "#....", "#....", "####.", "#....", "#....", "#...."),
    "G": (".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."),
    "H": ("#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "I": (".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "J": ("..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
    "L": ("#....", "#....", "#....", "#....", "#....", "#....", "#####"),
    "M": ("#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"),
    "N": ("#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "P": ("####.", "#...#", "#...#", "####.", "#....", "#....", "#...."),
    "Q": (".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"),
    "R": ("####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
    "T": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "U": ("#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "V": ("#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "W": ("#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"),
    "X": ("#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"),
    "Y": ("#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."),
    "Z": ("#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"),
}

BG = (16, 18, 22)
CELL_BG = (26, 29, 35)
BORDER = (64, 72, 84)
LABEL = (232, 236, 242)
HEADER = (255, 255, 255)
ACCENT = (255, 201, 138)


def default_sim_path() -> str:
    name = "mosu_sim.exe" if os.name == "nt" else "mosu_sim"
    return os.path.join(REPO_ROOT, "sim", "build", name)


def read_bmp(path: str):
    """Return (width, height, RGB888 top-down bytearray) for a 24-bit BMP."""
    with open(path, "rb") as handle:
        data = handle.read()

    if len(data) < 54 or data[0:2] != b"BM":
        raise ValueError("%s: not a BMP file" % path)

    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError("%s: unsupported BMP header (%d bytes)" % (path, dib_size))

    width, height = struct.unpack_from("<ii", data, 18)
    planes, bits = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if planes != 1 or bits != 24 or compression != 0:
        raise ValueError(
            "%s: expected an uncompressed 24-bit BMP, got %d bpp / compression %d"
            % (path, bits, compression)
        )
    if width <= 0 or height == 0:
        raise ValueError("%s: bad dimensions %dx%d" % (path, width, height))

    top_down = height < 0
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    pixels = bytearray(width * height * 3)

    for y in range(height):
        src_row = y if top_down else height - 1 - y
        start = offset + src_row * stride
        line = data[start : start + width * 3]
        if len(line) != width * 3:
            raise ValueError("%s: truncated pixel data" % path)
        dst = y * width * 3
        pixels[dst : dst + width * 3 : 3] = line[2::3]
        pixels[dst + 1 : dst + width * 3 : 3] = line[1::3]
        pixels[dst + 2 : dst + width * 3 : 3] = line[0::3]

    return width, height, pixels


def write_bmp(path: str, width: int, height: int, pixels: bytes) -> None:
    """Write RGB888 top-down pixels as an uncompressed 24-bit BMP."""
    stride = (width * 3 + 3) & ~3
    padding = b"\x00" * (stride - width * 3)
    body = bytearray()

    for y in range(height - 1, -1, -1):
        start = y * width * 3
        row = bytearray(width * 3)
        row[0::3] = pixels[start + 2 : start + width * 3 : 3]
        row[1::3] = pixels[start + 1 : start + width * 3 : 3]
        row[2::3] = pixels[start : start + width * 3 : 3]
        body += row
        body += padding

    file_size = 54 + len(body)
    header = bytearray(54)
    header[0:2] = b"BM"
    struct.pack_into("<I", header, 2, file_size)
    struct.pack_into("<I", header, 10, 54)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<i", header, 18, width)
    struct.pack_into("<i", header, 22, height)
    struct.pack_into("<H", header, 26, 1)
    struct.pack_into("<H", header, 28, 24)
    struct.pack_into("<I", header, 30, 0)
    struct.pack_into("<I", header, 34, len(body))
    struct.pack_into("<i", header, 38, 2835)  # 72 dpi
    struct.pack_into("<i", header, 42, 2835)
    struct.pack_into("<I", header, 46, 0)
    struct.pack_into("<I", header, 50, 0)

    if file_size != len(header) + len(body):
        raise ValueError("BMP size mismatch")

    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(body)


def write_png(path: str, width: int, height: int, pixels: bytes) -> None:
    """Write RGB888 top-down pixels as a PNG, standard library only."""
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

    body = b"\x89PNG\r\n\x1a\n"
    body += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    body += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    body += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(body)


def write_sheet(path: str, width: int, height: int, pixels: bytes) -> None:
    if path.lower().endswith(".bmp"):
        write_bmp(path, width, height, pixels)
    else:
        write_png(path, width, height, pixels)


def blend_rect(pixels: bytearray, width: int, height: int, x: int, y: int,
               w: int, h: int, color) -> None:
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(width, x + w)
    y1 = min(height, y + h)
    if x0 >= x1 or y0 >= y1:
        return
    row = bytes(color) * (x1 - x0)
    for yy in range(y0, y1):
        start = (yy * width + x0) * 3
        pixels[start : start + len(row)] = row


def outline(pixels: bytearray, width: int, height: int, x: int, y: int,
            w: int, h: int, color, thickness: int = 1) -> None:
    blend_rect(pixels, width, height, x, y, w, thickness, color)
    blend_rect(pixels, width, height, x, y + h - thickness, w, thickness, color)
    blend_rect(pixels, width, height, x, y, thickness, h, color)
    blend_rect(pixels, width, height, x + w - thickness, y, thickness, h, color)


def text_width(text: str, scale: int) -> int:
    return (len(text) * 6 - 1) * scale


def draw_text(pixels: bytearray, width: int, height: int, x: int, y: int,
              text: str, scale: int, color) -> None:
    """Draw text with the built-in 5x7 font, one glyph every 6 cells."""
    cursor = x
    for char in text.upper():
        glyph = FONT.get(char, FONT[" "])
        for row, line in enumerate(glyph):
            for col, ink in enumerate(line):
                if ink == "#":
                    blend_rect(
                        pixels,
                        width,
                        height,
                        cursor + col * scale,
                        y + row * scale,
                        scale,
                        scale,
                        color,
                    )
        cursor += 6 * scale


def blit(dst: bytearray, dw: int, dh: int, src: bytes, sw: int, sh: int,
         x: int, y: int) -> None:
    for row in range(sh):
        dy = y + row
        if dy < 0 or dy >= dh:
            continue
        src_start = row * sw * 3
        dst_start = (dy * dw + x) * 3
        dst[dst_start : dst_start + sw * 3] = src[src_start : src_start + sw * 3]


def render_scene(sim: str, out_path: str, scene) -> None:
    hour, minute, battery, wifi, slash, valid = scene
    command = [
        sim,
        "--render",
        out_path,
        str(hour),
        str(minute),
        str(battery),
        str(wifi),
        str(slash),
        str(valid),
    ]
    subprocess.run(
        command, check=True, cwd=REPO_ROOT, stdout=subprocess.DEVNULL
    )


def build_sheet(sim: str, sheet_path: str, frames_dir: str | None = None,
                columns: int = 4) -> tuple[int, int]:
    margin = 12
    gap = 10
    label_h = 44
    header_h = 64
    cell_w = PANEL_W
    cell_h = PANEL_H + label_h

    rows = (len(SCENES) + columns - 1) // columns
    sheet_w = margin * 2 + columns * cell_w + (columns - 1) * gap
    sheet_h = margin * 2 + header_h + rows * cell_h + (rows - 1) * gap

    pixels = bytearray(bytes(BG) * (sheet_w * sheet_h))

    title = "MOSUTIME 410 X 502 PX  -  HOURS AND DIGITS"
    title_scale = 4
    while text_width(title, title_scale) > sheet_w - 2 * margin and title_scale > 1:
        title_scale -= 1
    draw_text(
        pixels,
        sheet_w,
        sheet_h,
        (sheet_w - text_width(title, title_scale)) // 2,
        margin + 8,
        title,
        title_scale,
        HEADER,
    )
    rule_y = margin + header_h - 14
    blend_rect(pixels, sheet_w, sheet_h, margin, rule_y, sheet_w - 2 * margin, 2,
               ACCENT)

    if frames_dir:
        os.makedirs(frames_dir, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mosu-sheet-") as temp_dir:
        for index, scene in enumerate(SCENES):
            hour, minute, battery, wifi, slash, valid = scene
            frame_name = "%02d_%02d_%d_%d_%d_%d.bmp" % (
                hour,
                minute,
                battery,
                wifi,
                slash,
                valid,
            )
            frame_path = os.path.join(temp_dir, frame_name)
            render_scene(sim, frame_path, scene)

            face_w, face_h, face = read_bmp(frame_path)
            if (face_w, face_h) != (PANEL_W, PANEL_H):
                raise ValueError(
                    "%s: expected %dx%d, got %dx%d"
                    % (frame_path, PANEL_W, PANEL_H, face_w, face_h)
                )
            if frames_dir:
                with open(frame_path, "rb") as src, open(
                    os.path.join(frames_dir, frame_name), "wb"
                ) as dst:
                    dst.write(src.read())

            col = index % columns
            row = index // columns
            cell_x = margin + col * (cell_w + gap)
            cell_y = margin + header_h + row * (cell_h + gap)

            blend_rect(pixels, sheet_w, sheet_h, cell_x, cell_y, cell_w, cell_h,
                       CELL_BG)
            blit(pixels, sheet_w, sheet_h, face, face_w, face_h, cell_x, cell_y)
            outline(pixels, sheet_w, sheet_h, cell_x - 1, cell_y - 1,
                    cell_w + 2, face_h + 2, BORDER)

            suffix = "PM" if hour >= 12 else "AM"
            label = "%02d:%02d %s" % (hour, minute, suffix)
            label_scale = 3
            label_y = cell_y + face_h + (label_h - 7 * label_scale) // 2
            draw_text(
                pixels,
                sheet_w,
                sheet_h,
                cell_x + (cell_w - text_width(label, label_scale)) // 2,
                label_y,
                label,
                label_scale,
                LABEL,
            )

    write_sheet(sheet_path, sheet_w, sheet_h, bytes(pixels))
    return sheet_w, sheet_h


def write_readme(path: str, sheet_w: int, sheet_h: int) -> None:
    diagonal_mm = PANEL_DIAGONAL_IN * 25.4
    diagonal_px = math.hypot(PANEL_W, PANEL_H)
    pitch = diagonal_mm / diagonal_px
    panel_w_mm = PANEL_W * pitch
    panel_h_mm = PANEL_H * pitch
    digits_mm = DIGIT_HEIGHT_PX * pitch
    stroke_mm = DIGIT_STROKE_PX * pitch
    gap_mm = DIGIT_GAP_PX * pitch
    ppi = 25.4 / pitch

    def angle_deg(size_mm: float, distance_mm: float) -> float:
        return 2.0 * math.degrees(math.atan(size_mm / (2.0 * distance_mm)))

    def wrap(text: str) -> str:
        return textwrap.fill(" ".join(text.split()), width=78) + "\n\n"

    def bullet_list(items) -> str:
        out = ""
        for item in items:
            out += (
                textwrap.fill(
                    item, width=78, initial_indent="- ", subsequent_indent="  "
                )
                + "\n"
            )
        return out + "\n"

    parts = [
        "# Accessibility renders\n\n",
        wrap(
            "Generated by `tools/contact_sheet.py`; do not edit this file or "
            "`hours.png` by hand. Rebuild both from the repository root with:"
        ),
        "```sh\npython tools/contact_sheet.py\n```\n\n",
        wrap(
            f"`hours.png` is a {sheet_w}x{sheet_h} contact sheet: "
            f"sixteen native {PANEL_W}x{PANEL_H} renders, four per row, each "
            "labelled with the 24 h time it shows. Between them the scenes "
            "cover every clock digit 0-9, the low and the high end of the "
            "battery, all three Wi-Fi states and both the AM and PM themes."
        ),
        "## What is being probed\n\n",
        wrap(
            "The numerals are drawn, not typeset: a seven-segment style "
            f"geometry of {DIGIT_HEIGHT_PX} px with a {DIGIT_STROKE_PX} px "
            f"stroke and a {DIGIT_GAP_PX} px gap between digits. The questions "
            "this sheet answers are physical, not aesthetic:"
        ),
        bullet_list(
            [
                "can a person with reduced contrast sensitivity still "
                "separate the strokes?",
                "is the AM/PM difference visible without relying on hue alone?",
                "does the status row stay secondary to the time?",
            ]
        ),
        "## Physical size math\n\n",
        bullet_list(
            [
                f"Display: {PANEL_W} x {PANEL_H} px on a "
                f"{PANEL_DIAGONAL_IN} inch (diagonal) panel.",
                f"Panel diagonal: {PANEL_DIAGONAL_IN} x 25.4 = "
                f"{diagonal_mm:.2f} mm.",
                f"Diagonal in pixels: sqrt({PANEL_W}^2 + {PANEL_H}^2) = "
                f"{diagonal_px:.1f} px.",
                f"Pixel pitch: {diagonal_mm:.2f} mm / {diagonal_px:.1f} px = "
                f"**{pitch:.4f} mm/px** (about {pitch * 1000:.0f} um per "
                f"pixel, roughly {ppi:.0f} ppi).",
                f"Active area: {PANEL_W} px x {pitch:.4f} mm/px = "
                f"**{panel_w_mm:.1f} mm wide** and {PANEL_H} px x "
                f"{pitch:.4f} mm/px = **{panel_h_mm:.1f} mm tall**.",
                f"Clock digit box: {DIGIT_HEIGHT_PX} px x {pitch:.4f} mm/px = "
                f"**{digits_mm:.1f} mm tall**.",
                f"Stroke: {DIGIT_STROKE_PX} px = **{stroke_mm:.1f} mm**; gap "
                f"between digits: {DIGIT_GAP_PX} px = **{gap_mm:.1f} mm**.",
            ]
        ),
        wrap(
            "For scale: a credit card is 0.76 mm thick, so a "
            f"{digits_mm:.1f} mm numeral is about {digits_mm / 0.76:.1f} card "
            "thicknesses tall."
        ),
        "## Viewing distance\n\n",
        wrap(
            "The digits subtend "
            f"{angle_deg(digits_mm, 300):.1f} degrees at 30 cm, "
            f"{angle_deg(digits_mm, 400):.1f} degrees at 40 cm and "
            f"{angle_deg(digits_mm, 1000):.1f} degrees at 1 m. Normal acuity "
            "resolves about 1 arcminute, and comfortable reading starts "
            "around 10-15 arcminutes, so the time stays legible well beyond "
            "arm's length."
        ),
        bullet_list(
            [
                "25-35 cm: normal watch-checking distance, the intended "
                "viewing range.",
                "50 cm: still comfortable; this is where the 200 px digit "
                "height pays off.",
                "1 m: readable as a clock on a bedside table, not as text.",
                "2 m: the shapes are recognisable but individual digits start "
                "to blur; do not size the layout for this distance.",
            ]
        ),
        wrap(
            "Judge the sheet at the distance you actually use the watch, not "
            "at 100 % zoom on a monitor. A 410 px wide face shown at 410 "
            "screen pixels is roughly three times the physical size of the "
            "watch on a typical 96 ppi monitor."
        ),
        "## Cataract colour shift\n\n",
        wrap(
            "Age-related nuclear cataract yellows and browns the lens. The "
            "eye loses short-wavelength (blue) transmission first and "
            "scatters the remaining light, which changes what this face looks "
            "like in three ways:"
        ),
        bullet_list(
            [
                "**Blue and green lose contrast.** Blue pixels look darker "
                "and greyer, and low-saturation status colours can disappear "
                "into the black background. The sheet therefore keeps the AM "
                "face warm white and the PM face amber; both survive the "
                "yellow shift, unlike a blue/white pair.",
                "**Colour is never the only cue.** Battery state also changes "
                "the filled proportion of the icon, Wi-Fi failure adds a "
                "slash, and the minute line is always below the hour line. "
                "Someone who cannot tell red from green can still read the "
                "state from shape and position.",
                "**Scatter costs edge contrast.** Thin light strokes on black "
                f"bloom and halo. That is why the stroke is {stroke_mm:.1f} "
                f"mm instead of a hairline, and why there is {gap_mm:.1f} mm "
                "of black between digits rather than tight kerning.",
            ]
        ),
        wrap(
            "When reviewing, dim the room a little and step back: cataracts "
            "hurt most in low light with high-glare, thin, saturated-blue "
            "content. If a scene is still readable under those conditions it "
            "will be readable on a wrist."
        ),
    ]

    text = "".join(parts)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Render the MosuTime digit atlas into docs/accessibility."
    )
    parser.add_argument(
        "--sim",
        default=default_sim_path(),
        help="path to the built simulator (default: sim/build/mosu_sim[.exe])",
    )
    parser.add_argument(
        "--out",
        default=SHEET_PATH,
        help="output PNG or BMP (default: docs/accessibility/hours.png)",
    )
    parser.add_argument(
        "--frames",
        default=None,
        help="optional directory that keeps the individual scene BMPs",
    )
    args = parser.parse_args(argv)

    sim = os.path.abspath(os.path.expanduser(args.sim))
    if not os.path.isfile(sim):
        print("simulator not found: %s" % sim, file=sys.stderr)
        print(
            "build it first: cmake -S sim -B sim/build && cmake --build sim/build",
            file=sys.stderr,
        )
        return 1

    out_path = os.path.abspath(os.path.expanduser(args.out))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    width, height = build_sheet(sim, out_path, args.frames)
    readme = os.path.join(os.path.dirname(out_path), "README.md")
    write_readme(readme, width, height)

    print("wrote %s (%dx%d)" % (out_path, width, height))
    print("wrote %s" % readme)
    return 0


if __name__ == "__main__":
    sys.exit(main())
