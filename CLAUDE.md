# LEAN Wheelchair — Control Firmware

Senior capstone (ME487, Boise State). A powered wheelchair for wheelchair
basketball, driven hands-free by torso lean measured with seat force sensors.
This repo is the Teensy 4.1 control firmware.

## Build / upload / monitor (PlatformIO)
- Build:   `pio run`
- Upload:  `pio run -t upload`   (Teensy on USB; press the board button if upload stalls)
- Monitor: `pio device monitor`  (115200 baud)
- Always build before suggesting an upload. Report compile errors verbatim.

## Hardware
- MCU: Teensy 4.1, 3.3 V logic. **Analog pins are NOT 5 V tolerant.**
- Sensors: 4 force sensors in a plus formation (front/back/left/right) under a
  rigid load plate. Each goes through one channel of an MCP6004 single-supply
  transimpedance amp: Vout = VB * (1 + RF/RS), VB ≈ 0.3 V, RF = 47k (tunable),
  CF = 1 nF, 1k + 10 nF RC into the Teensy pin. Resting reading ≈ 370 counts.
- Speed + acceleration: two 5-position rotary selector switches (C&K A105),
  each wired as a 10k resistor ladder (pos 1 = GND, pos 5 = 3.3 V), rotor to
  the Teensy with 100 nF + 1 MΩ to GND. Broken wire reads position 1 (slowest)
  on purpose.
- Zero/calibration button: pin 4 to GND, INPUT_PULLUP.
- Motor controllers: 2x VESC 6/75 on CAN (controller ID 1 = left, 2 = right),
  MCP2562 transceiver. CAN output is NOT implemented yet (see TODO in loop()).

## Pin map
| Signal | Pin |
|---|---|
| Front sensor | A0 |
| Back sensor | A1 |
| Left sensor | A2 |
| Right sensor | A3 |
| Speed switch | A6 (pin 20) |
| Accel switch | A7 (pin 21) |
| Zero button | 4 |
| CAN1 TX / RX (future) | 22 / 23 |
Keep pins 18/19 (I2C) and 22/23 (CAN) free.

## Control pipeline (src/main.cpp)
read forces (floor-subtracted) -> occupancy gate -> center of pressure per axis
(front/back = speed, right/left = turn) minus calibrated neutral -> deadzone ->
differential mix (left = speed + turn, right = speed - turn, normalized if >1)
-> scale by speed-switch max -> slew limit set by accel switch -> output.

`SIM_MODE 1` replaces sensors with typed input: `F,B,L,R` raw ADC values, or
`cal` to capture neutral. `USE_POTS 0` uses default speed/accel instead of the
switches.

## Safety rules (do not violate)
- Empty seat, disconnected sensor, or lost input must produce ZERO output.
- Never remove or weaken the slew limit, deadzone, or occupancy gate without
  being explicitly asked, and flag the safety impact when changing them.
- Zeroing is only allowed while the chair is stopped.
- The hardware e-stop (circuit breaker on battery +) is independent of firmware.
- Anything that commands motors gets tested wheels-off-the-ground first.
- Never tie the two VESCs' 5 V outputs together; 5 V comes from the left VESC only.

## Status / next work
1. Verify the pipeline in SIM_MODE on hardware.
2. Wire and test the rotary-switch ladders and zero button.
3. Implement VESC CAN current commands (IDs 0x101 / 0x102, SET_CURRENT) and
   read VESC status frames (ERPM, current, MOSFET temp, voltage).
4. When all 4 sensors return: SIM_MODE 0, set FLOOR and OCCUPANCY_MIN from
   measurements, tune RF so a hard corner lean stays below ~3900 counts.
