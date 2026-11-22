# LEAN experiments

A record of every chair experiment, kept so anyone can reproduce it. Each
experiment lists its setup, protocol, the data file and its time windows, the
results and their interpretation. Raw data is in `data/` (laptop time in the
first column, then the firmware's telemetry CSV, firmware text lines and
`TAP` link events).

## How data is captured
- Telemetry comes over Bluetooth (HC-06 "LEANReciever", PIN 8976). Read it with
  `python.exe wheelchair-firmware/tools/chair_tap.py --port COM4` (Windows Python
  from WSL; it reconnects by itself after an e-stop). That writes
  `wheelchair-firmware/logs/live/*.csv` at 20 Hz.
- Query with `python3 wheelchair-firmware/tools/chair_sql.py --file <csv> "SQL"`.
- Raw counts: zero force on any sensor reads **≈372** (amplifier offset).
  Until the `DBG SENSORS` firmware is uploaded, raw counts are back-calculated
  from telemetry: raw = 400 + value / gain (uncalibrated floor 400, gains
  F=7 B=3 L=0.01 R=0.01). Left and right can't be read that way.

## Plate coordinates
(0,0) is the centre of the sensor plus (not the exact plate centre). Holes run
outward along each arm: the first is ≈2.5" from centre, then 1" apart. Front and
back have holes 1–6, left and right 1–5. A configuration is written as `F5 B2 L5 R5`.

## Lean types (use these names when calling out moves)
| Name | What the rider does |
|---|---|
| `passive-neutral` | Sits relaxed, all weight down, back against the backrest as it falls |
| `active-neutral` | Sits upright, slightly forward, holding posture |
| `forward` | Leans the upper body forward and holds |
| `back-min` | Leans back only until the backrest starts to push back (no pivoting) |
| `back-full` | Leans back into the backrest and pivots on it (hips tend to lift) |
| `left` / `right` | Leans the upper body sideways and holds |
| `reseat` | Stands up and sits back down |

Riders keep their hips passive (upper body only): the end user, Parker, is
paralyzed from the waist down.

## Index
| ID | Date | What | Data |
|---|---|---|---|
| E1 | 2026-11-21 | Empty-seat baseline: plate preload, drift, plate off vs on | `data/2026-11-21-202251-tap.csv` |
| E2 | 2026-11-21 | Mason, bolts as before: forward / back-full / back-min / neutrals | `data/2026-11-21-E2-mason-bolts-as-before.csv` |

---

## E1: Empty-seat baseline (2026-11-21, 20:22–21:20)
**Setup:** config `F5 B2 L5 R5`, top-plate bolts tightened by eye (Mason's usual way), cushion on, chair in DISARM, firmware at commit 7e90334 (Caleb), gains F7 B3 L0.01 R0.01, uncalibrated.

**Protocol:** seat left empty with the plate on (20:22–20:44); top plate removed (≈20:45–20:47, handling spikes); plate off (20:47–20:49+); plate reinstalled during the e-stop (21:20–21:22), with the bolts set "as before" by eye.

**Results (raw counts):**
| Condition | Front | Back |
|---|---|---|
| Plate on, empty (20:39–20:44) | 373 | 429 |
| Plate off (20:47–20:49) | 371.5 | 372 |
| Plate reinstalled, empty (21:22–21:24) | 378 | 431 |
- Zero force ≈ 372 on both sensors.
- The plate preload sits almost entirely on the back sensor (≈+57 to +59); the front gets ≈+1 to +6.
- Noise at zero load is about ±5 counts.
- Empty-seat drift with the plate on: back ≈437 → 426 → 430 over 18 min.

## E2: Mason, bolts as before (2026-11-21, 21:24:25–21:28:15)
**Setup:** as E1 after the reinstall. Rider: Mason (smaller rider), hips passive, strapped in.

**Protocol (calls from the meeting, wall times):**
- Sat down 21:24:25. `passive-neutral` 21:24:50–21:25:08.
- `forward` ×3, ~5 s each, 21:25:08–21:25:33.
- `back-full` ×3, 21:25:33–21:25:55.
- `back-min` ×2–3, 21:25:59–21:26:20 and 21:26:44–21:27:15.
- `active-neutral` held 21:27:22–21:27:53.
- `passive-neutral` from 21:27:56.

**Results (raw counts, 3 s averages):**
| Phase | Front | Back |
|---|---|---|
| passive-neutral (start) | 378–382 | 655–680 |
| forward | 460–600 | 590–670 (drops 60–90) |
| back-full | 372 | 775–855 |
| back-min | 372 | 940–1004 |
| between back reps | 372 | 845–960 |
| active-neutral | 372–375 | 790–920 |
| passive-neutral (end) | 380–400 | 680–735 |

**Interpretation (to verify):**
1. At `passive-neutral` with a rider, the front sensor carries ~zero load. It only senses forward lean, so the back-lean signal is the back sensor alone. This fits Charles's seesaw model: the plate pivots on the line through the side sensors, and the rider's centre of mass sits behind it.
2. During a back lean the front stays at its floor (it can't go lower), so it gives no back-lean information. During a forward lean the back drops 60–90 counts.
3. `back-min` loads the back sensor more than `back-full`. Mason: once he fights the backrest, his butt lifts.
4. After the back leans, the back sensor stayed ≈200 counts above its starting neutral for ~2 min, until Mason shifted. Either the plate catches on the bolts or threads, or the sensor/rubber creeps. Calibrating in that state would eat most of the back-lean range.

**Next:** repeat E2 with the bolts loosened (E3), ending with 30 s of `passive-neutral` to check whether the back sensor returns.
