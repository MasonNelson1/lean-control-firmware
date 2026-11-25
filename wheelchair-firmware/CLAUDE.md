# LEAN Wheelchair — Control Firmware

Teensy 4.1 firmware for a lean-driven wheelchair (Boise State capstone). Read
the code; the docs can be stale.

- Build `pio run` · Upload `pio run -t upload` · Tests `./run-tests.sh`
- **Ask before uploading code that commands a motor.**
- Weird sensor data: investigate the hardware first (plate, bolts, preload, wiring). Prefer an easy hardware fix over a software workaround.
- Chair experiments are catalogued in `../experiments/README.md` (setup, protocol, data, results).
- PCB (KiCad, plain text): `../pcb/`, see its README.
- Meeting history and decisions: `../notes/` (`2026-11-21-summary.md` for the short version, `2026-11-21-meeting-detailed.md` for every decision and number). Session telemetry: `logs/sessions/`. Every Claude Code session (how decisions were reached): `../notes/claude-sessions/`.
- Live chair setup (Bluetooth telemetry, tools, upload rules, experiments): `building-with-charles.md`.
