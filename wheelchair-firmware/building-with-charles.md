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
  - Once a second:
    - `DBG SENSORS`: raw, floors, W, seat load, seated/calibrated/railed flags.
    - `DBG VESC`: per controller id, status-frame age, frames, ERPM, current, duty, MOSFET temperature, voltage.
    - `DYNAMIC_GAINS`.
  - The dashboard hides `DBG` lines.

## Reading the data
- **SQL:** `python3 tools/chair_sql.py "SELECT ..."` (also `--schema`, `--trend N`, `--file <csv>`).
  - Tables: `t` (telemetry rows), `raw` (20 Hz raw), `sens`, `vesc`, `msg` (text lines; `kind` `TAP`/`CORRUPT`/...).
- **Quick look:** `python3 tools/chair_summary.py [secs]`.
- **Experiment tools** in `../experiments/tools/`:
  - `auto_sweeps.py`: finds lean sweeps automatically and gives range and curve.
  - `cop_inches.py`: centre of pressure in plate inches.
  - `compare_math.py`: compares lean-math versions on the same sweeps.
  - `sweep_analysis.py`
- **Facts:**
  - Zero force ≈ **372** raw on every sensor (amplifier offset).
  - Centre of pressure = Σ(force·position)/Σforce, using raw − 372.
  - Riders: Mason ≈ Parker's size; Charles is heavier.

## Uploading firmware
- `pio run` builds; `pio run -t upload` uploads. `wsl_upload.py` hands off to the Windows Teensy tools.
- `tools/upload_on_plug.sh` (run in background) waits for the Teensy on USB (VID 16C0) and uploads the current build once, if it differs from the last one uploaded.
- Teensy USB shows as COM3.

## Plate and sensors (as of 2026-11-21)
- **Sensors:** FlexiForce A502 force sensors in a plus, each through an MCP6004 transimpedance amp. Readings depend strongly on contact area.
- **Top plate:** sits on 1/4" hollow rubber stoppers. Tightening bolts are off; they made no difference.
- **Coordinates:** (0,0) is the plus centre. First hole 2.5" out, then 1" apart. Front/back holes 1–6, left/right 1–5. Current config **F5 B6 L5 R5**; B5 is equivalent.
- **Lean names** to call out: `passive-neutral`, `active-neutral`, `forward`, `back-min`, `back-touch`, `back-pivot`, `left`, `right`, `reseat`. Riders keep their hips passive (Parker is paralyzed from the waist down).
- **Planned:** a square (corner) layout centred ≈1.2" behind the current centre. Do a plate-on known-weight test first. Record footrest and backrest-strap heights in every test: both shift the rider's weight.

## Other context
- PCB (KiCad, plain text): `../../pcb/` (README has the pin/net table).
- Design doc text (partly stale): `../../cdr/cdr.txt`.
- Issues: `MasonNelson1/lean-control-firmware`, filed as FriChaFri (`GH_TOKEN=$(gh auth token --user FriChaFri) gh ...`; the default gh account is enterprise-blocked).
