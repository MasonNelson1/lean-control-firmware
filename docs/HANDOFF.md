# LEAN — Project Handoff for the Firmware Agent

Read this fully before writing code. It summarizes design decisions made in
earlier planning sessions. Where this file and `CLAUDE.md` disagree, this file
is newer. Merge the durable facts (hardware, protocol, safety rules) into
`CLAUDE.md` so future sessions have them.

---

## 1. What LEAN is

Senior capstone (ME487, Boise State). A powered wheelchair for wheelchair
basketball. The rider drives hands-free by leaning their torso; seat force
sensors detect the lean, a Teensy 4.1 turns it into motor commands, and two
VESC motor controllers drive the two wheels. Hands stay free for the ball.

Key specs the control system must meet:
- **B** — on any fault/e-stop: safe state within 0.10 s, wheel speed < 5% within 0.50 s
- **I** — all propulsion and steering from torso lean alone
- **J** — calibrate for a rider in ≤ 5 min
- **K** — respond to a lean within 0.10 s of threshold crossing
- **F/G/H** — acceleration ≥ 2.85 m/s², top speed ≥ 11 mph, turn rate ≥ 200 °/s

## 2. System architecture

```
4 force sensors ─► MCP6004 op-amp front-end ─► Teensy 4.1 ADC
                                                   │
       speed switch, accel switch, zero button ───►│
                                                   ▼
                              lean pipeline (src/main.cpp)
                                                   │
                         MCP2562 CAN transceiver ◄─┘
                                   │  CAN bus, 500 kbps
                     ┌─────────────┴─────────────┐
                VESC 6/75 ID 1 (left)       VESC 6/75 ID 2 (right)
                     │                           │
               PEACO BLDC motor            PEACO BLDC motor
```

- Battery: 36 V LiFePO4. Hardware e-stop is a circuit breaker on battery +,
  independent of all firmware.
- Motors: 36 V 450 W BLDC with Hall sensors. **No motor thermistor** — the only
  temperature available is the VESC's own MOSFET temperature.

## 3. Sensing — history and current design

Design evolved: 16 FSRs + ADS1115s → Velostat matrix (explored) → **current:
4 force sensors in a plus formation** under a rigid load plate.

```
        [FRONT]          front pressure = forward
  [LEFT]       [RIGHT]   left/right     = turn
        [BACK]           back pressure  = backward
                         combinations   = turn while moving
```

Front-end per sensor (one channel of an MCP6004 quad op-amp, single 3.3 V supply):
- Transimpedance stage: sensor from op-amp − input to GND; RF = 47 kΩ ‖ CF = 1 nF
  from output to − input; + input tied to VB ≈ 0.3 V.
- VB comes from a 100 kΩ / 10 kΩ divider off 3.3 V with a 1 µF cap (shared by
  all four channels). It replaces the negative reference in the sensor
  datasheet's circuit so everything runs on one supply.
- Vout = VB × (1 + RF/RS). Output can't exceed 3.3 V, so it's safe for the Teensy.
- 1 kΩ + 10 nF RC filter into each Teensy analog pin.
- RF sets sensitivity. It gets tuned later with the real load plate and rider
  (lower RF if a hard lean clips near 4095, raise it if it barely moves).

**Bench status right now:** only 2 of 4 sensors are on hand (a teammate has the
others for mounting work). The two are on a breadboard:
- Sensor 1 → **A16 (pin 40)**
- Sensor 2 → **A17 (pin 41)**
- Resting reading ≈ 370 counts (the 0.3 V floor). A firm thumb press reaches
  ≈ 650–700. Real body weight on the plate will go much higher.

The final pin map (A0–A3 for front/back/left/right) is in `CLAUDE.md`. For
bench tests, use A16/A17.

## 4. Firmware status (`src/main.cpp`)

Written and syntax-checked; runs in `SIM_MODE 1` with typed input.
Pipeline: floor-subtracted forces → occupancy gate → center of pressure per
axis minus calibrated neutral → deadzone → differential mix → × speed-switch
max → slew limit set by accel switch → `outLeft` / `outRight` (−1..+1).
The output is currently only printed (TODO in `loop()`).

Inputs:
- Speed and acceleration: two 5-position C&K A105 rotary selector switches,
  each a 10 kΩ resistor ladder (position 1 = GND … position 5 = 3.3 V), rotor
  to A6 / A7 with 100 nF + 1 MΩ to GND. Firmware snaps to the nearest position
  and requires 3 stable reads. Broken wire = position 1 = gentlest (fail-safe).
- Zero/calibration button: pin 4 to GND, INPUT_PULLUP, 50 ms debounce, only
  accepted while the chair is stopped.

## 5. CAN hardware

- MCP2562: pin 1 TXD → Teensy **pin 22 (CAN1 TX)**; pin 4 RXD → Teensy
  **pin 23 (CAN1 RX)**; pin 2 VSS → GND; pin 3 VDD → 5 V from the **left
  VESC's** CAN 5 V output; pin 5 VIO → **3.3 V** (sets logic level — required);
  pin 8 STBY → **GND** (if it floats, the chip stays in standby and transmits
  nothing). CANH = pin 7, CANL = pin 6.
- Termination: 120 Ω at each end of the bus → measure **≈ 60 Ω between CANH and
  CANL with power off**. With only one VESC on the bench, the bus ends are the
  MCP2562 and that VESC, so both ends still need a 120 Ω.
- Never connect the two VESCs' 5 V outputs together (can permanently damage
  them). Grounds must be common.

## 6. VESC CAN protocol reference

- **Extended (29-bit) IDs, 500 kbps.** `id = controller_id | (packet_type << 8)`.
- Multi-byte values are **big-endian, signed**.

