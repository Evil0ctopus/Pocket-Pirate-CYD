# Optional UART GPS (Chart Room / Instruments)

Pocket Pirate accepts a standard NMEA GPS module on the Hosyond CYD **UART header**.
USB CDC owns the console (`Serial`), so UART0 pins are free for `Serial1`.

## Pins (`include/board_pins.h`)

| Function | ESP32-S3 GPIO | Wire to GPS module |
|----------|---------------|--------------------|
| GPS RX   | **43**        | GPS **TX**         |
| GPS TX   | **44**        | GPS **RX** (optional) |
| GND      | GND           | GND                |
| 3V3      | 3V3           | VCC (3.3 V modules only) |

Baud: **9600** (common default for NEO-6M / NEO-8M style modules).

## Behavior

- Module **absent**: Chart Room / Instruments show `NO GPS`; WiGLE CSV lat/lon/alt/accuracy stay blank; timestamps fall back to uptime placeholder `2000-01-01 …`.
- Module present, **no fix yet**: status `NO FIX`.
- **Fix**: status `FIX N` (satellite count). Chart Room writes real lat/lon/alt + UTC `FirstSeen` into `/wigle.csv`.

Passive metadata only — GPS is receive-only NMEA; no RF transmit features.
