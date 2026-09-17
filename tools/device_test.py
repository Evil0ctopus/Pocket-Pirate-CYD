#!/usr/bin/env python3
"""Drive the Pocket Pirate firmware over USB serial and capture screenshots.

Uses the firmware's serial test hooks (TAP/SHOT/INFO). Screenshots decode the
raw RGB565 canvas dump into PNGs in device_shots/.

Usage: python tools/device_test.py [port]     # default COM16
"""

import os
import sys
import time

import serial
from PIL import Image

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"
OUT = "device_shots"
os.makedirs(OUT, exist_ok=True)


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
    d = Dev(PORT)
    print(d.info())
    d.shot("01_boot")


if __name__ == "__main__":
    main()
