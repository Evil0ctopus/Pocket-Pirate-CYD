#!/usr/bin/env python3
"""Drive the Pocket Pirate firmware over USB serial and capture screenshots.

Uses the firmware's serial test hooks (TAP/SHOT/INFO). Screenshots decode the
raw RGB565 canvas dump into PNGs in device_shots/.

Usage:
    python tools/device_test.py --port COM3
    python tools/device_test.py --port /dev/ttyACM0
    python tools/device_test.py              # auto-detect if exactly one USB port

Requires: pip install -r tools/requirements.txt  (pyserial, Pillow)
"""

import argparse
import os
import sys
import time

import serial
from serial.tools import list_ports
from PIL import Image

OUT = "device_shots"


def is_likely_usb_serial(port_info):
    """Prefer USB CDC/UART adapters; skip bare platform ports like ttyS0."""
    if port_info.vid is not None:
        return True
    name = port_info.device.rsplit("/", 1)[-1].lower()
    if name.startswith("com") and name[3:].isdigit():
        return True
    return name.startswith((
        "ttyacm",
        "ttyusb",
        "cu.usb",
        "cu.wchusbserial",
        "cu.usbserial",
        "cu.usbmodem",
    ))


def resolve_port(explicit):
    """Return an explicit port, or auto-detect when exactly one USB serial port exists."""
    if explicit:
        return explicit

    all_ports = list(list_ports.comports())
    ports = [p for p in all_ports if is_likely_usb_serial(p)]

    if not ports:
        lines = [
            "No USB serial ports found.",
            "Connect the CYD over USB, then pass --port explicitly, e.g.:",
            "  python tools/device_test.py --port COM3              # Windows",
            "  python tools/device_test.py --port /dev/ttyACM0      # Linux",
            "  python tools/device_test.py --port /dev/cu.usbmodem* # macOS",
        ]
        if all_ports:
            lines.append("Non-USB ports seen (not auto-selected):")
            for p in all_ports:
                lines.append(f"  {p.device}: {p.description}")
        sys.exit("\n".join(lines))

    if len(ports) == 1:
        chosen = ports[0].device
        print(f"Auto-detected serial port: {chosen} ({ports[0].description})")
        return chosen

    print("Multiple USB serial ports found; pass --port explicitly:")
    for p in ports:
        print(f"  {p.device}: {p.description}")
    sys.exit(
        "Usage: python tools/device_test.py --port <PORT>\n"
        "Example: python tools/device_test.py --port COM3"
    )


class Dev:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=3)
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def line(self):
        return self.s.readline().decode(errors="replace").rstrip()

    def cmd(self, text):
        self.s.write((text + "\n").encode())

    def info(self):
        self.cmd("INFO")
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.line()
            if ln.startswith("INFO"):
                return ln
        return "(no INFO reply)"

    def tap(self, x, y, settle=0.45):
        self.cmd(f"TAP {x} {y}")
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.line()
            if ln.startswith("OK TAP"):
                break
        time.sleep(settle)

    def shot(self, name):
        self.s.reset_input_buffer()
        self.cmd("SHOT")
        w = h = 0
        t0 = time.time()
        while time.time() - t0 < 5:
            ln = self.line()
            if ln.startswith("SHOT_BEGIN"):
                _, w, h = ln.split()
                w, h = int(w), int(h)
                break
        if not w:
            print(f"  !! no SHOT_BEGIN for {name}")
            return None
        raw = self.s.read(w * h * 2)
        if len(raw) != w * h * 2:
            print(f"  !! short read {len(raw)} for {name}")
            return None
        # LovyanGFX 16bpp sprites store panel byte order (big-endian RGB565)
        img = Image.new("RGB", (w, h))
        px = img.load()
        for i in range(w * h):
            v = (raw[2 * i] << 8) | raw[2 * i + 1]
            px[i % w, i // w] = (((v >> 11) & 0x1F) << 3,
                                 ((v >> 5) & 0x3F) << 2,
                                 (v & 0x1F) << 3)
        path = os.path.join(OUT, name + ".png")
        img.save(path)
        print(f"  shot -> {path}")
        return img


def main():
    parser = argparse.ArgumentParser(
        description="Drive Pocket Pirate firmware over USB serial and capture screenshots."
    )
    parser.add_argument(
        "--port", "-p",
        help="Serial port (e.g. COM3, /dev/ttyACM0). Auto-detects if exactly one USB serial port is present.",
    )
    args = parser.parse_args()

    port = resolve_port(args.port)
    os.makedirs(OUT, exist_ok=True)

    d = Dev(port)
    print(d.info())
    d.shot("01_boot")


if __name__ == "__main__":
    main()
