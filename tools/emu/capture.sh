#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# Capture the three watch-face states from a running emulator.
#
# Prerequisites: see README.md in this folder.  Run from the repository root or
# anywhere else; paths are resolved from this script.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

EMU="${ESP_EMU:-esp-emu}"
FW="${FW:-$ROOT/build-emu/merged-binary.bin}"
OUT="$ROOT/docs/emulator"
PART="$HERE/panel/target/wasm32-unknown-unknown/release/mosu_panel.wasm"

if ! command -v "$EMU" >/dev/null 2>&1; then
    echo "esp-emu not found; set ESP_EMU=/path/to/esp-emu" >&2
    exit 1
fi
if [ ! -f "$FW" ]; then
    echo "firmware not found: $FW (build build-emu first)" >&2
    exit 1
fi
if [ ! -f "$PART" ]; then
    echo "panel part not built: $PART" >&2
    echo "  cd tools/emu/panel && ESP_COSIM_PROTO_DIR=... cargo build --release --target wasm32-unknown-unknown" >&2
    exit 1
fi

mkdir -p "$OUT"

# run_case NAME BOARD [extra emulator args...]
run_case() {
    local name="$1"
    local board="$2"
    shift 2

    local log="$HERE/emu-$name.jsonl"
    local uart="$HERE/emu-$name.log"

    echo "== $name"
    "$EMU" --chip esp32s3 \
        --firmware "$FW" \
        --psram-size 8M \
        --console uart0 \
        --timeout 8s \
        --cosim-board "$board" \
        --cosim-log "$log" \
        --cosim-log-filter BusRequest,Register,RegisterAck \
        "$@" > "$uart" 2>&1 || true

    python3 "$HERE/reconstruct.py" "$log" "$OUT/$name.png"

    # Fail loudly if the firmware never reached the watch face.
    if ! grep -q "watch face ready" "$uart"; then
        echo "no 'watch face ready' in $uart" >&2
        exit 1
    fi
}

run_case am "$HERE/board.json"
run_case pm "$HERE/board.json" --inject-on "console ready" --inject "settime 2026-11-10 21:41\n"
run_case unknown "$HERE/board-unknown.json"

echo
echo "wrote $OUT/am.png, $OUT/pm.png, $OUT/unknown.png"
