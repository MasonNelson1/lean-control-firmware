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

## Lean types (call these out loud; Charles's names, 2026-11-21)
| Name | What the rider does |
|---|---|
| `passive-neutral` | Sits relaxed (slouched), all weight down, settled against the backrest |
| `active-neutral` | Sits upright, holding posture, the way you'd sit to drive |
| `forward` | Leans the upper body forward and holds |
| `back-min` | A small back lean, well short of the backrest |
| `back-touch` | Leans back until the backrest just touches and starts to push back |
| `back-pivot` | Leans back into the backrest and pivots on it (hips tend to lift) |
| `left` / `right` | Leans the upper body sideways and holds |
| `reseat` | Stands up and sits back down |

Older names in E2/E3 notes: slouch = passive-neutral, tall = active-neutral, back-push / back-full = back-pivot. In E2, "back-min" meant back-touch.

Riders keep their hips passive (upper body only): the end user, Parker, is
paralyzed from the waist down.

## Index
| ID | Date | What | Data |
|---|---|---|---|
| E1 | 2026-11-21 | Empty-seat baseline: plate preload, drift, plate off vs on | `data/2026-11-21-202251-tap.csv` |
| E2 | 2026-11-21 | Mason, bolts as before: forward / back-push / back-touch / slouch / tall | `data/2026-11-21-E2-mason-bolts-as-before.csv` |
| E3 | 2026-11-21 | Mason, bolts loosened: same moves as E2 | `data/2026-11-21-E3-mason-bolts-loose.csv` |

Charts: `E2-E3-compare.html` (E2 vs E3 by lean type, same axes) and `E2-E3-leans.html` (time series).

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
4. ~~After the back leans, the back sensor stayed ≈200 counts high (plate catching or creep).~~ **Corrected after E3:** that was mostly posture, not sticking. Between the back reps and in `active-neutral`, Mason sat upright, and `active-neutral` loads the back sensor ≈100–150 counts more than `passive-neutral` in both runs. After returning to `passive-neutral`, the back came back to within ≈+20–50 of its start.


## E3: Mason, bolts loosened (2026-11-21, 21:35–21:39:30)
**Setup:** as E2, but all top-plate bolts were loosened as far as possible while still keeping some thread engagement (≈21:33–21:35, chair powered). The backrest was off at first; it was reinstalled before the run (21:37:22). Charles heard the plate rub on the threads but thought it wasn't catching; Mason said it felt smooth. A pad is slightly pinched under the plate edge (Charles: probably not significant).

**Protocol (wall time = 19:38:10 + recording offset):**
- Empty seat, loose bolts: 21:35:05–21:36:41.
- Sat down: 21:36:45.
- `passive-neutral`: 21:37:33–21:37:43.
- `active-neutral`: 21:37:44–21:37:54.
- `back-pivot` (one long hold): 21:37:59–21:38:11.
- `back-touch`: 21:38:12–21:38:33.
- `forward` (one hold): 21:38:35–21:38:48.
- `passive-neutral`: 21:38:49–21:39:00.
- `active-neutral`: 21:39:01–21:39:30.

**Results (raw counts, 4 s averages), compared with E2:**
| Phase | Front E3 | Back E3 | Back E2 (bolts as before) |
|---|---|---|---|
| empty seat | 372–375 | 400 → 423 (slowly rising) | 431 |
| slouch (start) | 373–376 | 680–726 | 655–680 |
| tall | 372 | 802–859 | 790–920 |
| back-push | 372 | 770–853 | 775–855 |
| back-touch | 372 | 918–968 | 940–1004 |
| forward | 533 | 669–674 | 590–670 (front 460–600) |
| slouch (end) | 372–373 | 714–732 | 680–735 |
| tall (end) | 372–374 | 772–812 | n/a |

**Interpretation:**
1. **Loosening the bolts changed little.** The empty-seat back preload dropped ≈25 counts (431 → ~405, then settled upward). The lean responses were the same within the variation between reps. Charles's hypothesis that the bolts block the plate from tilting back isn't supported at these bolt settings: the plate wasn't binding much.
2. **Replicated:** the front sensor is ~zero at `passive-neutral`, `active-neutral` and all back leans, so it only senses `forward`. `back-touch` loads the back sensor more than `back-pivot` (≈950 vs ≈810).
3. **New, and important for consistency:** `active-neutral` vs `passive-neutral` alone moves the back sensor ≈100–150 counts. That's half to two-thirds of a full back lean (`back-touch` ≈ +250 over `passive-neutral`). Calibrating in one neutral posture and riding in the other shifts the lean reading by about half a back lean. "Neutral" has to be defined and coached, or the math has to be insensitive to it.
4. Moving from back leans to `forward` and then `passive-neutral` brings the back reading back to its start (+20–50). Mason's calibrate-while-leaning-forward trick effectively resets the posture to a low-back neutral.

---

## Plan: sensor position study (updated 2026-11-21 22:05, led by Claude)
**Physics (Charles's seesaw model):** the plate tips about the line through the two side sensors. The rider's centre of mass sits behind that line, so the front sensor lifts to zero at neutral. No existing hole moves that line; only new side holes behind (0,0) do.

**Changed conditions from here on:** wheels ON (chair off the casters), top-plate bolts REMOVED (the team found they make no difference, per E2 vs E3).

| Step | Config | What changes | Question |
|---|---|---|---|
| 0 | n/a | Upload `charles/dev-setup` firmware while the chair is e-stopped (DBG lines) | See all four sensors raw |
| E4 | `F5 B2 L5 R5`, no bolts | Baseline incl. left/right | Full four-sensor picture of the current setup |
| E5 | `F5 B5 L5 R5` | Back sensor moved out | Does a longer back lever widen back-lean vs active-neutral? |
| E6 | best of E4/E5 + front shim | Washers under the front stopper so the front reads ≈ +100 empty | Does a pre-loaded front sensor drop on back leans (a second back signal)? |
| E7 | best + `L1 R1` | Side sensors moved inward | Do the sides carry more of the weight split; does the left/right swing change? |
| later | side holes ≈2–3" behind (0,0) | Drill | The real seesaw test |

**Every step:** 30 s empty seat (preload check) → rider protocol (Mason, hips passive):
`passive-neutral` 10 s → `active-neutral` 10 s → `forward` 5 s ×2 → `back-min` 5 s ×2 → `back-touch` 5 s ×2 → `back-pivot` 5 s ×1 → `left` 5 s ×2 → `right` 5 s ×2 → `passive-neutral` 10 s → `reseat` ×2.

**Scored on:** back-lean swing over `active-neutral`; front swing on `forward`; the passive/active-neutral gap as a fraction of the back swing (smaller is better); left/right swing and symmetry; reseat repeatability.
