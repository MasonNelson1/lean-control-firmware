# LEAN PCBs (Mason's KiCad project, Rev M, 2026-10-19)

Trimmed from the Drive zip on 2026-11-21: backups, gerbers, 3D exports and
fabrication zips removed. The full original is still on the team Drive.

- `kicad/` schematic (5 sheets), board, project, symbol + footprint libraries. Plain text: grep nets.
- `docs/` Mason's design notes (wiring, Bluetooth, power switch, rotary switches, panel).
- `csv/` connector pinouts, BOM, Teensy pin map (`old_number` = footprint pad, NOT Teensy pin).
- `previews/` schematic and assembly PNGs.
- `datasheets/` assembly guide, control-board V2/V3 drawings, Pololu regulator, rotary switch, VESC (TRAMPA MkVI) manual.
- `vesc-reference/` VESC hw config source and appconf parameter definitions.

## Teensy pin → net (read from the board copper, matches firmware `src/common.h`)
| Pin | Net | Pin | Net |
|---|---|---|---|
| 0 / 1 | Bluetooth TX→RX / RX←TX (Serial1, J22) | 24 / 25 / 26 | BTN LEAN / DISARM / REMOTE |
| 2 / 32 | BT EN / STATE (R65/R66 DNP, disconnected) | 27–31 | LED CAL / LEAN / DISARM / REMOTE / CONNECTED |
| 4 | Calibrate button (J5.2) | A0 / A1 | SPEED / ACCEL knob |
| 22 / 23 | CAN1 TX / RX → MCP2562 | A14–A17 | FSR4 J9 / FSR3 J8 / FSR2 J7 / FSR1 J6 |

Firmware sensor roles: J6/A17 back, J7/A16 right, J8/A15 front, J9/A14 left.
VESC: J2 primary = 5 V, GND, CANH, CANL. J1 secondary = CAN only.