Commands (Teensy → VESC):

| Packet | Type | Payload (int32) | Example |
|---|---|---|---|
| SET_DUTY | 0 | duty × 100000 | 0.10 duty → 10000 |
| SET_CURRENT | 1 | amps × 1000 | 5 A → 5000 |
| SET_CURRENT_BRAKE | 2 | amps × 1000 | |
| SET_RPM | 3 | ERPM | |

Left VESC, SET_DUTY → id `0x001`; left SET_CURRENT → `0x101`;
right SET_CURRENT → `0x102`.

Status (VESC → Teensy, broadcast when enabled in VESC Tool):

| Packet | Type | Bytes |
|---|---|---|
| STATUS | 9 | 0–3 ERPM (int32) · 4–5 current ×10 · 6–7 duty ×1000 |
| STATUS_4 | 16 | 0–1 MOSFET temp ×10 · 2–3 motor temp ×10 (invalid here, no thermistor) · 4–5 input current ×10 |
| STATUS_5 | 27 | 0–3 tachometer · 4–5 input voltage ×10 |

**Correction to older notes:** temperature is in STATUS_4, not STATUS_5.

- **The VESC stops the motor if commands stop arriving** (App Settings →
  General → Timeout). Firmware must send a command every cycle, including
  zeros, at ≥ 50 Hz. Confirm the timeout value in VESC Tool (~250 ms is good).
- Library: `FlexCAN_T4` (ships with the Teensy core). If PlatformIO can't find
  it, add `lib_deps = https://github.com/tonton81/FlexCAN_T4` to platformio.ini.

## 7. VESC setup in VESC Tool (do before any CAN test)

1. Connect each VESC by USB, update firmware if prompted.
2. Run the motor setup wizard (FOC, Hall sensors) with the motor free to spin.
3. App Settings → General: Controller ID (1 = left, 2 = right); CAN baud 500k.
4. App Settings → General → CAN Status Message Mode: enable STATUS 1–5, 20 ms.
5. Motor current limits low for bench testing (e.g. 10 A).
6. Spin the motor from VESC Tool itself first. If that doesn't work, CAN won't either.

---

## 8. CURRENT TASK — two sensors drive one motor over CAN

**Goal:** prove the full chain sensor → Teensy → CAN → VESC → motor.
Sensor 1 (A16) pressed → motor spins forward. Sensor 2 (A17) pressed → motor
spins in reverse. Harder press → faster. Neither, or both → stop.

### Use duty-cycle control, not current control, for this test
On a bench with nothing attached, the motor has almost no load, so *any*
current command makes it accelerate to full no-load speed. Duty cycle
(`SET_DUTY`) maps directly to speed and is predictable. Use current control
only once the motor is driving the chair.

### Required behavior
- Target VESC ID 1 only (make the ID a constant).
- `s1 = analogRead(A16) − FLOOR`, `s2 = analogRead(A17) − FLOOR`, clamp ≥ 0.
  `FLOOR = 370`, `THRESH = 60` counts, `FULL = 300` counts above floor.
- Only s1 above THRESH → duty = +MAX_DUTY × min(1, (s1 − THRESH)/(FULL − THRESH)).
- Only s2 above THRESH → same magnitude, negative.
- Both or neither → 0.
- `MAX_DUTY = 0.10` (keep ≤ 0.15 for this test).
- Slew-limit the duty (full range in no less than ~0.5 s).
- Send `SET_DUTY` at **50 Hz continuously**, including zeros.
- Serial kill switch: typing `s` forces duty to 0 and latches it until `g` is typed.
- Receive STATUS / STATUS_4 / STATUS_5 and print at 10 Hz:
  `s1, s2, cmdDuty, ERPM, MOSFET temp, input voltage, frames received`.
- If no VESC status frame has arrived for 500 ms, print a warning
  (this is the main CAN-wiring diagnostic).

### Do it in stages; don't skip ahead
1. **Listen only** — no commands sent. Confirm STATUS frames arrive and ERPM /
   voltage look sane. This proves the CAN wiring, termination, and STBY/VIO.
2. **Fixed command from serial** — e.g. `d5` sends duty 0.05; prove the motor
   spins both directions with a typed value.
3. **Sensors in control** — the behavior above.

Put this test in its own environment or file (e.g. `src/can_motor_test.cpp`
built by a separate PlatformIO env) so it doesn't disturb the main pipeline.

### Bench safety (must hold for every stage)
- Motor clamped down, nothing attached to the shaft, hands clear.
- VESC current limits set low in VESC Tool; inline fuse on the supply.
- Someone keeps a hand on the power disconnect.
- Lifting both thumbs must stop the motor. Unplugging the USB/Teensy must stop
  the motor within the VESC timeout. Verify both before trusting it.

### Acceptance
- Stage 1: status frames arrive continuously; voltage matches the supply.
- Stage 2: typed duty spins the motor both directions; `s` stops it.
- Stage 3: sensor 1 → forward, sensor 2 → reverse, speed scales with press,
  both/neither → stop, Teensy unplugged → motor stops within the timeout.

## 9. Rules for the agent

- Build (`pio run`) after every change; report compile errors verbatim.
- Never remove or weaken a safety behavior (timeout reliance, zero-on-idle,
  slew limit, kill switch, occupancy gate) without being asked, and flag it if
  a change affects safety.
- Explain what to physically check when a test fails instead of guessing:
  most CAN failures are STBY not grounded, VIO not on 3.3 V, missing
  termination, TX/RX swapped, wrong bitrate, or wrong controller ID.
- Ask before uploading code that commands the motor.
