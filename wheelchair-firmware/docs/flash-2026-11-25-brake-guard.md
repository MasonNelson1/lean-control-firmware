# Flash 2026-11-25: stop-band brake guard (interim fix)

## What changed and why
Both swerves on 11/22 (17:08 right, 18:03 left) were the VESC tripping `ABS_OVER_CURRENT` and letting a wheel go for ~0.6 s. The cause: we sent a wheel that was still rolling a speed setpoint under the VESC's min ERPM (300 ERPM ≈ 5 wheel RPM), either ~0 at the end of a hard stop or a small reverse on the inside wheel of a turn. For any setpoint that small the VESC shorts the motor (duty 0), and at speed that short pulls more current than the trip limit. The firmware now catches this per wheel just before the command goes out: while a wheel rolls faster than 11 RPM (0.35 m/s), a near-zero or reversed command becomes 8 RPM in the wheel's own direction, so the VESC brakes it through its normal current-limited speed loop. Once the wheel is under 11 RPM, commands pass through as before, so stopping, turning in place and reversing from standstill are unchanged.

Details: `src/stop_band_guard.h`, constants and their reasons in `src/drive_constants.h`.

## Commit to flash
`ee2557c` on `charles/dev-setup` ("Firmware: stop-band guard, never send a near-stop RPM to a rolling wheel"). Later commits on the branch that only touch docs build the same firmware. After the upload the four mode LEDs chase three times, and `DBG BOOT stamp=` changes.

## What did NOT change
- Battery regen cap: `l_in_current_min` still −3 A.
- VESC motor braking limit: `l_current_min` −20 A (the 11/22 patch), and `s_pid_min_erpm` 300.
- The 50 RPM clamp, the smoother, the turning logic, the safety brake, the bench routine.
- ASSIST, DISARM and calibration (they don't send RPM commands, so the guard never touches them).

## 1. Bench test, wheels off the ground
Chair on blocks, both drive wheels clear, tap running (`python.exe tools/chair_tap.py --port COM4`).

1. Check the VESCs still hold 300: `python3 tools/vesc_conf.py s_pid_min_erpm` should print 300 for both ids (2 and 26) once a dump has come through (about 30 s). If either says 900, stop and tell Charles (the 8 RPM floor sits inside a 900 ERPM stop band).
2. Lowest speed knob. Drive the wheels with a teammate seated in LEAN (the chair on blocks) or from REMOTE.
3. Spin both wheels forward past ~30 RPM, then command a hard stop (sit upright / centre the remote). Repeat 5×.
4. Spin forward while turning, then stop. Spin in place one way, then flick to the other way. Repeat each 3×.
5. One run in reverse: spin backward past ~20 RPM, then stop.

What the log should show:
- `DBG EVT <ms> stop-guard L on meas=… cmd=… n=…` and the same for `R`, followed by `… off …` a fraction of a second later (wheels off the ground spin down fast). `meas` is above 11 on `on`, under 11 on `off`. In reverse, `meas` is negative.
- During the guard the CSV's smoothed RPM for that wheel reads 8 (or −8), never between −8 and 8 while the wheel is above 11.
- **No** `DBG VFAULT … ABS_OVER_CURRENT` lines.

```bash
python3 tools/chair_sql.py "SELECT host_time, detail FROM evt WHERE kind='stop-guard'"
python3 tools/chair_sql.py "SELECT host_time, side, name, imot, erpm FROM vfault"
```

If you see no `stop-guard` lines at all, the wheels never got above 14 RPM (it arms above 14): spin them faster.

## 2. Court test
Rider: a teammate, **not Parker**. One spotter walking alongside. Tap running and logging the whole time; note the wall time of each run.

1. Lowest speed knob first, then the next one up. Straight hard stops from full speed: 5 runs each.
2. Hard stops while turning (lean to turn, then sit up sharply): 5 runs each direction. This is the 18:03 case.
3. Slow down while turning without stopping (inside wheel slows a lot): 5 runs each direction.
4. Normal driving for a few minutes: turn in place from standstill, reverse from standstill, gentle stops. These should feel exactly as before.

Check after each set: `stop-guard` on/off lines for each hard stop, and no `ABS_OVER_CURRENT` in `vfault`. Also watch the last half-metre of each hard stop: the final ~1 m/s now brakes through the VESC's speed loop instead of the short, so the end of a stop may be a little softer. If the chair rolls noticeably further, or a wheel creeps at a slow, steady speed instead of stopping, stop the test and send Charles the log. Catalogue the session in `../experiments/README.md` as usual.

## The real fix
This is a stopgap that removes the trigger. The proper fix is the force/current-control rewrite, GitHub issue #20.
