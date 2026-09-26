#!/usr/bin/env python3
"""USB CDC companion for Pocket Pirate CYD.

Talks the firmware line/JSON protocol over serial:
  INFO / STATUS / LOGS / LOGGET / WIFICFG / OTAURL / OTARUN / SLEEP / HELP / TAP / SHOT

Also offline WiGLE CSV helpers (no device required):
  summarize <file.csv>
  export <file.csv> --format slim|geojson|unique-ssid

Usage:
  python tools/companion.py --port COM3 status
  python tools/companion.py status          # auto-detect single USB serial
  python tools/companion.py logs
  python tools/companion.py logget /wigle.csv -o wigle.csv
  python tools/companion.py summarize wigle.csv
  python tools/companion.py export wigle.csv --format slim -o slim.csv
  python tools/companion.py watch

Requires: pip install -r tools/requirements.txt
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

import serial
from serial.tools import list_ports

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
                for tok in ln.split():
                    if tok.startswith("size="):
                        size = int(tok.split("=", 1)[1])
                break
        if size is None:
            raise TimeoutError("no LOGGET_BEGIN")
        data = self.s.read(size)
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.readline()
            if "LOGGET_END" in ln:
                break
        with open(dest, "wb") as f:
            f.write(data)
        return len(data)

    def wificfg(self, ssid: str, password: str) -> str:
        self.cmd(f"WIFICFG {ssid}|{password}")
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.readline()
            if ln.startswith("OK WIFICFG") or ln.startswith("ERR"):
                return ln
        return "(no reply)"

    def otaurl(self, url: str) -> str:
        self.cmd(f"OTAURL {url}")
        t0 = time.time()
        while time.time() - t0 < 3:
            ln = self.readline()
            if ln.startswith("OK OTAURL") or ln.startswith("ERR"):
                return ln
        return "(no reply)"

    def otarun(self) -> str:
        self.cmd("OTARUN")
        # OTA can take a long time; keep reading until OTARUN result or timeout.
        self.s.timeout = 120
        lines = []
        t0 = time.time()
        while time.time() - t0 < 180:
            ln = self.readline()
            if not ln:
                continue
            lines.append(ln)
            if ln.startswith("OTARUN"):
                break
        return "\n".join(lines) if lines else "(no reply)"


# ---------------------------------------------------------------------------
# Offline WiGLE CSV helpers
# ---------------------------------------------------------------------------

WIGLE_FIELDS = [
    "MAC", "SSID", "AuthMode", "FirstSeen", "Channel", "RSSI",
    "CurrentLatitude", "CurrentLongitude", "AltitudeMeters",
    "AccuracyMeters", "Type",
]


def _open_wigle(path: Path):
    """Yield dict rows from a Pocket Pirate / WiGLE 1.4 CSV."""
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        # Skip WiGLE meta line if present
        first = f.readline()
        if not first.startswith("WigleWifi"):
            f.seek(0)
        reader = csv.DictReader(f)
        for row in reader:
            # Normalize keys (strip BOM residue / whitespace)
            yield { (k or "").lstrip("\ufeff").strip(): v for k, v in row.items() }


def summarize_wigle(path: Path) -> dict:
    rows = list(_open_wigle(path))
    ssids = Counter()
    macs = set()
    channels = Counter()
    auth = Counter()
    rssi_vals = []
    with_gps = 0
    for r in rows:
        ssid = (r.get("SSID") or "").strip() or "<hidden>"
        ssids[ssid] += 1
        mac = (r.get("MAC") or "").strip().upper()
        if mac:
            macs.add(mac)
        ch = (r.get("Channel") or "").strip()
        if ch:
            channels[ch] += 1
        a = (r.get("AuthMode") or "").strip() or "?"
        auth[a] += 1
        try:
            rssi_vals.append(int(r.get("RSSI") or 0))
        except ValueError:
            pass
        try:
            lat = float(r.get("CurrentLatitude") or 0)
            lon = float(r.get("CurrentLongitude") or 0)
            if abs(lat) > 0.0001 or abs(lon) > 0.0001:
                with_gps += 1
        except ValueError:
            pass
    return {
        "file": str(path),
        "rows": len(rows),
        "unique_macs": len(macs),
        "unique_ssids": len(ssids),
        "rows_with_gps": with_gps,
        "rssi_min": min(rssi_vals) if rssi_vals else None,
        "rssi_max": max(rssi_vals) if rssi_vals else None,
        "top_ssids": ssids.most_common(15),
        "channels": channels.most_common(20),
        "auth_modes": auth.most_common(12),
    }


def export_wigle(path: Path, fmt: str, dest: Path) -> int:
    rows = list(_open_wigle(path))
    if fmt == "slim":
        with dest.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["MAC", "SSID", "AuthMode", "Channel", "RSSI",
                        "Latitude", "Longitude", "FirstSeen"])
            for r in rows:
                w.writerow([
                    r.get("MAC", ""), r.get("SSID", ""), r.get("AuthMode", ""),
                    r.get("Channel", ""), r.get("RSSI", ""),
                    r.get("CurrentLatitude", ""), r.get("CurrentLongitude", ""),
                    r.get("FirstSeen", ""),
                ])
        return len(rows)

    if fmt == "unique-ssid":
        best = {}
        for r in rows:
            ssid = (r.get("SSID") or "").strip() or "<hidden>"
            try:
                rssi = int(r.get("RSSI") or -999)
            except ValueError:
                rssi = -999
            prev = best.get(ssid)
            if prev is None or rssi > prev[0]:
                best[ssid] = (rssi, r)
        with dest.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["SSID", "MAC", "AuthMode", "Channel", "BestRSSI",
                        "Latitude", "Longitude"])
            for ssid, (rssi, r) in sorted(best.items(), key=lambda kv: -kv[1][0]):
                w.writerow([
                    ssid, r.get("MAC", ""), r.get("AuthMode", ""),
                    r.get("Channel", ""), rssi,
                    r.get("CurrentLatitude", ""), r.get("CurrentLongitude", ""),
                ])
        return len(best)

    if fmt == "geojson":
        features = []
        for r in rows:
            try:
                lat = float(r.get("CurrentLatitude") or 0)
                lon = float(r.get("CurrentLongitude") or 0)
            except ValueError:
                continue
            if abs(lat) < 0.0001 and abs(lon) < 0.0001:
                continue
            features.append({
                "type": "Feature",
                "geometry": {"type": "Point", "coordinates": [lon, lat]},
                "properties": {
                    "mac": r.get("MAC", ""),
                    "ssid": r.get("SSID", ""),
                    "auth": r.get("AuthMode", ""),
                    "channel": r.get("Channel", ""),
                    "rssi": r.get("RSSI", ""),
                    "seen": r.get("FirstSeen", ""),
                },
            })
        fc = {"type": "FeatureCollection", "features": features}
        dest.write_text(json.dumps(fc, indent=2), encoding="utf-8")
        return len(features)

    sys.exit(f"unknown export format: {fmt}")


def print_summary(s: dict):
    print(f"File:          {s['file']}")
    print(f"Rows:          {s['rows']}")
    print(f"Unique MACs:   {s['unique_macs']}")
    print(f"Unique SSIDs:  {s['unique_ssids']}")
    print(f"Rows with GPS: {s['rows_with_gps']}")
    if s["rssi_min"] is not None:
        print(f"RSSI range:    {s['rssi_min']} .. {s['rssi_max']} dBm")
    print("\nTop SSIDs:")
    for name, n in s["top_ssids"]:
        print(f"  {n:5d}  {name}")
    print("\nChannels:")
    for ch, n in s["channels"]:
        print(f"  ch {ch:>2}: {n}")
    print("\nAuth modes:")
    for a, n in s["auth_modes"]:
        print(f"  {n:5d}  {a}")


def main():
    ap = argparse.ArgumentParser(description="Pocket Pirate USB companion")
    ap.add_argument("--port", "-p", help="Serial port (auto if exactly one USB)")
    ap.add_argument(
        "command",
        choices=[
            "info", "status", "logs", "logget", "watch", "help",
            "wificfg", "otaurl", "otarun", "sleep",
            "summarize", "export",
        ],
        help="Companion command",
    )
    ap.add_argument("path", nargs="?", help="Path for logget / summarize / export / otaurl")
    ap.add_argument("password", nargs="?", help="Password for wificfg")
    ap.add_argument("-o", "--output", help="Output file for logget / export")
    ap.add_argument(
        "--format", "-f",
        choices=["slim", "geojson", "unique-ssid"],
        default="slim",
        help="Export format (default: slim)",
    )
    ap.add_argument("--ssid", help="WiFi SSID for wificfg")
    args = ap.parse_args()

    # Offline CSV commands — no serial needed
    if args.command == "summarize":
        if not args.path:
            sys.exit("summarize requires a CSV path")
        print_summary(summarize_wigle(Path(args.path)))
        return
    if args.command == "export":
        if not args.path:
            sys.exit("export requires a CSV path")
        dest = Path(args.output) if args.output else Path(
            f"{Path(args.path).stem}.{ 'geojson' if args.format == 'geojson' else 'csv'}"
        )
        n = export_wigle(Path(args.path), args.format, dest)
        print(f"Wrote {n} records -> {dest}")
        return

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
        elif args.command == "wificfg":
            ssid = args.ssid or args.path
            password = args.password or ""
            if not ssid:
                sys.exit("wificfg requires --ssid NAME and password arg")
            print(c.wificfg(ssid, password))
        elif args.command == "otaurl":
            if not args.path:
                sys.exit("otaurl requires a URL")
            print(c.otaurl(args.path))
        elif args.command == "otarun":
            print(c.otarun())
        elif args.command == "sleep":
            c.cmd("SLEEP")
            print("SLEEP sent")
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
