#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""MosuTime Manager - a small Windows GUI for the watch.

Tabs:
  Dashboard  live status, battery, network, SD, quick actions
  Console    raw console with the same commands as the serial terminal
  Firmware   build and flash through tools/build.ps1, with a live log
  SD card    format the card, push mosu.cfg and the voice clips
  Power      battery detail and a graceful shutdown

Everything goes through the USB Serial/JTAG port that also flashes the board,
so one cable is enough.  Run with:

    python tools/mosu_gui.py
"""

import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
import zlib
from tkinter import colorchooser, filedialog, messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    tk.messagebox.showerror(
        "MosuTime Manager",
        "pyserial is missing.\n\nRun: python -m pip install pyserial")
    raise SystemExit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_SCRIPT = os.path.join(ROOT, "tools", "build.ps1")
SD_CFG = os.path.join(ROOT, "sd_card", "mosu.cfg")
SD_VOICE = os.path.join(ROOT, "sd_card", "voice")
BUILD_DIR = os.path.join(ROOT, "build")

BG = "#14161a"
PANEL = "#1c1f25"
FG = "#e8eaed"
MUTED = "#9aa0a6"
GREEN = "#34c759"
AMBER = "#ffb020"
RED = "#ff453a"
ACCENT = "#4c8dff"

CHUNK = 4096
READY = "=== RX READY ==="
DONE = "=== RX DONE ==="


class Device:
    """One serial port, one buffer, one log sink."""

    def __init__(self, log):
        self.ser = None
        self.buffer = bytearray()
        self.lock = threading.RLock()
        self.busy = False
        self.log = log

    @property
    def open(self):
        return self.ser is not None and self.ser.is_open

    def open_port(self, port):
        self.close()
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.02
        self.ser.write_timeout = 20
        self.ser.dsrdtr = False
        self.ser.rtscts = False
        self.ser.open()
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass
        self.buffer.clear()

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.buffer.clear()

    def write(self, data):
        self.ser.write(data)

    def send(self, line):
        self.write((line + "\n").encode("utf-8"))

    def _fill(self, deadline):
        data = self.ser.read(self.ser.in_waiting or 1)
        if data:
            self.buffer += data
            return True
        return time.time() < deadline

    def read_line(self, timeout):
        deadline = time.time() + timeout
        while b"\n" not in self.buffer:
            if not self._fill(deadline):
                return None
        raw, _, rest = self.buffer.partition(b"\n")
        self.buffer = bytearray(rest)
        line = raw.replace(b"\r", b"").decode("utf-8", "replace")
        self.log(line)
        return line

    def wait_line(self, marker, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.read_line(min(1.0, max(0.01, deadline - time.time())))
            if line is None:
                continue
            if marker in line:
                return True
        return False

    def drain(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.read_line(0.2)

    def capture(self, seconds):
        """Read lines for a while, log them, and return them for parsing."""
        out = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            line = self.read_line(min(0.2, max(0.01, deadline - time.time())))
            if line is not None:
                out.append(line)
        return out

    def pump(self):
        if not self.open or self.busy:
            return
        with self.lock:
            while self.buffer or self.ser.in_waiting:
                self.read_line(0.01)

    def command(self, cmd, settle=1.0):
        self.send(cmd)
        self.drain(settle)


def find_ports():
    ports = []
    for info in list_ports.comports():
        label = "%s - %s" % (info.device, info.description or "serial")
        ports.append((info.device, label, info.vid, info.pid))
    return ports


def pick_port(port):
    for info in list_ports.comports():
        if info.device == port:
            return info
    return None


class Field:
    """One mosu.cfg key, its type, its range and a short help line."""

    def __init__(self, section, key, label, kind, default, lo=None, hi=None,
                 step=1, choices=None, hint="", secret=False):
        self.section = section
        self.key = key
        self.label = label
        self.kind = kind
        self.default = default
        self.lo = lo
        self.hi = hi
        self.step = step
        self.choices = choices or []
        self.hint = hint
        self.secret = secret


SCHEMA = [
    Field("device", "name", "Device name", "text", "MosuTime",
          hint="Shown in the console only"),
    Field("device", "log_level", "Log level", "enum", "info",
          choices=["none", "error", "warn", "info", "debug", "verbose"],
          hint="Console and USB log detail"),

    Field("display", "brightness", "Brightness", "int", 55, 1, 100,
          hint="1..100, the same at every wake"),
    Field("display", "screen_on_ms", "Screen on time", "int", 15000, 1000,
          300000, 500, hint="Milliseconds after a touch or wrist raise"),
    Field("display", "wake_on_touch", "Touch wake", "bool", True,
          hint="Touch the glass to light the face"),
    Field("display", "wake_on_motion", "Wrist raise wake", "bool", True,
          hint="Raise the arm to light the face"),
    Field("display", "motion_angle_deg", "Raise angle", "int", 35, 10, 80,
          hint="Minimum wrist angle in degrees"),
    Field("display", "motion_refractory_ms", "Raise gap", "int", 2500, 300,
          20000, 100, hint="Ignore raises this long after one"),
    Field("display", "motion_poll_hz", "IMU poll rate", "int", 8, 1, 20,
          hint="Samples per second while the screen is off (4..8 keeps power low)"),
    Field("display", "pixel_shift_sec", "Pixel shift", "int", 180, 0, 3600,
          10, hint="Move the face 1..2 px to avoid burn-in; 0 = off"),
    Field("display", "tap_feedback_ms", "Tap border", "int", 250, 0, 2000, 50,
          hint="Border flash after a double tap; 0 = none"),

    Field("colors", "background", "Background", "color", "#000000"),
    Field("colors", "time_am", "Time (AM)", "color", "#FFFFFF",
          hint="Digits in the morning"),
    Field("colors", "time_pm", "Time (PM)", "color", "#FFC98A",
          hint="Digits in the afternoon and evening"),
    Field("colors", "battery_green", "Battery green", "color", "#00E676"),
    Field("colors", "battery_orange", "Battery orange", "color", "#FFAB00"),
    Field("colors", "battery_red", "Battery red", "color", "#FF1744"),
    Field("colors", "wifi_idle", "Wi-Fi idle", "color", "#8A8A8A",
          hint="Grey: not synced yet"),
    Field("colors", "wifi_ok", "Wi-Fi ok", "color", "#00E676"),
    Field("colors", "wifi_fail", "Wi-Fi failed", "color", "#FF1744"),
    Field("colors", "tap_border", "Tap border", "color", "#FFFFFF"),

    Field("status", "battery_green_min", "Green from", "int", 40, 1, 100,
          hint="Percent at or above this is green"),
    Field("status", "battery_orange_min", "Orange from", "int", 15, 1, 100,
          hint="Percent at or above this is orange, below is red"),
    Field("status", "show_battery", "Show battery icon", "bool", True),
    Field("status", "show_wifi", "Show Wi-Fi icon", "bool", True),
    Field("status", "wifi_fail_slash", "Slash on failure", "bool", True,
          hint="Cross the Wi-Fi icon when the last sync failed"),

    Field("clock", "timezone", "Timezone", "text", "Europe/Bucharest",
          hint="IANA name, POSIX TZ or a numeric offset"),
    Field("clock", "auto_timezone", "Automatic timezone", "bool", True,
          hint="Ask the network for the zone using the IP address"),
    Field("clock", "geo_url", "Geolocation URL", "text",
          "http://ip-api.com/line/?fields=timezone,offset",
          hint="Plain text, two lines or JSON with timezone/offset"),
    Field("clock", "ntp1", "NTP server 1", "text", "pool.ntp.org"),
    Field("clock", "ntp2", "NTP server 2", "text", "time.google.com"),
    Field("clock", "sync_hours", "Sync interval", "int", 6, 1, 168,
          hint="Hours between syncs when sync_per_day is 0"),
    Field("clock", "sync_per_day", "Syncs per day", "int", 0, 0, 24,
          hint="Overrides the interval: 4 = every 6 h, 24 = hourly, 0 = off"),
    Field("clock", "wifi_always_on", "Wi-Fi always on", "bool", False,
          hint="Keep the radio on; costs far more current"),
    Field("clock", "manual_time", "Manual time", "text", "",
          hint='Fallback "YYYY-MM-DD HH:MM", local time'),

    *[field for i in range(1, 9) for field in (
        Field("wifi", "network%d_ssid" % i, "Network %d SSID" % i, "text", "",
              hint="Tried in order; leave blank to disable" if i == 1 else ""),
        Field("wifi", "network%d_pass" % i, "Network %d password" % i,
              "secret", "", secret=True),
    )],

    Field("announce", "enabled", "Voice announcement", "bool", True,
          hint="A double tap speaks the time"),
    Field("announce", "double_tap_ms", "Double tap window", "int", 650, 200,
          1500, 50, hint="Maximum gap between the two taps"),
    Field("announce", "max_hold_ms", "Tap hold", "int", 400, 80, 1500, 10,
          hint="Longest press that still counts as a tap"),
    Field("announce", "max_move_px", "Tap movement", "int", 40, 4, 200,
          hint="How far the finger may slide"),
    Field("announce", "cooldown_ms", "Announcement cooldown", "int", 2000, 300,
          10000, 100, hint="Pause after a sentence, to avoid repeats"),
    Field("announce", "volume", "Volume", "int", 85, 0, 100),
    Field("announce", "announce_day", "Say the weekday", "bool", True),
    Field("announce", "voice_dir", "Voice folder", "text", "/sdcard/voice"),
    Field("announce", "clip_gap_ms", "Clip gap", "int", 60, 0, 1000, 10,
          hint="Pause between the WAV fragments"),
]


class ConfigFile:
    """Read/write mosu.cfg while keeping every comment and blank line."""

    def __init__(self, path):
        self.path = path
        with open(path, "r", encoding="utf-8", newline="") as handle:
            self.text = handle.read()

    def save(self, path=None):
        with open(path or self.path, "w", encoding="utf-8", newline="") as handle:
            handle.write(self.text)

    def _lines(self):
        return self.text.splitlines(keepends=True)

    def get(self, section, key):
        current = ""
        for raw in self._lines():
            s = raw.strip()
            if s.startswith("[") and s.endswith("]"):
                current = s[1:-1].strip().lower()
                continue
            if not s or s.startswith("#") or s.startswith(";") or "=" not in raw:
                continue
            lhs, rhs = raw.split("=", 1)
            if current == section and lhs.strip().lower() == key:
                return rhs.strip()
        return None

    def set(self, section, key, value):
        lines = self._lines()
        current = ""
        last_in_section = None
        comment_line = None
        for index, raw in enumerate(lines):
            s = raw.strip()
            if s.startswith("[") and s.endswith("]"):
                current = s[1:-1].strip().lower()
                continue
            if s.startswith("#") and current == section:
                body = s.lstrip("#").strip()
                if "=" in body and body.split("=", 1)[0].strip().lower() == key:
                    comment_line = index
                continue
            if not s or s.startswith(";") or "=" not in raw:
                continue
            if current == section:
                last_in_section = index
            lhs, rhs = raw.split("=", 1)
            if current == section and lhs.strip().lower() == key:
                indent = raw[:len(raw) - len(raw.lstrip())]
                eol = "\r\n" if raw.endswith("\r\n") else "\n"
                comment = self._comment(rhs)
                lines[index] = "%s%s = %s%s%s" % (
                    indent, key, value, comment, eol)
                self.text = "".join(lines)
                return
        line = "%s = %s\n" % (key, value)
        if comment_line is not None:
            raw = lines[comment_line]
            indent = raw[:len(raw) - len(raw.lstrip())]
            eol = "\r\n" if raw.endswith("\r\n") else "\n"
            lines[comment_line] = "%s%s = %s%s" % (indent, key, value, eol)
        elif last_in_section is not None:
            eol = "\r\n" if lines[last_in_section].endswith("\r\n") else "\n"
            lines.insert(last_in_section + 1, line.replace("\n", eol))
        else:
            lines.append("\n[%s]\n%s" % (section, line))
        self.text = "".join(lines)

    @staticmethod
    def _comment(rhs):
        quote = False
        for index, ch in enumerate(rhs):
            if ch == '"':
                quote = not quote
            elif ch == "#" and not quote and index and rhs[index - 1] == " ":
                return " " + rhs[index - 1:].strip()
        return ""


def parse_raw(field, raw):
    if raw is None:
        return field.default
    text = raw.strip()
    if field.kind in ("text", "secret"):
        if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
            text = text[1:-1]
        return text
    if field.kind == "bool":
        return text.lower() in ("true", "yes", "on", "1")
    if field.kind == "int":
        try:
            return int(text)
        except ValueError:
            return field.default
    return text


def format_value(field, value):
    if field.kind in ("text", "secret"):
        return '"%s"' % value
    if field.kind == "bool":
        return "true" if value else "false"
    return str(value)


class App:
    def __init__(self, root):
        self.root = root
        self.lines = queue.Queue()
        self.actions = queue.Queue()
        self.device = Device(self.push_log)
        self.worker = None
        self.info = {}

        root.title("MosuTime Manager")
        root.geometry("980x720")
        root.minsize(820, 600)
        root.configure(bg=BG)
        self.style()
        self.build_ui()
        self.refresh_ports()
        self.root.after(80, self.tick)

    # ------------------------------------------------------------------ ui

    def style(self):
        s = ttk.Style()
        s.theme_use("clam")
        s.configure(".", background=BG, foreground=FG, fieldbackground=PANEL,
                    bordercolor="#2a2e36", lightcolor=PANEL, darkcolor=PANEL)
        s.configure("TFrame", background=BG)
        s.configure("Panel.TFrame", background=PANEL)
        s.configure("TLabel", background=BG, foreground=FG)
        s.configure("Muted.TLabel", background=BG, foreground=MUTED)
        s.configure("Panel.TLabel", background=PANEL, foreground=FG)
        s.configure("PanelMuted.TLabel", background=PANEL, foreground=MUTED)
        s.configure("Title.TLabel", background=BG, foreground=FG,
                    font=("Segoe UI Semibold", 16))
        s.configure("Value.TLabel", background=PANEL, foreground=FG,
                    font=("Consolas", 11))
        s.configure("Big.TLabel", background=PANEL, foreground=FG,
                    font=("Segoe UI Semibold", 30))
        s.configure("TButton", background="#2a2f38", foreground=FG,
                    padding=(12, 6), borderwidth=0)
        s.map("TButton",
              background=[("active", "#353b46"), ("disabled", "#20242a")],
              foreground=[("disabled", "#6b7078")])
        s.configure("Accent.TButton", background=ACCENT, foreground="#ffffff")
        s.map("Accent.TButton", background=[("active", "#6aa1ff")])
        s.configure("Danger.TButton", background="#7a2b28", foreground=FG)
        s.map("Danger.TButton", background=[("active", "#9b3733")])
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", background="#1a1d22", foreground=MUTED,
                    padding=(18, 8))
        s.map("TNotebook.Tab", background=[("selected", PANEL)],
              foreground=[("selected", FG)])
        s.configure("TEntry", fieldbackground=PANEL, foreground=FG,
                    insertcolor=FG, bordercolor="#2a2e36")
        s.configure("TCombobox", fieldbackground=PANEL, foreground=FG,
                    background=PANEL, arrowcolor=FG)
        s.configure("Horizontal.TProgressbar", background=ACCENT,
                    troughcolor=PANEL, borderwidth=0)
        s.configure("TSeparator", background="#2a2e36")

    def build_ui(self):
        top = ttk.Frame(self.root, padding=(14, 12, 14, 6))
        top.pack(fill="x")
        ttk.Label(top, text="MosuTime", style="Title.TLabel").pack(side="left")
        ttk.Label(top, text="  watch manager", style="Muted.TLabel").pack(
            side="left", padx=(2, 0), pady=(6, 0))

        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(top, textvariable=self.port_var, width=46,
                                     state="readonly")
        self.port_box.pack(side="right", padx=(8, 0))
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(
            side="right", padx=(8, 0))
        self.connect_btn = ttk.Button(top, text="Connect", style="Accent.TButton",
                                      command=self.toggle_connect)
        self.connect_btn.pack(side="right", padx=(8, 0))
        self.dot = tk.Canvas(top, width=12, height=12, bg=BG,
                             highlightthickness=0)
        self.dot_id = self.dot.create_oval(1, 1, 11, 11, fill="#555b64",
                                           outline="")
        self.dot.pack(side="right")

        self.tabs = ttk.Notebook(self.root)
        self.tabs.pack(fill="both", expand=True, padx=12, pady=(0, 6))
        self.tab_dash = ttk.Frame(self.tabs, padding=14)
        self.tab_console = ttk.Frame(self.tabs, padding=14)
        self.tab_firmware = ttk.Frame(self.tabs, padding=14)
        self.tab_settings = ttk.Frame(self.tabs, padding=10)
        self.tab_sd = ttk.Frame(self.tabs, padding=14)
        self.tab_power = ttk.Frame(self.tabs, padding=14)
        self.tabs.add(self.tab_dash, text="Dashboard")
        self.tabs.add(self.tab_console, text="Console")
        self.tabs.add(self.tab_settings, text="Settings")
        self.tabs.add(self.tab_firmware, text="Firmware")
        self.tabs.add(self.tab_sd, text="SD card")
        self.tabs.add(self.tab_power, text="Power")
        self.build_dashboard()
        self.build_console()
        self.build_settings()
        self.build_firmware()
        self.build_sd()
        self.build_power()

        bottom = ttk.Frame(self.root, padding=(14, 0, 14, 10))
        bottom.pack(fill="x")
        self.status = ttk.Label(bottom, text="Not connected", style="Muted.TLabel")
        self.status.pack(side="left")
        self.progress = ttk.Progressbar(bottom, mode="determinate", length=220)
        self.progress.pack(side="right")

    def card(self, parent, title):
        frame = ttk.Frame(parent, style="Panel.TFrame", padding=14)
        ttk.Label(frame, text=title, style="Panel.TLabel",
                  font=("Segoe UI Semibold", 11)).pack(anchor="w")
        return frame

    def build_dashboard(self):
        grid = ttk.Frame(self.tab_dash)
        grid.pack(fill="both", expand=True)
        grid.columnconfigure(0, weight=1)
        grid.columnconfigure(1, weight=1)

        dev = self.card(grid, "Firmware and board")
        dev.grid(row=0, column=0, sticky="nsew", padx=(0, 8), pady=(0, 8))
        self.d_app = self.add_row(dev, "Firmware", "-")
        self.d_idf = self.add_row(dev, "ESP-IDF", "-")
        self.d_mac = self.add_row(dev, "MAC", "-")
        self.d_chip = self.add_row(dev, "Chip", "-")

        clock = self.card(grid, "Clock and network")
        clock.grid(row=0, column=1, sticky="nsew", padx=(8, 0), pady=(0, 8))
        self.d_clock = self.add_row(clock, "Clock", "-")
        self.d_tz = self.add_row(clock, "Timezone", "-")
        self.d_net = self.add_row(clock, "Wi-Fi", "-")
        self.d_sync = self.add_row(clock, "Last sync", "-")

        power = self.card(grid, "Power and storage")
        power.grid(row=1, column=0, sticky="nsew", padx=(0, 8), pady=(8, 0))
        self.d_batt = self.add_row(power, "Battery", "-")
        self.d_charge = self.add_row(power, "Charging", "-")
        self.d_sd = self.add_row(power, "SD card", "-")
        self.d_audio = self.add_row(power, "Audio", "-")

        actions = self.card(grid, "Actions")
        actions.grid(row=1, column=1, sticky="nsew", padx=(8, 0), pady=(8, 0))
        row = ttk.Frame(actions, style="Panel.TFrame")
        row.pack(fill="x", pady=(10, 0))
        self.act(row, "Refresh", self.refresh_dashboard).pack(side="left")
        self.act(row, "Say the time", lambda: self.quick("say")).pack(
            side="left", padx=8)
        self.act(row, "Sync now", lambda: self.quick("sync")).pack(side="left")
        row2 = ttk.Frame(actions, style="Panel.TFrame")
        row2.pack(fill="x", pady=(10, 0))
        self.act(row2, "Screenshot to SD",
                 lambda: self.quick("screenshot /sdcard/shot.bmp")).pack(
                     side="left")
        self.act(row2, "Reboot", self.reboot).pack(side="left", padx=8)

    def act(self, parent, text, cmd, style="TButton"):
        return ttk.Button(parent, text=text, command=cmd, style=style)

    def add_row(self, parent, label, value):
        row = ttk.Frame(parent, style="Panel.TFrame")
        row.pack(fill="x", pady=(8, 0))
        ttk.Label(row, text=label, style="PanelMuted.TLabel", width=11).pack(
            side="left")
        var = tk.StringVar(value=value)
        ttk.Label(row, textvariable=var, style="Value.TLabel").pack(
            side="left", fill="x", expand=True)
        return var

    def build_console(self):
        bar = ttk.Frame(self.tab_console)
        bar.pack(fill="x", pady=(0, 8))
        for text, cmd in (("Help", "help"), ("Status", "status"),
                          ("Version", "version"), ("PMU", "pmu"),
                          ("Config", "config"), ("Files", "ls /sdcard"),
                          ("Test audio", "test audio"),
                          ("Save log", None),
                          ("Clear", None)):
            if text == "Clear":
                ttk.Button(bar, text=text, command=self.clear_log).pack(
                    side="left", padx=(0, 6))
            elif text == "Save log":
                ttk.Button(bar, text=text, command=self.save_log).pack(
                    side="left", padx=(0, 6))
            else:
                ttk.Button(bar, text=text,
                           command=lambda c=cmd: self.quick(c)).pack(
                               side="left", padx=(0, 6))

        self.log_text = tk.Text(self.tab_console, bg="#0f1114", fg=FG,
                                insertbackground=FG, font=("Consolas", 10),
                                relief="flat", wrap="word", padx=10, pady=8)
        self.log_text.pack(fill="both", expand=True)
        self.log_text.configure(state="disabled")

        entry_row = ttk.Frame(self.tab_console)
        entry_row.pack(fill="x", pady=(8, 0))
        self.cmd_var = tk.StringVar()
        entry = ttk.Entry(entry_row, textvariable=self.cmd_var,
                          font=("Consolas", 10))
        entry.pack(side="left", fill="x", expand=True)
        entry.bind("<Return>", lambda _e: self.send_console())
        self.console_entry = entry
        ttk.Button(entry_row, text="Send", style="Accent.TButton",
                   command=self.send_console).pack(side="left", padx=(8, 0))

    # -------------------------------------------------------------- settings

    def scrollable(self, parent):
        canvas = tk.Canvas(parent, bg=BG, highlightthickness=0)
        bar = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        inner = ttk.Frame(canvas)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")

        def resize(_event):
            canvas.configure(scrollregion=canvas.bbox("all"))

        def widen(event):
            canvas.itemconfigure(window, width=event.width)

        def wheel(event):
            canvas.yview_scroll(int(-event.delta / 120), "units")

        inner.bind("<Configure>", resize)
        canvas.bind("<Configure>", widen)
        canvas.bind("<Enter>", lambda _e: canvas.bind_all("<MouseWheel>", wheel))
        canvas.bind("<Leave>", lambda _e: canvas.unbind_all("<MouseWheel>"))
        canvas.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        canvas.configure(yscrollcommand=bar.set)
        return inner

    def build_settings(self):
        self.s_vars = {}
        self.s_fields = {}
        self.s_modified = False
        self.s_loading = False

        bar = ttk.Frame(self.tab_settings)
        bar.pack(fill="x", pady=(0, 8))
        ttk.Button(bar, text="Reload file", command=self.settings_reload).pack(
            side="left")
        ttk.Button(bar, text="Save file", command=self.settings_save).pack(
            side="left", padx=8)
        ttk.Button(bar, text="Save and push", style="Accent.TButton",
                   command=self.settings_push).pack(side="left")
        ttk.Button(bar, text="Reboot to apply",
                   command=self.reboot).pack(side="left", padx=8)
        self.s_state = ttk.Label(bar, text="", style="Muted.TLabel")
        self.s_state.pack(side="left", padx=12)

        ttk.Label(
            self.tab_settings,
            text="%s - the file the watch reads at boot. Every value is "
                 "checked against the firmware's range before it is written. "
                 "Push writes only mosu.cfg; reboot applies it." % SD_CFG,
            style="Muted.TLabel", wraplength=900, justify="left").pack(
                anchor="w", pady=(0, 8))

        pages = ttk.Notebook(self.tab_settings)
        pages.pack(fill="both", expand=True)
        titles = [
            ("device", "Device"), ("display", "Display"), ("colors", "Colours"),
            ("status", "Status"), ("clock", "Clock and network"),
            ("wifi", "Wi-Fi"), ("announce", "Voice"),
        ]
        for section, title in titles:
            page = ttk.Frame(pages, padding=10)
            pages.add(page, text=title)
            inner = self.scrollable(page)
            for field in [f for f in SCHEMA if f.section == section]:
                self.add_field(inner, field)

        self.settings_reload(silent=True)

    def add_field(self, parent, field):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=(7, 10))
        row.columnconfigure(0, weight=1)

        text = ttk.Frame(row)
        text.grid(row=0, column=0, sticky="w")
        tk.Label(text, text=field.label, bg=BG, fg=FG,
                 font=("Segoe UI", 10)).pack(anchor="w")
        if field.hint:
            tk.Label(text, text=field.hint, bg=BG, fg=MUTED,
                     font=("Segoe UI", 8), wraplength=620,
                     justify="left").pack(anchor="w")

        holder = ttk.Frame(row)
        holder.grid(row=0, column=1, sticky="e")
        key = (field.section, field.key)

        if field.kind == "bool":
            var = tk.BooleanVar()
            ttk.Checkbutton(holder, variable=var).pack(side="right")
        elif field.kind == "int":
            var = tk.StringVar()
            ttk.Spinbox(holder, from_=field.lo, to=field.hi,
                        increment=field.step, textvariable=var,
                        width=9).pack(side="right")
        elif field.kind == "enum":
            var = tk.StringVar()
            ttk.Combobox(holder, textvariable=var, values=field.choices,
                         state="readonly", width=10).pack(side="right")
        elif field.kind == "color":
            var = tk.StringVar()
            swatch = tk.Canvas(holder, width=30, height=20, bg=BG,
                               highlightthickness=0)
            swatch.pack(side="right", padx=(6, 0))
            ttk.Button(holder, text="Pick",
                       command=lambda v=var: self.pick_colour(v)).pack(
                           side="right", padx=(6, 0))
            ttk.Entry(holder, textvariable=var, width=9).pack(side="right")
            var.trace_add("write", lambda *_a, c=swatch, v=var:
                          self.paint_swatch(c, v))
            self.paint_swatch(swatch, var)
        else:
            var = tk.StringVar()
            ttk.Entry(holder, textvariable=var, width=40,
                      show="*" if field.secret else "").pack(side="right")

        var.trace_add("write", lambda *_a: self.settings_touched())
        self.s_vars[key] = var
        self.s_fields[key] = field

    def paint_swatch(self, canvas, var):
        try:
            canvas.configure(bg=var.get() or BG)
        except tk.TclError:
            canvas.configure(bg=BG)

    def pick_colour(self, var):
        _rgb, chosen = colorchooser.askcolor(color=var.get() or "#FFFFFF",
                                             parent=self.root,
                                             title="Colour")
        if chosen:
            var.set(chosen.upper())

    def settings_touched(self):
        if getattr(self, "s_loading", False):
            return
        if self.s_modified:
            return
        self.s_modified = True
        self.s_state.configure(text="modified", foreground=AMBER)

    def settings_reload(self, silent=False):
        if self.s_modified and not silent:
            if not messagebox.askyesno("MosuTime Manager",
                                       "Discard the changes in the editor?"):
                return
        if not os.path.isfile(SD_CFG):
            messagebox.showerror("MosuTime Manager",
                                 "%s was not found." % SD_CFG)
            return
        cfg = ConfigFile(SD_CFG)
        self.s_loading = True
        for (section, key), var in self.s_vars.items():
            field = self.s_fields[(section, key)]
            value = parse_raw(field, cfg.get(section, key))
            if field.kind == "bool":
                var.set(bool(value))
            elif field.kind == "int":
                var.set(str(value))
            else:
                var.set(value)
        self.s_loading = False
        self.s_modified = False
        self.s_state.configure(text="loaded", foreground=GREEN)

    def settings_values(self):
        """Validate every field; returns (values, errors)."""
        values = {}
        errors = []
        for (section, key), var in self.s_vars.items():
            field = self.s_fields[(section, key)]
            if field.kind == "bool":
                values[(section, key)] = bool(var.get())
                continue
            text = str(var.get()).strip()
            if field.kind == "int":
                try:
                    number = int(text)
                except ValueError:
                    errors.append("%s: not a number" % field.label)
                    continue
                if not (field.lo <= number <= field.hi):
                    errors.append("%s: %d..%d" % (field.label, field.lo,
                                                  field.hi))
                    continue
                values[(section, key)] = number
            elif field.kind == "color":
                colour = text.upper()
                if not re.fullmatch(r"#[0-9A-F]{6}", colour):
                    errors.append("%s: use #RRGGBB" % field.label)
                    continue
                values[(section, key)] = colour
            else:
                if '"' in text or "\n" in text:
                    errors.append("%s: quotes and newlines are not allowed"
                                  % field.label)
                    continue
                if len(text) > 160:
                    errors.append("%s: too long" % field.label)
                    continue
                values[(section, key)] = text
        if not errors:
            green = values[("status", "battery_green_min")]
            orange = values[("status", "battery_orange_min")]
            if orange > green:
                errors.append("Status: orange threshold must not exceed green")
        return values, errors

    def settings_save(self, silent=False):
        values, errors = self.settings_values()
        if errors:
            messagebox.showerror("MosuTime Manager",
                                 "Fix these values first:\n\n" +
                                 "\n".join(errors[:12]))
            return False
        cfg = ConfigFile(SD_CFG)
        for (section, key), value in values.items():
            field = self.s_fields[(section, key)]
            cfg.set(section, key, format_value(field, value))
        cfg.save()
        self.s_modified = False
        self.s_state.configure(text="saved", foreground=GREEN)
        if not silent:
            self.log("settings saved to %s" % SD_CFG)
        return True

    def settings_push(self):
        if not self.device.open:
            messagebox.showinfo("MosuTime Manager",
                                "Connect to the watch first; the file travels "
                                "over the USB console.")
            return
        if not self.settings_save(silent=True):
            return
        with open(SD_CFG, "rb") as handle:
            data = handle.read()

        def work():
            self.device.send("md /sdcard")
            self.device.drain(0.3)
            ok = self.push_file("/sdcard/mosu.cfg", data, "mosu.cfg")
            self.root.after(0, self.settings_pushed, ok)

        self.run_worker(work, "Pushing mosu.cfg...")

    def settings_pushed(self, ok):
        self.s_state.configure(text="pushed" if ok else "push failed",
                               foreground=GREEN if ok else RED)
        if ok and messagebox.askyesno(
                "MosuTime Manager",
                "mosu.cfg is on the card.\n\nReboot the watch now to apply it?"):
            self.quick("reboot")

    def build_firmware(self):
        intro = ttk.Label(
            self.tab_firmware,
            text="Builds and flashes through tools/build.ps1, the same path as "
                 "the command line.\nFlashing needs the watch on the selected "
                 "port; NVS and the SD card are kept.",
            style="Muted.TLabel", justify="left")
        intro.pack(anchor="w", pady=(0, 10))

        bar = ttk.Frame(self.tab_firmware)
        bar.pack(fill="x", pady=(0, 8))
        ttk.Button(bar, text="Build", command=self.build_only).pack(
            side="left")
        ttk.Button(bar, text="Build and flash", style="Accent.TButton",
                   command=self.build_flash).pack(side="left", padx=8)
        ttk.Button(bar, text="Open build folder",
                   command=self.open_build).pack(side="left")
        ttk.Button(bar, text="Factory reset (NVS)", style="Danger.TButton",
                   command=self.factory_reset).pack(side="left", padx=8)
        self.fw_state = ttk.Label(bar, text="", style="Muted.TLabel")
        self.fw_state.pack(side="left", padx=12)

        self.fw_text = tk.Text(self.tab_firmware, bg="#0f1114", fg=FG,
                               insertbackground=FG, font=("Consolas", 9),
                               relief="flat", wrap="none", padx=10, pady=8)
        self.fw_text.pack(fill="both", expand=True)
        self.fw_text.configure(state="disabled")

    def build_sd(self):
        ttk.Label(
            self.tab_sd,
            text="The card holds mosu.cfg and the voice clips. Edit the "
                 "settings in the Settings tab first; this pushes the saved "
                 "file. Formatting repairs a card that a PC formatter made "
                 "unreadable and erases everything on it.",
            style="Muted.TLabel", wraplength=880, justify="left").pack(
                anchor="w", pady=(0, 10))

        bar = ttk.Frame(self.tab_sd)
        bar.pack(fill="x", pady=(0, 8))
        ttk.Button(bar, text="Format card", style="Danger.TButton",
                   command=self.format_card).pack(side="left")
        ttk.Button(bar, text="Push config and voice", style="Accent.TButton",
                   command=self.push_sd).pack(side="left", padx=8)
        ttk.Button(bar, text="List files",
                   command=lambda: self.quick("ls /sdcard")).pack(side="left")
        ttk.Button(bar, text="Check voice",
                   command=self.check_voice).pack(side="left", padx=8)
        self.sd_state = ttk.Label(bar, text="", style="Muted.TLabel")
        self.sd_state.pack(side="left", padx=12)

        self.sd_text = tk.Text(self.tab_sd, bg="#0f1114", fg=FG,
                               insertbackground=FG, font=("Consolas", 9),
                               relief="flat", wrap="none", padx=10, pady=8)
        self.sd_text.pack(fill="both", expand=True)

    def build_power(self):
        card = self.card(self.tab_power, "Battery")
        card.pack(fill="x")
        self.p_batt = tk.StringVar(value="-")
        ttk.Label(card, textvariable=self.p_batt, style="Big.TLabel").pack(
            anchor="w", pady=(8, 0))
        self.p_detail = tk.StringVar(value="")
        ttk.Label(card, textvariable=self.p_detail,
                  style="PanelMuted.TLabel").pack(anchor="w", pady=(4, 10))
        self.p_bar = ttk.Progressbar(card, mode="determinate", maximum=100,
                                     length=420)
        self.p_bar.pack(anchor="w")

        note = ttk.Label(
            self.tab_power,
            text="\nThe watch is meant to stay on. To switch it off, hold the "
                 "side PWR button for 10 seconds.\nA short press only wakes it; "
                 "shorter holds are ignored on purpose.\n\nThe button below is "
                 "the same request from the console: it saves the clock, turns "
                 "the screen off and cuts the rails.",
            style="Muted.TLabel", justify="left")
        note.pack(anchor="w", pady=(16, 10))
        ttk.Button(self.tab_power, text="Shut down now", style="Danger.TButton",
                   command=self.shutdown).pack(anchor="w")

    # -------------------------------------------------------------- helpers

    def push_log(self, line):
        self.lines.put(line)

    def log(self, text):
        self.push_log(text)

    def tick(self):
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                break
            self.log_text.configure(state="normal")
            self.log_text.insert("end", line + "\n")
            self.log_text.configure(state="disabled")
            self.log_text.see("end")
        self.device.pump()
        self.root.after(80, self.tick)

    def set_status(self, text, colour=MUTED):
        self.status.configure(text=text, foreground=colour)

    def run_worker(self, fn, label):
        if self.device.busy:
            messagebox.showinfo("MosuTime Manager",
                                "An operation is already running.")
            return
        if not self.device.open:
            messagebox.showinfo("MosuTime Manager", "Connect to the watch first.")
            return
        self.device.busy = True
        self.set_status(label, ACCENT)
        self.progress.configure(mode="indeterminate")
        self.progress.start(12)

        def wrap():
            try:
                fn()
            except Exception as exc:  # noqa: BLE001 - surface serial errors
                self.log("!! %s" % exc)
            finally:
                self.device.busy = False
                self.root.after(0, self.worker_done)

        self.worker = threading.Thread(target=wrap, daemon=True)
        self.worker.start()

    def worker_done(self):
        self.progress.stop()
        self.progress.configure(mode="determinate", value=0)
        self.set_status("Ready" if self.device.open else "Not connected",
                        GREEN if self.device.open else MUTED)

    # ----------------------------------------------------------- connection

    def refresh_ports(self):
        ports = find_ports()
        labels = [label for _dev, label, _vid, _pid in ports]
        self.ports = ports
        self.port_box["values"] = labels
        if labels:
            chosen = 0
            for index, (_dev, label, vid, pid) in enumerate(ports):
                if vid == 0x303A:
                    chosen = index
                    break
            self.port_box.current(chosen)

    def current_port(self):
        index = self.port_box.current()
        if index < 0 or index >= len(self.ports):
            return None
        return self.ports[index][0]

    def toggle_connect(self):
        if self.device.open:
            self.device.close()
            self.connect_btn.configure(text="Connect")
            self.dot.itemconfigure(self.dot_id, fill="#555b64")
            self.set_status("Disconnected")
            return
        port = self.current_port()
        if not port:
            messagebox.showinfo("MosuTime Manager", "No serial port found.")
            return
        self.set_status("Connecting to %s..." % port, ACCENT)

        def work():
            self.device.open_port(port)
            time.sleep(1.5)
            for _ in range(10):
                self.device.send("help")
                if self.device.wait_line("commands:", 0.8):
                    break
            else:
                raise RuntimeError("the watch did not answer on %s" % port)
            self.root.after(0, self.connected, port)

        self.device.busy = True
        threading.Thread(target=lambda: self._connect_wrap(work),
                         daemon=True).start()

    def _connect_wrap(self, work):
        try:
            work()
        except Exception as exc:  # noqa: BLE001
            self.device.close()
            self.root.after(0, self.connect_failed, str(exc))
        finally:
            self.device.busy = False

    def connected(self, port):
        self.connect_btn.configure(text="Disconnect")
        self.dot.itemconfigure(self.dot_id, fill=GREEN)
        self.set_status("Connected to %s" % port, GREEN)
        self.refresh_dashboard()

    def connect_failed(self, text):
        self.connect_btn.configure(text="Connect")
        self.dot.itemconfigure(self.dot_id, fill=RED)
        self.set_status("Connection failed", RED)
        messagebox.showerror("MosuTime Manager", text)

    # ------------------------------------------------------------- dashboard

    def quick(self, cmd):
        if not self.device.open:
            messagebox.showinfo("MosuTime Manager", "Connect to the watch first.")
            return
        self.device.send(cmd)
        self.tabs.select(1)

    def clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def save_log(self):
        path = filedialog.asksaveasfilename(
            title="Save the console log", defaultextension=".txt",
            initialfile="mosu-console.txt",
            filetypes=[("Text", "*.txt"), ("All files", "*.*")])
        if not path:
            return
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(self.log_text.get("1.0", "end"))
        self.set_status("Log saved to %s" % path, GREEN)

    def send_console(self):
        cmd = self.cmd_var.get().strip()
        if not cmd:
            return
        if not self.device.open:
            messagebox.showinfo("MosuTime Manager", "Connect to the watch first.")
            return
        self.cmd_var.set("")
        self.device.send(cmd)

    def refresh_dashboard(self):
        if not self.device.open:
            return

        def work():
            self.device.drain(0.2)
            out = []
            for cmd, settle in (("version", 0.9), ("status", 1.1)):
                self.device.send(cmd)
                out += self.device.capture(settle)
            self.root.after(0, self.parse_dashboard, out)

        self.run_worker(work, "Reading the watch...")

    def parse_dashboard(self, lines):
        app = idf = mac = chip = ""
        clock = tz = net = sync = batt = charge = sd = audio = ""
        for line in lines:
            s = line.strip()
            if s.startswith("app    :"):
                app = s.split(":", 1)[1].strip()
            elif s.startswith("idf    :"):
                idf = s.split(":", 1)[1].strip()
            elif s.startswith("mac    :"):
                mac = s.split(":", 1)[1].strip()
            elif s.startswith("chip   :"):
                chip = s.split(":", 1)[1].strip()
            elif s.startswith("config :"):
                sd = "mounted" if "SD mounted" in s else "missing"
            elif s.startswith("clock  :"):
                clock = s.split(":", 1)[1].strip()
            elif s.startswith("net    :"):
                net = s.split(":", 1)[1].strip()
            elif s.startswith("power  :"):
                batt = s.split(":", 1)[1].strip()
            elif s.startswith("audio  :"):
                audio = s.split(":", 1)[1].strip()
            elif s.startswith("display :"):
                pass
            elif s.startswith("voice"):
                pass
        if app:
            self.d_app.set(app)
        if idf:
            self.d_idf.set(idf)
        if mac:
            self.d_mac.set(mac)
        if chip:
            self.d_chip.set(chip)
        if clock:
            parts = [p.strip() for p in clock.split(",")]
            self.d_clock.set(parts[0].capitalize())
            for p in parts:
                if p.startswith("tz "):
                    self.d_tz.set(p[3:])
                if p.startswith("sync age"):
                    self.d_sync.set(p[9:])
        if net:
            self.d_net.set(net)
        if batt:
            self.d_batt.set(batt.split(",")[0])
            self.d_charge.set(",".join(batt.split(",")[1:]).strip())
            self.update_power(batt)
        if sd:
            self.d_sd.set("mounted" if sd == "mounted" else "not mounted")
        if audio:
            self.d_audio.set(audio)
        if not (app or clock or batt):
            self.set_status("The watch sent no status; is the console free?",
                            AMBER)

    def update_power(self, line):
        try:
            mv = int(line.split("mV")[0].split()[-1])
            pct = int(line.split(",")[1].strip().split("%")[0])
        except (ValueError, IndexError):
            return
        self.p_batt.set("%d%%  %d mV" % (pct, mv))
        self.p_detail.set(line)
        self.p_bar["value"] = pct

    def reboot(self):
        if messagebox.askyesno("MosuTime Manager", "Reboot the watch?"):
            self.quick("reboot")

    # -------------------------------------------------------------- firmware

    def build_only(self):
        self.run_build(flash=False)

    def build_flash(self):
        port = self.current_port()
        if not port:
            messagebox.showinfo("MosuTime Manager", "No serial port selected.")
            return
        if not messagebox.askyesno(
                "MosuTime Manager",
                "Build and flash the firmware to %s?\n\n"
                "The watch restarts and the screen goes dark for a few seconds. "
                "NVS and the SD card are kept." % port):
            return
        self.run_build(flash=True, port=port)

    def run_build(self, flash, port=None):
        if not os.path.isfile(BUILD_SCRIPT):
            messagebox.showerror("MosuTime Manager",
                                 "tools/build.ps1 was not found.")
            return
        if self.device.busy:
            messagebox.showinfo("MosuTime Manager",
                                "An operation is already running.")
            return
        # esptool needs the port to itself.
        was_open = self.device.open
        if was_open:
            self.device.close()
            self.connect_btn.configure(text="Connect")
            self.dot.itemconfigure(self.dot_id, fill="#555b64")
        self.device.busy = True          # keep the console pump quiet
        self.set_status("Flashing..." if flash else "Building...", ACCENT)
        self.fw_state.configure(text="running")
        self.progress.configure(mode="indeterminate")
        self.progress.start(12)

        def work():
            args = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", BUILD_SCRIPT]
            if flash:
                args += ["-Port", port, "-Flash"]
            self.append_fw("$ " + " ".join(args) + "\n")
            proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    encoding="utf-8", errors="replace",
                                    bufsize=1)
            for line in proc.stdout:
                self.append_fw(line.rstrip("\r\n"))
            code = proc.wait()
            self.append_fw("\n== %s ==\n" % ("PASS" if code == 0 else
                                             "FAIL (exit %d)" % code))
            self.root.after(0, self.build_done, code, was_open)

        threading.Thread(target=work, daemon=True).start()

    def append_fw(self, text):
        self.root.after(0, self._append_fw, text)

    def open_build(self):
        if hasattr(os, "startfile"):
            os.startfile(BUILD_DIR)  # noqa: S606 - opens Explorer
        else:
            filedialog.askdirectory(initialdir=BUILD_DIR)

    def _append_fw(self, text):
        self.fw_text.configure(state="normal")
        self.fw_text.insert("end", text + "\n")
        self.fw_text.see("end")
        self.fw_text.configure(state="disabled")

    def build_done(self, code, was_open=False):
        self.device.busy = False
        self.progress.stop()
        self.progress.configure(mode="determinate", value=0)
        self.fw_state.configure(
            text="last build: %s" % ("ok" if code == 0 else "failed"),
            foreground=GREEN if code == 0 else RED)
        self.set_status("Ready" if self.device.open else "Not connected",
                        GREEN if self.device.open else MUTED)
        if code != 0:
            messagebox.showerror("MosuTime Manager",
                                 "The build or flash failed. See the log.")
        elif was_open:
            self.root.after(1200, self.reconnect)

    def reconnect(self):
        if self.device.open or self.device.busy:
            return
        if not self.current_port():
            self.refresh_ports()
        self.toggle_connect()

    # ---------------------------------------------------------------- sd card

    def format_card(self):
        if not messagebox.askyesno(
                "MosuTime Manager",
                "Format the SD card?\n\nThis erases every file on it, "
                "including the voice clips. It is the reliable way to repair "
                "a card that a PC formatter made unusable."):
            return

        def work():
            self.device.drain(0.2)
            self.device.command("sd format yes", 0.5)
            self.device.wait_line("sd format: ESP_OK", 180)

        self.run_worker(work, "Formatting the card...")

    def push_sd(self):
        if not os.path.isfile(SD_CFG):
            messagebox.showerror("MosuTime Manager", "sd_card/mosu.cfg is missing.")
            return
        if not self.settings_save(silent=True):
            return
        names = []
        if os.path.isdir(SD_VOICE):
            names = sorted(n for n in os.listdir(SD_VOICE) if n.endswith(".wav"))
        total = sum(os.path.getsize(os.path.join(SD_VOICE, n)) for n in names)
        if not messagebox.askyesno(
                "MosuTime Manager",
                "Push mosu.cfg and %d voice clips (%d KiB) to the card?"
                % (len(names), total // 1024)):
            return

        def work():
            self.sd_state.configure(text="pushing...")
            self.device.drain(0.2)
            self.device.send("md /sdcard/voice")
            self.device.drain(0.3)
            with open(SD_CFG, "rb") as handle:
                cfg = handle.read()
            ok = self.push_file("/sdcard/mosu.cfg", cfg, "mosu.cfg")
            done = 0
            if ok and names:
                for index, name in enumerate(names, 1):
                    with open(os.path.join(SD_VOICE, name), "rb") as handle:
                        data = handle.read()
                    target = "/sdcard/voice/" + name
                    pushed = False
                    for attempt in range(2):
                        if self.push_file(target, data, name):
                            pushed = True
                            break
                        self.log("retrying %s" % name)
                        self.device.drain(1.0)
                    if not pushed:
                        ok = False
                        break
                    done += len(data)
                    if index % 10 == 0 or index == len(names):
                        self.progress.configure(
                            mode="determinate", maximum=100,
                            value=100.0 * index / len(names))
                        self.sd_state.configure(
                            text="%d/%d clips" % (index, len(names)))
            self.root.after(0, self.push_done, ok, len(names))

        self.run_worker(work, "Pushing files...")

    def check_voice(self):
        script = os.path.join(ROOT, "tools", "check_voice.py")
        if not os.path.isfile(script):
            messagebox.showerror("MosuTime Manager", "tools/check_voice.py is missing.")
            return

        def work():
            args = [sys.executable, script, SD_VOICE]
            proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    encoding="utf-8", errors="replace",
                                    bufsize=1)
            for line in proc.stdout:
                proc_line = line.rstrip("\r\n")
                self.root.after(0, self._append_sd, proc_line)
            code = proc.wait()
            self.root.after(0, self._append_sd,
                            "== %s ==" % ("PASS" if code == 0 else "FAIL"))

        threading.Thread(target=work, daemon=True).start()

    def _append_sd(self, text):
        self.sd_text.configure(state="normal")
        self.sd_text.insert("end", text + "\n")
        self.sd_text.see("end")
        self.sd_text.configure(state="disabled")

    def factory_reset(self):
        if not self.device.open:
            messagebox.showinfo("MosuTime Manager", "Connect to the watch first.")
            return
        if not messagebox.askyesno(
                "MosuTime Manager",
                "Erase the saved configuration and Wi-Fi calibration?\n\n"
                "The SD card is not touched. The watch reboots and reloads "
                "mosu.cfg from the card, or uses built-in defaults when there "
                "is no card."):
            return
        if not messagebox.askyesno(
                "MosuTime Manager",
                "Last check: really erase NVS now?"):
            return
        self.quick("nvs erase")

    def push_done(self, ok, count):
        self.sd_state.configure(text="done" if ok else "failed",
                                foreground=GREEN if ok else RED)
        if ok:
            self.quick("ls /sdcard/voice")

    def push_file(self, target, data, label):
        self.device.send("receive %s %d /crc" % (target, len(data)))
        if not self.device.wait_line(READY, 15):
            self.log("!! no READY for %s" % label)
            return False
        sent = 0
        while sent < len(data):
            chunk = data[sent:sent + CHUNK]
            self.device.write(chunk)
            sent += len(chunk)
            line = self.device.read_line(8.0)
            while line is not None and not line.startswith("RX "):
                if line.startswith("receive:"):
                    return False
                line = self.device.read_line(8.0)
            if line is None:
                self.log("!! no ACK for %s at %d" % (label, sent))
                return False
            if int(line[3:]) != sent:
                self.log("!! bad ACK for %s" % label)
                return False
        self.device.write(struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF))
        return self.device.wait_line(DONE, 20)

    # ------------------------------------------------------------------ power

    def shutdown(self):
        if not messagebox.askyesno(
                "MosuTime Manager",
                "Shut the watch down now?\n\nPress PWR briefly to turn it "
                "back on."):
            return
        self.quick("poweroff")

    def on_close(self):
        self.device.close()
        self.root.destroy()


def selftest():
    root = tk.Tk()
    app = App(root)
    root.update()
    app.parse_dashboard([
        "app    : MosuTime test, built today",
        "idf    : v5.5.5",
        "mac    : 44:BD:8D:77:8A:60",
        "chip   : ESP32-S3 rev 0.2, 2 core(s)",
        "config : MosuTime (SD mounted)",
        "clock  : valid, tz Europe/Bucharest, sync age recent",
        "net    : state 2, ip 192.168.1.252, rssi -77",
        "power  : 4180 mV, 98%, charging 1, vbus 1",
        "audio  : idle",
    ])
    root.update()
    assert app.d_batt.get() == "4180 mV", app.d_batt.get()
    assert app.p_batt.get() == "98%  4180 mV", app.p_batt.get()
    assert app.p_bar["value"] == 98
    assert app.d_tz.get() == "Europe/Bucharest"
    assert app.d_sd.get() == "mounted"
    cfg = ConfigFile(SD_CFG)
    original = cfg.get("display", "brightness")
    cfg.set("display", "brightness", "42")
    assert cfg.get("display", "brightness") == "42"
    cfg.set("display", "brightness", original)
    values, errors = app.settings_values()
    assert not errors, errors
    assert values[("display", "brightness")] == int(original)
    root.destroy()
    print("selftest: OK")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    root = tk.Tk()
    app = App(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
