# LEAN Wheelchair — Control Firmware

Senior capstone (ME487, Boise State). A powered wheelchair for wheelchair
basketball, driven hands-free by torso lean measured with seat force sensors.
This repo is the Teensy 4.1 control firmware. `HANDOFF.md` holds the full
design history; this file holds the durable facts.

## Build / upload / monitor (PlatformIO)
`pio` is not on the shell PATH here; use `~/.platformio/penv/bin/pio`.

| Env | Source | Purpose |
|---|---|---|
| `teensy41` (default) | `src/main.cpp` | lean control pipeline |
| `can_test` | `src/can_motor_test.cpp` | two-sensor → one-VESC bench test |

- Build:   `pio run` (main) / `pio run -e can_test`
- Upload:  `pio run -t upload` / `pio run -e can_test -t upload`
- Monitor: `pio device monitor`  (115200 baud; close it before uploading)
- Always build before suggesting an upload. Report compile errors verbatim.
- Upload uses the Teensy Loader GUI (`teensy-gui`). Its `[SUCCESS]` only means
  the loader opened, not that the board was flashed — confirm via the serial
  output. `teensy_loader_cli` is x86-only and won't run without Rosetta.

## Requirements the control system must meet
- **B** — on any fault/e-stop: safe state within 0.10 s, wheel speed < 5% within 0.50 s
- **I** — all propulsion and steering from torso lean alone
- **J** — calibrate for a rider in ≤ 5 min
- **K** — respond to a lean within 0.10 s of threshold crossing
- **F/G/H** — acceleration ≥ 2.85 m/s², top speed ≥ 11 mph, turn rate ≥ 200 °/s

## Hardware
- MCU: Teensy 4.1, 3.3 V logic. **Analog pins are NOT 5 V tolerant.**
- Power: 36 V LiFePO4. Hardware e-stop = circuit breaker on battery +,
  independent of firmware.
- Motors: 2x 36 V 450 W BLDC with Hall sensors. **No motor thermistor** — the
  only temperature available is the VESC MOSFET temperature.
