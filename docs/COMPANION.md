# USB companion protocol

Line-oriented commands over USB CDC at **115200** baud (newline-terminated).
Same port as `tools/device_test.py`.

## Commands

| Command | Reply |
|---------|-------|
| `INFO` | One-line key=value summary |
| `STATUS` / `STATUSJ` / `SUMMARY` | Single JSON object |
| `LOGS` | `LOGS_BEGIN` … `LOG name size` … `LOGS_END` |
| `LOGGET /path` | `LOGGET_BEGIN` … raw bytes … `LOGGET_END` |
| `WIFICFG ssid\|password` | Store STA credentials for OTA (NVS) |
| `OTAURL <url>` | Store firmware URL (use **app-only** `.bin`) |
| `OTARUN` | Connect Wi-Fi + HTTPUpdate (blocking) |
| `SLEEP` | Deep sleep; wake on touch INT |
| `HELP` | Command list |
| `TAP x y` | Inject touch (test) |
| `SHOT` | RGB565 framebuffer dump |
| `BEEP` / `SCAN` | Audio chime / I2C scan |

## Host script

```bash
pip install -r tools/requirements.txt
python tools/companion.py --port COM3 status
python tools/companion.py watch
python tools/companion.py logs
python tools/companion.py logget /wigle.csv -o wigle.csv

# OTA prep
python tools/companion.py --port COM3 wificfg --ssid 'HomeWiFi' 'secret'
python tools/companion.py --port COM3 otaurl 'https://…/…-app.bin'
python tools/companion.py --port COM3 otarun
```

## Offline WiGLE CSV helpers

No device required — works on any Chart Room export:

```bash
python tools/companion.py summarize wigle.csv
python tools/companion.py export wigle.csv --format slim -o slim.csv
python tools/companion.py export wigle.csv --format unique-ssid -o ssids.csv
python tools/companion.py export wigle.csv --format geojson -o points.geojson
```

`tools/device_test.py` continues to work (`INFO` / `TAP` / `SHOT`).
See also [`docs/OTA.md`](OTA.md).
