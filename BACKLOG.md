# LEAN backlog

The main tracking lives in GitHub issues on `MasonNelson1/lean-control-firmware`
(#9–#17, filed 2026-11-21). The smaller items below haven't been filed yet.
Details: `notes/2026-11-21-meeting-findings.md`.

| Issue | Topic |
|---|---|
| #9 | Safety: detect silent VESCs and add a Teensy watchdog |
| #10 | Safety: stops coast or ramp; spec B not met (needs a decision) |
| #11 | Seat detection: mixed units, no hysteresis |
| #12 | Dynamic calibration: unbounded gains, lost on reboot, long press disarms |
| #13 | Lean math: switch to centre of pressure |
| #14 | Turning: overshoot, rate depends on the speed knob, smoothing kills steering |
| #15 | Test: force sensor drift and creep |
| #16 | Test: data-driven sensor position study |
| #17 | VESC Tool settings to check on both controllers |

Done on `charles/dev-setup` (not pushed yet): the DISARM button now works with an
empty seat, and a sensor at an ADC rail disarms LEAN (40bb3b7); `DBG SENSORS` /
`DBG VESC` telemetry lines (2681a57); live tap and query tools.

## Not filed yet
- ASSIST has no speed cap and feeds on itself (full boost above the speed limit).
- REMOTE drive packets have no CRC (config packets do).
- RPM commands are truncated to whole RPM (`vescSetWheelRPM` takes int32).
- A `building-with-charles.md` setup doc linked from CLAUDE.md, once the live setup is final.
- Let the dashboard and the tap share the one HC-06 connection.
