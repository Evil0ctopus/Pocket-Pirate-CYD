# OTA updates (Settings Cabin / companion)

Pocket Pirate can pull a new **application** image over Wi-Fi and write it with
ESP32 `HTTPUpdate`. Detection-only RF posture is unchanged (STA download only).

## Important: which binary?

| Asset | Flash / OTA |
|-------|-------------|
| `Pocket-Pirate-CYD-cheap-black-display.bin` | **Factory / USB** — merged image, write at **0x0** with esptool |
| `Pocket-Pirate-CYD-cheap-black-display-app.bin` | **OTA** — app partition only (what `HTTPUpdate` expects) |

Do **not** point OTA at the merged factory bin.

## Configure (USB companion)

```bash
pip install -r tools/requirements.txt

# Store Wi-Fi + firmware URL in NVS on the device
python tools/companion.py --port PORT wificfg --ssid 'MyHome' 'secretpass'
python tools/companion.py --port PORT otaurl \
  'https://github.com/Evil0ctopus/Pocket-Pirate-CYD/releases/download/v0.3.0/Pocket-Pirate-CYD-cheap-black-display-app.bin'

# Run now over serial…
python tools/companion.py --port PORT otarun

# …or tap **OTA Update** in Settings Cabin on-device
```

## On-device

1. Settings Cabin → confirm status line shows WiFi + URL set.
2. Tap **OTA Update**.
3. Device connects, downloads, flashes, reboots on success.
4. Failures surface as toast + `ota::status()` (also via companion `STATUS`).

## Safety notes

- Prefer **HTTPS** release URLs.
- Keep USB connected the first time you try OTA so you can recover with
  `pio run -t upload` / factory bin at 0x0 if something goes wrong.
- Idle sleep (optional) is separate from OTA; disable it while updating.
