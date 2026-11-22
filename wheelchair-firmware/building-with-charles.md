# Building with Charles: live chair setup

How Claude connects to the chair, reads its data, and runs experiments with the team. Charles's laptop: WSL (Ubuntu) on Windows.

## Working rules
- **Live sessions:** Charles records with the `live` skill. Answer from `~/live/current/transcript.md` (it runs ≈6 s behind the data), say who you're responding to, and write findings to `notes/<date>-meeting-findings.md` as you go.
- **Commits:** commit freely on `charles/dev-setup`. Push only when asked.
- **Uploads:**
  - Telemetry-only firmware: just upload.
  - Anything that changes how the chair behaves: flag it first. A change Charles asked for needs no re-confirmation.
  - Upload only with the chair powered off (e-stopped) and USB plugged in.
- **Weird sensor data:** investigate hardware first (plate, preload, positions, wiring).
- **Every test:** catalogue it in `../experiments/README.md` (setup, protocol with wall times, data file, results, interpretation).
- **Showing data:** interpretation first, a compact raw table (Mason reads raw counts), and a chart that compares on the same axes (Charles).
- **High trust:** for coding agents on this branch.

## Bluetooth telemetry (the chair's live data)
- **Module:** HC-06 on Teensy `Serial1`; shows in Windows as **"LEANReciever"**, PIN **8976**.
  - Windows lists it twice. Pair the **classic** entry, the one that asks for the PIN. The BLE entry gets no COM port.
  - "Connected → not connected" flicker is normal until a program opens the port. Outgoing port is **COM4**.
  - It takes **one connection at a time**: either the dashboard or the tap.
- **Tap:** run with Windows Python from WSL.
  ```bash
  cd wheelchair-firmware && python.exe tools/chair_tap.py --port COM4   # run in background
  ```
  - Writes `logs/live/<stamp>.csv` and `logs/live/current.csv` (laptop time + every line).
  - Reconnects by itself after e-stops and logs `TAP link up/down`.
  - Background jobs stop at 2 h; restart it.
- **What the firmware sends:**
  - The telemetry CSV row at 20 Hz (post-gain values; the dashboard parses it).
  - `DBG R <ms> F B L R`: raw ADC counts, 20 Hz.
  - `DBG COP <ms> x y dx dy total`: centre of pressure in inches and its offset from the calibrated (upright) neutral, 20 Hz, corner layout only (`chair_sql.py` table `cop`).
  - `DBG M <ms> curL curR dutyL dutyR iinL iinR pidL pidR accL accR rateL rateR intL intR lim`, 20 Hz: VESC motor/input current and duty, plus the RPM smoother's internals (`pid` is its RPM before the clamp to measured ± `MAX_RPM_COMMAND_DIFF`; `lim` is the slewed speed cap). Table `motor`.
  - `DBG VFAULT id= code= name= ...` the moment a VESC's fault code changes, including back to `NONE`. It carries the currents, erpm, duty and voltage from that poll. Table `vfault`.
  - `DBG EVT <ms> <kind> ...` on every button press/release, mode or selection change (`why=seat-empty|selected`), rail or CoP-jump disarm, calibration press/end, profile and settings change. Table `evt`.
  - Once a second (each on its own 20 Hz tick, so no burst overruns the HC-06):
    - `DBG SENSORS`: raw, floors, W, seat load, seated/calibrated/railed flags, `layout=PLUS|CORNER`.
    - `DBG CORNER` (corner layout): neutral, relaxed posture, the no-output box and full-lean box (left/right/back/front, inches), `reachCal`, the empty-plate baseline in use and `auto=1` when the empty-seat zero set it (`chair_sql.py` table `corner`).
    - `DBG VESC`: per VESC (`side=L|R`), every CAN status frame (1–5): age, frames, ERPM, current, duty, MOSFET and motor temperature, voltage, input current, current peaks since the last line (`ipk`, `iinpk`), Ah/Wh used/charged, tachometer, and per-type age `ages=s1/../s5` (`-1` = never sent; STATUS_5 needs `can_status_msgs_r2`).
    - `DBG VVAL`: per VESC, `COMM_GET_VALUES` (polled 10 Hz alternating, so each VESC every 200 ms; a VESC holds a fault for only 500 ms): fault code, averaged currents, temperatures, `n=asked/replies/lost`, `lat` (slowest reply ms).
    - `DBG SYS`: loop time, control-tick spacing, CPU temperature, CAN rx/tx-fail/error counters, Bluetooth bytes and lines sent and dropped (`drop=`), least TX space.
    - `DBG IO`: knobs (position/raw/state), buttons held, pending/active profile, selected/current mode, seated.
    - `DYNAMIC_GAINS`.
  - The dashboard hides `DBG` lines from its console. Its **Motor controllers** cards show each VESC's health (temps, voltage, currents, last fault, frame ages); the **VESC settings** page decodes the `DBG VCONF` dumps into every setting, side by side. Read-only: it never changes VESC settings. On the corner layout it moves the channel tiles to their corners and draws the plate: live CoP with a 2 s trail, neutral (+), relaxed posture (○), the no-output box and the full-lean box.

