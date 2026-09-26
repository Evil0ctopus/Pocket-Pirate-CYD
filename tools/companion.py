#!/usr/bin/env python3
"""USB CDC companion for Pocket Pirate CYD.

Talks the firmware line/JSON protocol over serial:
  INFO / STATUS (JSON) / LOGS / LOGGET <path> / HELP / TAP / SHOT

Usage:
  python tools/companion.py --port COM3 status
  python tools/companion.py status          # auto-detect single USB serial
  python tools/companion.py info
  python tools/companion.py logs
  python tools/companion.py logget /wigle.csv -o wigle.csv
  python tools/companion.py watch          # poll STATUS every 2s

Requires: pip install -r tools/requirements.txt
"""

from __future__ import annotations

import argparse
import json
import sys
import time

import serial
from serial.tools import list_ports

# Reuse port resolution from device_test when available.
try:
    from device_test import resolve_port
except ImportError:
    def is_likely_usb_serial(port_info):
        if port_info.vid is not None:
            return True
        name = port_info.device.rsplit("/", 1)[-1].lower()
        if name.startswith("com") and name[3:].isdigit():
            return True
        return name.startswith((
            "ttyacm", "ttyusb", "cu.usb", "cu.wchusbserial",
            "cu.usbserial", "cu.usbmodem",
        ))

    def resolve_port(explicit):
        if explicit:
            return explicit
        ports = [p for p in list_ports.comports() if is_likely_usb_serial(p)]
        if len(ports) == 1:
            print(f"Auto-detected serial port: {ports[0].device}")
            return ports[0].device
        if not ports:
            sys.exit("No USB serial ports found. Pass --port.")
        print("Multiple USB serial ports; pass --port:")
        for p in ports:
            print(f"  {p.device}: {p.description}")
        sys.exit(1)


class Companion:
    def __init__(self, port: str, baud: int = 115200):
        self.s = serial.Serial(port, baud, timeout=3)
        time.sleep(0.35)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def readline(self) -> str:
        return self.s.readline().decode(errors="replace").rstrip("\r\n")

    def cmd(self, text: str):
        self.s.write((text + "\n").encode())

    def info(self) -> str:
        self.cmd("INFO")
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.readline()
            if ln.startswith("INFO"):
                return ln
        return "(no INFO reply)"

    def status(self) -> dict:
        self.cmd("STATUS")
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.readline()
            if ln.startswith("{"):
                return json.loads(ln)
            if ln.startswith("ERR"):
                raise RuntimeError(ln)
        raise TimeoutError("no STATUS JSON reply")

    def logs(self) -> list[tuple[str, int]]:
        self.cmd("LOGS")
        out = []
        t0 = time.time()
        seen_begin = False
        while time.time() - t0 < 5:
            ln = self.readline()
            if ln.startswith("LOGS err"):
                raise RuntimeError(ln)
            if ln == "LOGS_BEGIN":
                seen_begin = True
                continue
            if ln.startswith("LOGS_END"):
                break
            if seen_begin and ln.startswith("LOG "):
                parts = ln.split()
                if len(parts) >= 3:
                    out.append((parts[1], int(parts[2])))
        return out

    def logget(self, path: str, dest: str):
        self.s.reset_input_buffer()
        self.cmd(f"LOGGET {path}")
        t0 = time.time()
        size = None
        while time.time() - t0 < 5:
            ln = self.readline()
            if ln.startswith("LOGGET err"):
                raise RuntimeError(ln)
            if ln.startswith("LOGGET_BEGIN"):
                # LOGGET_BEGIN path=/wigle.csv size=123
                for tok in ln.split():
                    if tok.startswith("size="):
                        size = int(tok.split("=", 1)[1])
                break
        if size is None:
            raise TimeoutError("no LOGGET_BEGIN")
        data = self.s.read(size)
        # drain until LOGGET_END
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.readline()
            if "LOGGET_END" in ln:
                break
        with open(dest, "wb") as f:
            f.write(data)
        return len(data)


def main():
    ap = argparse.ArgumentParser(description="Pocket Pirate USB companion")
    ap.add_argument("--port", "-p", help="Serial port (auto if exactly one USB)")
    ap.add_argument(
        "command",
        choices=["info", "status", "logs", "logget", "watch", "help"],
        help="Companion command",
    )
    ap.add_argument("path", nargs="?", help="Path for logget")
    ap.add_argument("-o", "--output", help="Output file for logget")
    args = ap.parse_args()

    port = resolve_port(args.port)
    c = Companion(port)
    try:
        if args.command == "info":
            print(c.info())
        elif args.command == "status":
            print(json.dumps(c.status(), indent=2))
        elif args.command == "logs":
            for name, size in c.logs():
                print(f"{size:8d}  {name}")
        elif args.command == "logget":
            if not args.path:
                sys.exit("logget requires a path, e.g. /wigle.csv")
            dest = args.output or args.path.lstrip("/").replace("/", "_")
            n = c.logget(args.path, dest)
            print(f"Wrote {n} bytes -> {dest}")
        elif args.command == "watch":
            while True:
                try:
                    st = c.status()
                    print(
                        f"{time.strftime('%H:%M:%S')} bat={st.get('bat_pct')}% "
                        f"usb={st.get('usb')} wifi={st.get('wifi')} "
                        f"ble={st.get('ble')} gps={st.get('gps')} "
                        f"bri={st.get('bri')}"
                    )
                except Exception as e:
                    print(f"watch error: {e}")
                time.sleep(2)
        elif args.command == "help":
            c.cmd("HELP")
            t0 = time.time()
            while time.time() - t0 < 2:
                ln = c.readline()
                if ln:
                    print(ln)
                    if ln.startswith("HELP"):
                        break
    finally:
        c.close()


if __name__ == "__main__":
    main()
