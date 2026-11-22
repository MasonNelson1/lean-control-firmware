# LEAN backlog

Ideas, bugs and tests to track. Started 2026-11-21 from the live meeting and a
two-reviewer firmware audit (details in `notes/2026-11-21-meeting-findings.md`).
These are meant to become GitHub issues in `MasonNelson1/lean-control-firmware`.
Charles's work GitHub account can't post there, so Mason or a personal account
has to file them.

Status: `open` · `in progress (council/firmware-9)` · `done (commit)`

## Safety
- **[done 40bb3b7] DISARM button ignored with an empty seat.** It's honored now.
- **[done 40bb3b7] Sensor at an ADC rail drives the chair.** Raw ≤100 / ≥4000 in LEAN now disarms.
- **[open] Silent VESC not detected, and no Teensy watchdog.** `vescFresh()` is never called. Commands are clamped to measured RPM ±50, so frozen feedback means the chair can't stop. Before adding the fault, confirm both VESCs report (`DBG VESC age=`). Also set the VESC app timeout to ≤100 ms with a brake current.
- **[open, needs decision] Stops coast or ramp; spec B unmet.** Seat loss and disarm send zero current (freewheel). A remote timeout takes ~0.9 s / 1.1 m at knob 2. Meeting <5% in 0.5 s needs ~5 m/s², but MAX_ACCEL is 3.7. Relax the spec, or allow a harder emergency decel?
- **[open] ASSIST has no speed cap and feeds on itself.** It delivers full boost above the speed limit.
- **[open] REMOTE drive packets have no CRC** (config packets do).

## Calibration and seat
- **[open] Seat detection mixes units and has no hysteresis.** A rider can count as seated before calibrating and empty after. One 5 ms sample → DISARM and lost calibration.
- **[open] Dynamic calibration.** Gains are unbounded (up to 100), RAM-only (lost on reboot or profile switch), and a long cal press disarms. Dashboard gain edits apply late.
- **[open] Neutral is a 40 ms snapshot taken at the button press.** Average ~1 s after release instead, with the wheels stopped.

## Control and turning
- **[open] Centre-of-pressure lean math.** Subtract ~372 per sensor, then x=(R−L)/Σ, y=(F−B)/Σ. Replaces the per-sensor gains and W (W is ~80% amp offset). Cancels rider weight and creep.
- **[open] Turning overshoot.** Smoother lag (~32° extra), per-wheel smoothing erases steering while accelerating, and turn rate ~4× from knob 1→4. Fix: smooth speed and turn as two axes, turn in deg/s, a standstill turn cap, blend the -0.3 reverse flip.
- **[open] Add a gyro/IMU** and close the loop on yaw rate. That's the real fix for hitting a 90°. Pins 18/19 (I2C) are free.
- **[open] RPM commands truncated to whole RPM** (`vescSetWheelRPM` takes int32).

## Tests (data-driven, with tools/chair_tap.py + tools/chair_sql.py)
- **[open] Sensor drift / creep test (FSR 402).** Empty seat 30 min; seated still 10 min; sit/stand ×10; with and without the cushion. The first empty-seat run already showed the back sensor moving ~110 → 79 → ~88 (post-gain) over 18 min.
- **[open] Sensor position study.** Front and back have 6 holes, left and right 5. A guided session: AI calls out the configuration and moves; score by sensitivity, symmetry, rest noise and reseat repeatability. Charles suspects the front sensor is best nearer the pivot. Might fix weak reversing.

## VESC Tool (check on both controllers, record the values)
- **[open]** `s_pid_min_erpm`: the default 900 ERPM ≈ 15.5 wheel RPM. A full standstill turn at knob 2 asks for only ~26 RPM, which may explain "nothing, then everything".
- **[open]** App timeout plus brake current; CAN STATUS 1/4/5 at 20 ms (battery voltage, MOSFET temp). IDs: left 2, right 26.

## Tooling
- **[done]** Live Bluetooth telemetry tap (`tools/chair_tap.py`), SQL queries (`tools/chair_sql.py`), and once-a-second `DBG SENSORS` / `DBG VESC` lines (2681a57, not uploaded yet).
- **[open]** A `building-with-charles.md` setup doc linked from CLAUDE.md, once the live setup is final.
- **[open]** Let the dashboard and the tap share the one HC-06 connection (the tap owns the port and the dashboard reads through it).
