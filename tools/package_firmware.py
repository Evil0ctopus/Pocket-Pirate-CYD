#!/usr/bin/env python3
"""Merge PlatformIO ESP32-S3 build artifacts into a factory image at 0x0.

esptool write_flash 0x0 of the *app-only* firmware.bin bricks/fails boot.
This script produces a flashable image:

  bootloader @ 0x0
  partition table @ 0x8000
  boot_app0 @ 0xe000 (if present)
  application @ 0x10000

Usage (from repo root, after `pio run -e cheap-black-display`):

  python tools/package_firmware.py
  python tools/package_firmware.py -o dist/Pocket-Pirate-CYD-cheap-black-display.bin
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "cheap-black-display"
DEFAULT_OUT = ROOT / "dist" / "Pocket-Pirate-CYD-cheap-black-display.bin"


def find_esptool() -> list[str]:
    # Prefer PlatformIO's esptool module.
    try:
        import esptool  # noqa: F401

        return [sys.executable, "-m", "esptool"]
    except ImportError:
        pass
    which = shutil.which("esptool") or shutil.which("esptool.py")
    if which:
        return [which]
    sys.exit("esptool not found (pip install esptool / PlatformIO env)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    ap.add_argument(
        "--env",
        default="cheap-black-display",
        help="PlatformIO env build dir name",
    )
    args = ap.parse_args()

    build = ROOT / ".pio" / "build" / args.env
    bootloader = build / "bootloader.bin"
    partitions = build / "partitions.bin"
    app = build / "firmware.bin"
    boot_app0 = build / "boot_app0.bin"

    for req in (bootloader, partitions, app):
        if not req.is_file():
            sys.exit(f"missing {req} — run: pio run -e {args.env}")

    args.output.parent.mkdir(parents=True, exist_ok=True)

    cmd = find_esptool() + [
        "--chip",
        "esp32s3",
        "merge-bin",
        "-o",
        str(args.output),
        "--flash-mode",
        "qio",
        "--flash-freq",
        "80m",
        "--flash-size",
        "16MB",
        "0x0",
        str(bootloader),
        "0x8000",
        str(partitions),
    ]
    if boot_app0.is_file():
        cmd += ["0xe000", str(boot_app0)]
    cmd += ["0x10000", str(app)]

    print("Running:", " ".join(cmd))
    subprocess.check_call(cmd)
    size = args.output.stat().st_size
    print(f"Wrote factory image ({size} bytes) -> {args.output}")
    print("Flash with: esptool.py --chip esp32s3 --port PORT write_flash 0x0", args.output.name)


if __name__ == "__main__":
    main()
