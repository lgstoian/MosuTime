#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Format the SD card and push mosu.cfg plus the voice clips over USB.

The watch implements an ACK-paced binary upload on the console:

    receive <path> <size> [/crc]
    === RX READY ===
    <4096 bytes>          ->  RX 4096
    ...                   ->  RX <cumulative>
    <crc32 little endian> ->  === RX DONE ===

Every chunk is answered only after it reached the file, so a slow card cannot
overflow the USB Serial/JTAG path.  The CRC-32 is checked by the watch, which
removes the file if it does not match.

Examples:
    python tools/push_sd.py COM11 --format
    python tools/push_sd.py COM11 --format --ssid camera --pass 'secret'
    python tools/push_sd.py COM11 --no-voice
"""

import argparse
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial is required: python -m pip install pyserial\n")
    raise SystemExit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "sd_card", "mosu.cfg")
VOICE = os.path.join(ROOT, "sd_card", "voice")
REMOTE_CFG = "/sdcard/mosu.cfg"
REMOTE_VOICE = "/sdcard/voice"

CHUNK = 4096
READY = b"=== RX READY ==="
DONE = b"=== RX DONE ==="


class Console:
    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.05
        self.ser.write_timeout = 20
        self.ser.dsrdtr = False
        self.ser.rtscts = False
        self.ser.open()
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass
        self.buffer = b""

    def close(self):
        self.ser.close()

    def _fill(self, deadline):
        data = self.ser.read(self.ser.in_waiting or 1)
        if data:
            self.buffer += data
            return True
        return time.time() < deadline

    def read_until(self, marker, timeout):
        deadline = time.time() + timeout
        while marker not in self.buffer:
            if not self._fill(deadline):
                return None
        head, self.buffer = self.buffer.split(marker, 1)
        return head + marker

    def read_line(self, timeout):
        deadline = time.time() + timeout
        while b"\n" not in self.buffer:
            if not self._fill(deadline):
                return None
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.replace(b"\r", b"").decode("utf-8", "replace")

    def command(self, line):
        self.ser.write((line + "\n").encode())
        self.ser.flush()

    def wait_shell(self):
        for _ in range(20):
            self.command("help")
            ended = time.time() + 0.7
            while time.time() < ended:
                text = self.read_line(0.2)
                if text is None:
                    continue
                if "commands:" in text:
                    return True
        return False


def format_card(console):
    print("formatting the card as FAT32 (this erases it)...", flush=True)
    console.command("sd format yes")
    deadline = time.time() + 180
    tail = []
    while time.time() < deadline:
        line = console.read_line(1.0)
        if line is None:
            continue
        tail.append(line)
        if line.startswith("sd format:") and "do not remove" not in line:
            print("  " + line)
            return "ESP_OK" in line
    print("  FAIL: no answer to sd format")
    print("  last output: %r" % ("\n".join(tail[-4:])))
    return False


def push_file(console, target, data, label):
    console.command("receive %s %d /crc" % (target, len(data)))
    if console.read_until(READY, 15) is None:
        print("  !! no READY for %s" % label)
        return False

    sent = 0
    while sent < len(data):
        chunk = data[sent:sent + CHUNK]
        try:
            console.ser.write(chunk)
        except Exception as exc:
            print("  !! %s: write failed at %d (%s)" % (label, sent, exc))
            return False
        sent += len(chunk)

        answer = console.read_line(6.0)
        while answer is not None and not answer.startswith("RX "):
            if answer.startswith("receive:"):
                print("  !! %s: %s" % (label, answer))
                return False
            answer = console.read_line(6.0)
        if answer is None:
            print("  !! no ACK for %s at %d/%d" % (label, sent, len(data)))
            return False
        cumulative = int(answer[3:])
        if cumulative != sent:
            print("  !! %s ACK %d, sent %d" % (label, cumulative, sent))
            return False

    try:
        console.ser.write(struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF))
    except Exception as exc:
        print("  !! %s: CRC write failed (%s)" % (label, exc))
        return False
    if console.read_until(DONE, 20) is None:
        print("  !! no DONE for %s" % label)
        return False
    return True


def resync(console):
    """Get the console back after a stalled or abandoned receive."""
    for _ in range(4):
        console.buffer = b""
        console.command("help")
        if console.read_until(b"commands:", 7) is not None:
            return True
        time.sleep(1.0)
    return False


def config_payload(ssid, password, stats=None):
    with open(CFG, "rb") as handle:
        text = handle.read().decode("utf-8")
    if ssid is not None:
        text = text.replace('network1_ssid = "Acasa"',
                            'network1_ssid = "%s"' % ssid)
    if password is not None:
        text = text.replace('network1_pass = "parola"',
                            'network1_pass = "%s"' % password)
    if stats is not None:
        text = text.replace("stats = false",
                            "stats = %s" % ("true" if stats else "false"))
    if "\n" not in text:
        text += "\n"
    return text.encode("utf-8")


def main():
    sys.stdout.reconfigure(line_buffering=True)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", default="COM11")
    parser.add_argument("--format", action="store_true",
                        help="format the card as FAT32 before uploading")
    parser.add_argument("--ssid", help="replace the template Wi-Fi SSID")
    parser.add_argument("--pass", dest="password",
                        help="replace the template Wi-Fi password")
    parser.add_argument("--no-voice", action="store_true",
                        help="upload only mosu.cfg")
    parser.add_argument("--stats", action="store_true",
                        help="enable the power counters in the pushed config")
    args = parser.parse_args()

    try:
        console = Console(args.port)
    except Exception as exc:
        print("FAIL: cannot open %s: %s" % (args.port, exc))
        return 1

    try:
        if not console.wait_shell():
            print("FAIL: console not responding on %s" % args.port)
            return 1

        if args.format and not format_card(console):
            return 1

        console.command("md " + REMOTE_VOICE)
        time.sleep(0.3)

        ok = True
        cfg = config_payload(args.ssid, args.password, args.stats)
        if push_file(console, REMOTE_CFG, cfg, "mosu.cfg"):
            print("PUSH ok   %s (%d bytes)" % (REMOTE_CFG, len(cfg)),
                  flush=True)
        else:
            ok = False

        if ok and not args.no_voice and os.path.isdir(VOICE):
            names = sorted(n for n in os.listdir(VOICE) if n.endswith(".wav"))
            started = time.time()
            total = sum(os.path.getsize(os.path.join(VOICE, n)) for n in names)
            done = 0
            for index, name in enumerate(names, 1):
                source = os.path.join(VOICE, name)
                with open(source, "rb") as handle:
                    data = handle.read()
                target = REMOTE_VOICE + "/" + name
                pushed = False
                for attempt in range(2):
                    if push_file(console, target, data, name):
                        pushed = True
                        break
                    if attempt == 0:
                        print("  retry %s" % name)
                        resync(console)
                if not pushed:
                    print("PUSH FAIL %s" % target)
                    ok = False
                    break
                done += len(data)
                if index % 10 == 0 or index == len(names):
                    rate = done / max(time.time() - started, 0.001) / 1024
                    print("PUSH %d/%d %s (%d/%d KiB, %.0f KiB/s)" %
                          (index, len(names), name, done // 1024,
                           total // 1024, rate),
                          flush=True)
        elif ok and args.no_voice:
            print("skipping the voice clips (--no-voice)")

        if ok:
            console.command("ls " + REMOTE_VOICE)
            time.sleep(1.0)
            while True:
                line = console.read_line(0.5)
                if line is None:
                    break
                if line.startswith("ls ") or line.startswith("  "):
                    print(line, flush=True)
    finally:
        console.close()

    print("PUSH", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
