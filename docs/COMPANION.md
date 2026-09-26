# USB companion protocol

Line-oriented commands over USB CDC at **115200** baud (newline-terminated).
Same port as `tools/device_test.py`.

## Commands

| Command | Reply |
|---------|-------|
| `INFO` | One-line key=value summary (backward compatible + battery/gps/wifi/ble) |
| `STATUS` / `STATUSJ` / `SUMMARY` | Single JSON object |
| `LOGS` | `LOGS_BEGIN` … `LOG name size` … `LOGS_END` |
| `LOGGET /path` | `LOGGET_BEGIN` … raw bytes … `LOGGET_END` |
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
```

`tools/device_test.py` continues to work (`INFO` / `TAP` / `SHOT`).