- Sensors: 4 force sensors in a plus formation (front/back/left/right) under a
  rigid load plate. Each goes through one channel of an MCP6004 (single 3.3 V
  supply, so output can't exceed 3.3 V) transimpedance amp: sensor from − input
  to GND, RF = 47k ‖ CF = 1 nF feedback, + input at VB ≈ 0.3 V (shared
  100k/10k divider off 3.3 V + 1 µF). Vout = VB * (1 + RF/RS). 1k + 10 nF RC
  into the Teensy pin. RF is tuned later with the real plate and rider.
- Resting reading ≈ 370 counts nominal; measured 385–398 on the bench
  (2026-10-02). A firm thumb press ≈ 650–700.
- Speed + acceleration: two 5-position rotary selector switches (C&K A105),
  each wired as a 10k resistor ladder (pos 1 = GND, pos 5 = 3.3 V), rotor to
  the Teensy with 100 nF + 1 MΩ to GND. Broken wire reads position 1 (slowest)
  on purpose.
- Zero/calibration button: pin 4 to GND, INPUT_PULLUP.
- Motor controllers: 2x VESC 6/75 on CAN, controller ID 1 = left, 2 = right.

## Pin map
| Signal | Pin |
|---|---|
| Front sensor | A14 (pin 38) |
| Back sensor | A17 (pin 41) |
| Left sensor | A16 (pin 40) |
| Right sensor | A15 (pin 39) |
| Speed switch | A6 (pin 20) |
| Accel switch | A7 (pin 21) |
| Zero button | 4 |
| CAN1 TX / RX | 22 / 23 |
Keep pins 18/19 (I2C) and 22/23 (CAN) free.

## CAN hardware (MCP2562 transceiver)
| MCP2562 pin | Connects to |
|---|---|
| 1 TXD | Teensy pin 22 (CAN1 TX) |
| 4 RXD | Teensy pin 23 (CAN1 RX) |
| 2 VSS | GND (common with both VESCs) |
| 3 VDD | 5 V from the **left** VESC's CAN 5 V output |
| 5 VIO | **3.3 V** (sets logic level — required) |
| 8 STBY | **GND** (floating = standby, transmits nothing) |
| 7 / 6 | CANH / CANL |
- 120 Ω at each end of the bus → ≈ 60 Ω between CANH and CANL, power off.
  With one VESC on the bench, the ends are the MCP2562 and that VESC.
- Most CAN failures: STBY not grounded, VIO not on 3.3 V, missing termination,
  TX/RX swapped, wrong bitrate, wrong controller ID. Check these physically
  before changing code.

## VESC CAN protocol
- Extended (29-bit) IDs, 500 kbps. `id = controller_id | (packet_type << 8)`.
- Multi-byte values big-endian, signed.
- Library: `FlexCAN_T4` (ships with the Teensy core).

| Command (Teensy → VESC) | Type | Payload (int32) |
|---|---|---|
| SET_DUTY | 0 | duty × 100000 |
| SET_CURRENT | 1 | amps × 1000 |
| SET_CURRENT_BRAKE | 2 | amps × 1000 |
| SET_RPM | 3 | ERPM |
e.g. left SET_DUTY = `0x001`, left SET_CURRENT = `0x101`, right = `0x102`.

| Status (VESC → Teensy) | Type | Bytes |
|---|---|---|
| STATUS | 9 | 0–3 ERPM (int32) · 4–5 current ×10 · 6–7 duty ×1000 |
| STATUS_4 | 16 | 0–1 MOSFET temp ×10 · 2–3 motor temp (invalid, no thermistor) · 4–5 input current ×10 |
| STATUS_5 | 27 | 0–3 tachometer · 4–5 input voltage ×10 |
Temperature is in STATUS_4, not STATUS_5.

- The VESC stops the motor when commands stop arriving (App Settings →
  General → Timeout, ~250 ms). Firmware must send a command every cycle,
  **including zeros**, at ≥ 50 Hz.
- VESC Tool setup before any CAN test: motor wizard (FOC, Hall), controller
  ID, CAN baud 500k, CAN status messages STATUS 1–5 at 20 ms, low current
  limits for bench (e.g. 10 A), and spin the motor from VESC Tool first.
- Bench tests use duty control (SET_DUTY): an unloaded motor runs away to
  full speed on any current command. Use current control only on the chair.

## Control pipeline (src/main.cpp)
read forces (floor-subtracted) -> occupancy gate -> center of pressure per axis
(front/back = speed, right/left = turn) minus calibrated neutral -> deadzone ->
differential mix (left = speed + turn, right = speed - turn, normalized if >1)
-> scale by speed-switch max -> slew limit set by accel switch -> output.

`SIM_MODE 1` replaces sensors with typed input: `F,B,L,R` raw ADC values, or
`cal` to capture neutral. `USE_POTS 0` uses default speed/accel instead of the
switches. `RAW_MONITOR 1` skips the pipeline and only prints raw counts from
`MONITOR_PINS` (sensor bring-up).

## Safety rules (do not violate)
- Empty seat, disconnected sensor, or lost input must produce ZERO output.
- Never remove or weaken the slew limit, deadzone, occupancy gate, kill switch,
  zero-on-idle, or reliance on the VESC command timeout without being
  explicitly asked, and flag the safety impact when changing them.
- Zeroing is only allowed while the chair is stopped.
- The hardware e-stop (circuit breaker on battery +) is independent of firmware.
- **Ask before uploading any code that commands a motor.**
- Anything that commands motors gets tested wheels-off-the-ground first.
  Bench: motor clamped, nothing on the shaft, hands clear, low VESC current
  limits, inline fuse, someone on the power disconnect.
- Never tie the two VESCs' 5 V outputs together; 5 V comes from the left VESC
  only. Grounds must be common.

## Status / next work
1. **Done (2026-10-02):** HANDOFF.md §8 two-sensor CAN motor test
   (`can_test` env). Stages 1–3 passed on hardware with VESC ID 1, including
   the USB-unplug stop. `STAGE` in can_motor_test.cpp selects the stage.
2. Wire and test the rotary-switch ladders and zero button.
3. Implement VESC CAN in the main pipeline (current commands, status frames).
4. When all 4 sensors return: SIM_MODE 0, set FLOOR and OCCUPANCY_MIN from
   measurements, tune RF so a hard corner lean stays below ~3900 counts.
