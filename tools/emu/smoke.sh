#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# Boot the real firmware against the interactive board and drive it the way a
# person would: a tap and a double tap on the FT3168, then a wrist raise on the
# QMI8658 after the face has gone to sleep.
#
# Prerequisites: see README.md in this folder, and a build-emu firmware.
# Run from anywhere; paths are resolved from this script.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

EMU="${ESP_EMU:-}"
if [ -z "$EMU" ]; then
    # 0.51.0 has no virtual parts, and a stale copy may still be in PATH.
    for candidate in esp-emu "$HOME/.local/bin/esp-emu" esp-emu-rc "$HOME/.local/bin/esp-emu-rc"; do
        if command -v "$candidate" >/dev/null 2>&1 \
            && "$candidate" --help 2>&1 | grep -q -- "--cosim-board"; then
            EMU="$candidate"
            break
        fi
    done
fi
if [ -z "$EMU" ] || ! command -v "$EMU" >/dev/null 2>&1; then
    echo "no esp-emu with virtual parts found; set ESP_EMU=/path/to/esp-emu" >&2
    exit 1
fi
FW="${FW:-$ROOT/build-emu/merged-binary.bin}"
BOARD="$HERE/board-interactive.json"
TOUCH_PORT="${TOUCH_PORT:-5556}"
IMU_PORT="${IMU_PORT:-5557}"

if [ ! -f "$FW" ]; then
    echo "firmware not found: $FW (build build-emu first)" >&2
    exit 1
fi

# ---------------------------------------------------------------- the parts

# build_part DIR CRATE
build_part() {
    local dir="$1"
    local crate="$2"
    local wasm="$dir/target/wasm32-unknown-unknown/release/$crate.wasm"

    if [ -f "$wasm" ] && [ "$wasm" -nt "$dir/src/lib.rs" ] && [ "$wasm" -nt "$dir/Cargo.toml" ]; then
        return
    fi

    echo "== building $crate"
    (
        cd "$dir"
        ESP_COSIM_PROTO_DIR="${ESP_COSIM_PROTO_DIR:-$ROOT/../esp-emulator/proto}" \
            cargo build --release --target wasm32-unknown-unknown
    )
}

build_part "$HERE/panel" mosu_panel
build_part "$HERE/touch" mosu_touch
build_part "$HERE/imu" mosu_imu
build_part "$HERE/chips" mosu_chips

# ------------------------------------------------------------------ helpers