## Reading the data
- **SQL:** `python3 tools/chair_sql.py "SELECT ..."` (also `--schema`, `--trend N`, `--file <csv>`).
  - Tables: `t` (telemetry rows), `raw` (20 Hz raw), `sens`, `corner`, `cop`, `vesc`, `vval`, `vfault`, `motor`, `sys`, `io`, `evt`, `msg` (text lines; `kind` `TAP`/`CORRUPT`/...). `--schema` lists the columns.
  - VESC settings: `python3 tools/vesc_conf.py [log] [--all] [field ...]`.
- **Quick look:** `python3 tools/chair_summary.py [secs]`.
- **Experiment tools** in `../experiments/tools/`:
  - `auto_sweeps.py`: finds lean sweeps automatically and gives range and curve.
  - `cop_inches.py`: centre of pressure in plate inches.
  - `compare_math.py`: compares lean-math versions on the same sweeps.
  - `replay_cop.py`: runs a log through the firmware's corner-layout math (constants read from the firmware source) and prints speed/turn per hold. `--hold NAME=HH:MM:SS-HH:MM:SS` for any log's holds (default E9), `--passive HOLD` for the relaxed posture, `--trace` for the whole sweep, `--jumps` for tick-to-tick CoP change and dead-channel jumps.
  - `sweep_analysis.py`
- **Facts:**
  - Zero force ≈ **372** raw on every sensor (amplifier offset).
  - Centre of pressure = Σ(force·position)/Σforce, using raw − 372.
  - Riders: Mason ≈ Parker's size; Charles is heavier.

## Uploading firmware
- `pio run` builds; `pio run -t upload` uploads. `wsl_upload.py` hands off to the Windows Teensy tools.
- `tools/upload_on_plug.sh` (run in background) waits for the Teensy on USB (VID 16C0) and uploads the current build once, if it differs from the last one uploaded.
- Teensy USB shows as COM3.

## Plate and sensors (as of 2026-11-22: rectangle)
- **Rectangle layout** since 2026-11-22, one sensor per corner. Channels keep their pin names: **R = front-right, L = front-left, F = back-left, B = back-right**. Positions (inches from the old plus centre) live in `src/driveModes/lean_layout.h`.
- **Firmware setting `Sensor layout`** (dashboard, Lean control): `CORNER` (default) steers from the centre of pressure; `PLUS` is the old math, kept intact. Changing it needs a recalibration.
- **Corner calibration (every LEAN entry):**
  1. Sit upright and engaged (**active neutral**), short press: sets neutral. The calibrate LED then blinks slowly for 10 s.
  2. Optional, within those 10 s: relax (**passive**), short press again: records the relaxed posture, so slumping doesn't creep forward. Forward reach is then measured from the relaxed point.
  3. Optional: **hold** either press and lean forward, back, left, right, then release: sets this rider's full-lean reach (each ≥ 1.2"). It ends in DISARM. The reach is kept until a reach setting changes, the seat is empty for 3 s (new rider), or the profile changes.
  - A press with any corner carrying almost nothing (< 50 counts or < 5% of the load) is rejected: check that sensor. The LED stays on.
- **Sensor faults disarm LEAN:** a raw reading ≤ 340 or ≥ 4000 on any channel (both layouts), or, on the corner layout, the CoP stepping more than 0.75" between control ticks (a sensor dropping out).
- **Empty-plate baseline:** configured under "Corner layout sensors" (with per-channel gains, all 1.0). While DISARMED with the seat empty and steady for 3 s, the firmware re-zeroes it in RAM (logs "Empty plate re-zeroed"), if within 30 counts of the configured value. Reseating the plate moved it ≈10 counts (391/406/427/400 → 383/396/420/404).
- **Accuracy (E11, plate on):** 28.2 lb at four known spots located within 0.4" (typically 0.25") with gains 1.0. See `../experiments/README.md`.

## Plate and sensors (as of 2026-11-21, plus layout)
- **Sensors:** FlexiForce A502 force sensors in a plus, each through an MCP6004 transimpedance amp. Readings depend strongly on contact area.
- **Top plate:** sits on 1/4" hollow rubber stoppers. Tightening bolts are off; they made no difference.
- **Coordinates:** (0,0) is the plus centre. First hole 2.5" out, then 1" apart. Front/back holes 1–6, left/right 1–5. Current config **F5 B6 L5 R5**; B5 is equivalent.
- **Lean names** to call out: `passive-neutral`, `active-neutral`, `forward`, `back-min`, `back-touch`, `back-pivot`, `left`, `right`, `reseat`. Riders keep their hips passive (Parker is paralyzed from the waist down).
- **Planned:** a square (corner) layout centred ≈1.2" behind the current centre. Do a plate-on known-weight test first. Record footrest and backrest-strap heights in every test: both shift the rider's weight.

## Other context
- PCB (KiCad, plain text): `../../pcb/` (README has the pin/net table).
- Design doc text (partly stale): `../../cdr/cdr.txt`.
- Issues: `MasonNelson1/lean-control-firmware`, filed as FriChaFri (`GH_TOKEN=$(gh auth token --user FriChaFri) gh ...`; the default gh account is enterprise-blocked).
