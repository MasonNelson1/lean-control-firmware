# Chair session logs (full Bluetooth tap captures)

Every line the chair sent over the HC-06 during a session, as written by
`tools/chair_tap.py` (laptop wall time in `host_time`, then the firmware line:
the 20 Hz telemetry CSV row, `DBG ...` lines, firmware messages and
`TAP link up/down` events). They are gzipped copies of `logs/live/`, which stays
gitignored because the tap writes there live.

The trimmed per-experiment extracts are in `../../../experiments/data/`; use
these full logs when you need context around an experiment or the court ride.

```bash
zcat logs/sessions/2026-11-22-171917.csv.gz > /tmp/court.csv
python3 tools/chair_sql.py --file /tmp/court.csv --schema
python3 tools/chair_summary.py   # reads logs/live/current.csv; copy a log there to use it
```

| File | Wall time | Lines | What happened |
|---|---|---|---|
| `2026-11-21-202251.csv.gz` | Fri 11/21 20:22–21:20 | 72k | First tap session: empty-seat baseline (E1), plus-layout leans. Ends when an e-stop dropped the link (the tap now reconnects by itself). |
| `2026-11-21-212106.csv.gz` | Fri 11/21 21:22–23:21 | 176k | Plus-layout experiments E2–E6 and S1 (bolts, back-sensor position sweeps). |
| `2026-11-21-232616.csv.gz` | Fri 11/21 23:26–00:03 | 96k | E7 (heavier rider sweeps) and E8 (known weight). |
| `2026-11-22-142630.csv.gz` | Sat 11/22 14:26–16:26 | 356k | Corner (rectangle) layout: E9–E12 (sweeps, known weights, plate on), VESC settings read and written over CAN. |
| `2026-11-22-162641.csv.gz` | Sat 11/22 16:26–17:03 | 112k | New firmware and VESC settings checked, then the start of the court ride. |
| `2026-11-22-170317.csv.gz` | Sat 11/22 17:03–17:18 | 81k | Court ride, up to the tap crash at 17:18:41 (a cp1252 encoding error; ~36 s lost). |
| `2026-11-22-171917.csv.gz` | Sat 11/22 17:19–21:39 | 320k | Rest of the court ride with live tuning (to ~18:33), then the evening with the wheels off. |
| `2026-11-22-214625.csv.gz` | Sat 11/22 21:47–21:50 | 17k | Short check after the bench-test build was uploaded (the routine itself was not run). |

The ranges come from the timestamps in each file plus `notes/2026-11-21-*.md`.
Treat the "What happened" column as a pointer: check the notes and
`experiments/README.md` for exact wall times.