EMU_PID=""
cleanup() {
    if [ -n "$EMU_PID" ] && kill -0 "$EMU_PID" 2>/dev/null; then
        kill "$EMU_PID" 2>/dev/null || true
        wait "$EMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# start_emu UART_LOG COSIM_LOG PORT TIMEOUT
start_emu() {
    local uart="$1"
    local cosim="$2"
    local port="$3"
    local timeout="$4"

    rm -f "$uart" "$cosim"
    "$EMU" --chip esp32s3 \
        --firmware "$FW" \
        --psram-size 8M \
        --console uart0 \
        --timeout "$timeout" \
        --net "${ESP_EMU_NET:-user,restrict=yes}" \
        --cosim-board "$BOARD" \
        --control-tcp "127.0.0.1:$port" \
        --cosim-log "$cosim" \
        --cosim-log-filter BusRequest,GpioDrive \
        > "$uart" 2>&1 &
    EMU_PID=$!
}

# control_cli PORT MODE UART COSIM
# MODE is "touch" or "imu".  It talks to the emulator and exits 0 only when
# every interaction reached the firmware.
control_cli() {
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import json
import socket
import sys
import time

port, mode, uart_path, cosim_path = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
WAIT_READY_S = 40


def wait_for_uart(needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(uart_path, encoding="utf-8", errors="replace") as f:
                if needle in f.read():
                    return
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    raise SystemExit(f"timed out waiting for {needle!r} in {uart_path}")


class Control:
    def __init__(self, port):
        end = time.time() + 15
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.time() > end:
                    raise SystemExit(f"control channel 127.0.0.1:{port} did not open")
                time.sleep(0.1)
        self.reader = self.sock.makefile("rwb")

    def cmd(self, line):
        self.reader.write((line + "\n").encode())
        self.reader.flush()
        reply = self.reader.readline().decode().strip()
        if not reply:
            raise SystemExit(f"no answer to {line!r}")
        return reply

    def clock_ms(self):
        reply = self.cmd("part clock")
        if not reply.startswith("ok "):
            raise SystemExit(f"part clock answered {reply!r}")
        return int(reply.split()[1])

    def close(self):
        self.sock.shutdown(socket.SHUT_WR)
        self.sock.close()


def panel_events(path):
    """Every panel SPI transfer in the co-simulation log, as (t_ns, tx)."""
    events = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if entry.get("part") != "panel":
                    continue
                spi = entry.get("msg", {}).get("BusRequest", {}).get("op", {}).get("Spi")
                if spi is None:
                    continue
                events.append((entry["t_ns"], spi.get("tx") or []))
    except FileNotFoundError:
        pass
    return events


def is_command(tx, opcode):
    # A one-byte panel command reaches the part as 02 00 <cmd> 00: 0x02 is
    # LCD_OPCODE_WRITE_CMD and 0x32 is the colour write opcode.
    return len(tx) >= 4 and tx[0] in (0x02, 0x32) and tx[2] == opcode


wait_for_uart("watch face ready", WAIT_READY_S)
ctl = Control(port)
try:
    parts = ctl.cmd("part list")
    if not parts.startswith("ok "):
        raise SystemExit(f"part list answered {parts!r}")
    names = set(parts.split()[1:])
    missing = {"panel", "touch", "imu", "chips"} - names
    if missing:
        raise SystemExit(f"parts missing from the board: {sorted(missing)} (got {parts!r})")

    if mode == "touch":
        def sleep_emulated(ms):
            start = ctl.clock_ms()
            while ctl.clock_ms() - start < ms:
                time.sleep(0.05)

        def uart_has(needle):
            try:
                with open(uart_path, encoding="utf-8", errors="replace") as f:
                    return needle in f.read()
            except FileNotFoundError:
                return False

        # The first frame is drawn by the LVGL task; a touch delivered while it
        # is still starting is coalesced away.  Let the firmware settle before
        # emulating a finger, as a person would.
        while ctl.clock_ms() < 3000:
            time.sleep(0.1)
        print(f"touch starting at {ctl.clock_ms()} ms of emulated time")

        # One tap, then double taps until the phrase builder answers.  The
        # firmware turns the touch interrupt into a level-low wake source
        # (mosu_board_enable_touch_wake), so an edge can occasionally be lost
        # between LVGL reads; a person would simply tap again.
        reply = ctl.cmd("part touch input tap")
        if reply != "ok":
            raise SystemExit(f"'part touch input tap' answered {reply!r}")
        sleep_emulated(200)

        clip = "missing clip: /sdcard/voice/d_"
        for attempt in range(1, 7):
            reply = ctl.cmd("part touch input double")
            if reply != "ok":
                raise SystemExit(f"'part touch input double' answered {reply!r}")
            deadline = ctl.clock_ms() + 1200
            while ctl.clock_ms() < deadline:
                if uart_has(clip):
                    break
                time.sleep(0.05)
            print(f"double tap {attempt}: {'phrase built' if uart_has(clip) else 'no phrase yet'}")
            if uart_has(clip):
                break
        else:
            raise SystemExit("no double tap reached the phrase builder in six tries")

        sleep_emulated(400)
        state = ctl.cmd("part touch state")
        print(f"touch state after the gestures: {state}")
        if state != "ok pressed=false x=205 y=251":
            raise SystemExit(f"the touch part did not settle after the gestures: {state}")

    elif mode == "imu":
        # The face turns itself off after screen_on_ms (15 s by default).  Wait
        # for emulated time, not wall time, so a slow host changes nothing.
        deadline = time.time() + 90
        while True:
            now = ctl.clock_ms()
            if now >= 16000:
                break
            if time.time() > deadline:
                raise SystemExit(f"emulated clock stuck at {now} ms")
            time.sleep(0.5)

        off_ms = ctl.clock_ms()
        reply = ctl.cmd("part imu state")
        if reply != "ok x=0 y=0 z=1":
            print("warning: IMU did not start at rest:", reply)

        raise_ms = ctl.clock_ms()
        reply = ctl.cmd("part imu input raise")
        if reply != "ok":
            raise SystemExit(f"'part imu input raise' answered {reply!r}")

        # Nothing may reach the panel while the face sleeps.
        quiet = [e for e in panel_events(cosim_path) if off_ms * 1_000_000 < e[0] <= raise_ms * 1_000_000]
        if quiet:
            raise SystemExit(f"{len(quiet)} panel transfer(s) while the face should be asleep")

        # A raise that the firmware accepts ends in the panel's display-on
        # command (0x29) after the motion detector's 150 ms hold.
        end = time.time() + 8
        while time.time() < end:
            hits = [e for e in panel_events(cosim_path)
                    if e[0] > raise_ms * 1_000_000 and is_command(e[1], 0x29)]
            if hits:
                print(f"screen on at {hits[0][0] / 1e6:.0f} ms (raise at {raise_ms} ms)")
                break
            time.sleep(0.25)
        else:
            raise SystemExit("no display-on command after the wrist raise: the screen did not wake")

        offs = [e for e in panel_events(cosim_path) if is_command(e[1], 0x28)]
        if not offs:
            raise SystemExit("no display-off command: the face never slept, so the wake is unproven")
        print(f"screen off at {offs[-1][0] / 1e6:.0f} ms (0x28), woke at {hits[0][0] / 1e6:.0f} ms (0x29)")
    else:
        raise SystemExit(f"unknown mode {mode!r}")
finally:
    ctl.close()
PY
}

# assert_clean UART_LOG
assert_clean() {
    local uart="$1"
    if grep -Eq "Guru Meditation|assert failed:|abort\(\) was called|ESP_ERROR_CHECK failed|panic'ed|Stack canary watchpoint" "$uart"; then
        echo "panic or assert in $uart:" >&2
        grep -En "Guru Meditation|assert failed:|abort\(\) was called|ESP_ERROR_CHECK failed|panic'ed|Stack canary watchpoint" "$uart" >&2
        exit 1
    fi
}

# assert_clips UART_LOG
assert_clips() {
    local uart="$1"
    local missing=0
    for prefix in h_ m_ d_; do
        if ! grep -Eq "missing clip: /sdcard/voice/${prefix}[0-9]+\.wav" "$uart"; then
            echo "no ${prefix}* missing-clip line in $uart" >&2
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then
        echo "--- clip lines found" >&2
        grep "missing clip" "$uart" >&2 || true
        exit 1
    fi
    grep "missing clip" "$uart"
}

# ---------------------------------------------------------------- touch run

echo "== touch: tap, then double tap on FT3168"
TOUCH_UART="$HERE/smoke-touch.log"
TOUCH_COSIM="$HERE/smoke-touch.jsonl"
start_emu "$TOUCH_UART" "$TOUCH_COSIM" "$TOUCH_PORT" 20s
control_cli "$TOUCH_PORT" touch "$TOUCH_UART" "$TOUCH_COSIM"
wait "$EMU_PID" || true
EMU_PID=""

grep -q "watch face ready" "$TOUCH_UART" || {
    echo "no 'watch face ready' in $TOUCH_UART" >&2
    exit 1
}

# INT (GPIO 38) was driven low and back high by the part.
if ! python3 - "$TOUCH_COSIM" <<'PY'
import json, sys
levels = []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    try:
        entry = json.loads(line)
    except json.JSONDecodeError:
        continue
    drive = entry.get("msg", {}).get("GpioDrive")
    if drive and drive.get("pin") == 38:
        levels.append(bool(drive.get("level")))
if not levels or levels[0] is not False or True not in levels[1:]:
    raise SystemExit(f"INT was not pulled low and released: {levels}")
print(f"INT drove: {levels}")
PY
then
    echo "touch did not drive the interrupt line" >&2
    exit 1
fi

assert_clips "$TOUCH_UART"
assert_clean "$TOUCH_UART"
echo "touch: tap reached the controller, double tap reached the phrase builder"

# ------------------------------------------------------------------ imu run

echo "== imu: wait for the face to sleep, then a wrist raise"
IMU_UART="$HERE/smoke-imu.log"
IMU_COSIM="$HERE/smoke-imu.jsonl"
start_emu "$IMU_UART" "$IMU_COSIM" "$IMU_PORT" 40s
control_cli "$IMU_PORT" imu "$IMU_UART" "$IMU_COSIM"
cleanup
EMU_PID=""

assert_clean "$IMU_UART"
echo "imu: the wrist raise woke the panel"

echo
echo "smoke: all interactions reached the firmware"
echo "logs: $TOUCH_UART, $TOUCH_COSIM, $IMU_UART, $IMU_COSIM"
