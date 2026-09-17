# 🏴‍☠️ Pocket-Pirate-CYD

<img width="1848" height="4000" alt="20260917_084137" src="https://github.com/user-attachments/assets/992e6dfd-d129-4ae0-95d4-3a98ccf2ce3d" />

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

### Installation & Flashing
1. **Clone the repository:**
   ```bash
   git clone [https://github.com/Evil0ctopus/pocket-pirate-cyd.git](https://github.com/Evil0ctopus/pocket-pirate-cyd.git)
   cd pocket-pirate-cyd
   
   Open the project workspace:
Launch VS Code and open the cloned pocket-pirate-cyd folder. PlatformIO will automatically recognize the platformio.ini configuration.

Connect your hardware:
Plug your ESP32-S3 CYD device into your computer via USB. (Put the device into bootloader mode if required by holding the BOOT button while connecting).

Build and Upload:
Click the PlatformIO: Upload button (the right-pointing arrow icon in the bottom status bar) or run the command in your terminal:

Bash
pio run --target upload
Launch:
Once the upload completes, the device will automatically reboot, execute the boot sequence, and present your pirate captain dashboard on the touch display!

pocket-pirate-cyd/
├── .vscode/             # VS Code workspace settings
├── artpack_pixel/       # Pixel art assets and UI packs
├── artpack_sample/      # Sample art assets and references
├── device_shots/        # Photos and screenshots of the hardware
├── docs/                # Documentation and schematics
├── include/             # Header files
├── src/                 # Main source code logic
├── tools/               # Helper scripts and utilities
├── platformio.ini       # PlatformIO build configuration
└── LICENSE              # MIT License

🧑‍💻 Creator & Author
Joshua Lorson (Evil0ctopus)

📜 License
Distributed under the terms of the MIT License.

