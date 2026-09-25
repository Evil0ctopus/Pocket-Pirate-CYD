# 🏴‍☠️ Pocket-Pirate-CYD

<img width="2716" height="1848" alt="20260917_084137" src="https://github.com/user-attachments/assets/530555d2-cbce-4406-8a25-06f6070307f6" />

A pirate-themed handheld utility and firmware suite built specifically for the ESP32-S3 2.8" TFT Touch Module (commonly known as the **Cheap Black Display** or **CYD**). Transform your pocket hardware into a fully functional pirate captain's dashboard complete with pixel art, interactive menus, and modular tools!

![Pocket-Pirate-CYD in Action](device_shots/screenshot_placeholder.jpg)

---

## ⚓ Features & Modules

Pocket-Pirate-CYD comes packed with custom assets and modular features designed for the ESP32-S3 architecture:

* **🏴‍☠️ Pirate Status Dashboard:**
  * Tracks your current rank (e.g., *Redbeard - Deckhand Lv 2*), experience progress bars, charts, cargo metrics, and pillaged "bottles".
  * Interactive touch tabs (`DECK` / `SHIP` / `STATIONS`) to navigate between different operational screens.
* **🎨 Artpack Pixel & Sample Engines:**
  * Modular graphics asset packs (`artpack_pixel` and `artpack_sample`) providing custom retro UI layouts, status icons, and background rendering.
* **🛠️ Utility & Tool Suite:**
  * Built-in helper scripts and tools located in the `tools/` directory to assist with asset conversion, flashing workflows, and system debugging.
* **📦 Modular Firmware Layout:**
  * Cleanly structured codebase utilizing PlatformIO for seamless expansion, custom hardware hooks, and easy deployment.

---

## 🚀 Getting Started & Startup Process

### Prerequisites
1. **Visual Studio Code** installed on your workstation.
2. The **PlatformIO IDE** extension installed inside VS Code.
3. A USB-C cable connected to your ESP32-S3 CYD board.
4. For the serial device test helper: Python 3, then `pip install -r tools/requirements.txt` (`pyserial`, `Pillow`).

### Installation & Flashing

1. **Clone the repository:**

   ```bash
   git clone https://github.com/Evil0ctopus/Pocket-Pirate-CYD.git
   cd Pocket-Pirate-CYD
   ```

2. **Open the project workspace:**
   Launch VS Code and open the cloned `Pocket-Pirate-CYD` folder. PlatformIO will automatically recognize the `platformio.ini` configuration.

3. **Connect your hardware:**
   Plug your ESP32-S3 CYD device into your computer via USB. (Put the device into bootloader mode if required by holding the BOOT button while connecting.)

4. **Build and Upload:**
   Click the PlatformIO: Upload button (the right-pointing arrow icon in the bottom status bar) or run:

   ```bash
   pio run --target upload
   ```

5. **Launch:**
   Once the upload completes, the device will automatically reboot, execute the boot sequence, and present your pirate captain dashboard on the touch display!

### Serial device test helper

`tools/device_test.py` drives the firmware's USB serial test hooks (`INFO` / `TAP` / `SHOT`) and writes PNGs under `device_shots/`.

Install deps once:

```bash
pip install -r tools/requirements.txt
```

There is **no hardcoded COM port**. Pass `--port`, or omit it to auto-detect when exactly one USB serial port is present. If multiple USB serial devices are connected, `--port` is required:

```bash
# Windows example
python tools/device_test.py --port COM3

# Linux example
python tools/device_test.py --port /dev/ttyACM0

# macOS example
python tools/device_test.py --port /dev/cu.usbmodem14101

# Auto-detect (succeeds only if exactly one port is available)
python tools/device_test.py
```

If no USB serial ports are found, or several are present, the script exits with a clear error (listing any non-USB ports it skipped) and the `--port` usage.

---

## 📁 Project layout

```
Pocket-Pirate-CYD/
├── .vscode/             # VS Code workspace settings
├── artpack_pixel/       # Pixel art assets and UI packs
├── artpack_sample/      # Sample art assets and references
├── device_shots/        # Photos and screenshots of the hardware
├── docs/                # Documentation and schematics
├── include/             # Header files
├── src/                 # Main source code logic
├── tools/               # Helper scripts and utilities
│   └── requirements.txt # pip deps for device_test.py
├── platformio.ini       # PlatformIO build configuration
└── LICENSE              # MIT License
```

---

## 🧑‍💻 Creator & Author

Evil0ctopus

### Support My Work

[![PayPal](https://img.shields.io/badge/PayPal-00457C?style=for-the-badge&logo=paypal&logoColor=white)](https://paypal.me/Evil0ctopus)

---

## 📜 License

Distributed under the terms of the MIT License.

