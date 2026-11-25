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

Riders keep their hips passive (upper body only): the end user (the rider) is
paralyzed from the waist down.

## Index
| ID | Date | What | Data |
|---|---|---|---|
| E1 | 2026-11-21 | Empty-seat baseline: plate preload, drift, plate off vs on | `data/2026-11-21-202251-tap.csv` |
| E2 | 2026-11-21 | Mason, bolts as before: forward / back-push / back-touch / slouch / tall | `data/2026-11-21-E2-mason-bolts-as-before.csv` |
| E3 | 2026-11-21 | Mason, bolts loosened: same moves as E2 | `data/2026-11-21-E3-mason-bolts-loose.csv` |
| S1 | 2026-11-21 | Mason, sensitivity sweep at F5 B2 L5 R5 | `data/2026-11-21-S1-sweep.csv`, chart `S1-sweep-sensitivity.html` |
| E5 | 2026-11-21 | Mason, back sensor moved to B5, sweep | `data/2026-11-21-E5-B5-sweep.csv`, chart `E5-vs-S1.html` |
| E6 | 2026-11-21 | Mason, back sensor at B6, sweeps with reps (back ×3) | `data/2026-11-21-E6-B6-sweep.csv`, chart `E6-B2-B5-B6.html` |
| E4 | 2026-11-21 | Mason, all four sensors raw (new firmware), no plate bolts, wheels on | `data/2026-11-21-E4-mason-four-sensors.csv`, chart `E4-four-sensors.html` |

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

## E4: Mason, all four sensors (2026-11-21, 22:16–22:20)
**Setup:** firmware `charles/dev-setup` (DBG lines; raw counts for all four sensors at 1 Hz in `DBG SENSORS`). Config `F5 B2 L5 R5`. Top-plate bolts removed, wheels on, backrest on, chair powered, DISARM, uncalibrated. Both VESCs now report status (ID 2 had shown 0 frames earlier only because the chair was unpowered).

**Protocol (Mason narrated; wall = 19:38:10 + recording offset):**
- Empty seat: 22:16:00–22:16:30.
- `passive-neutral`: 22:17:52–22:18:05.
- `active-neutral`: 22:18:06–22:18:11.
- `back-touch`: 22:18:12–22:18:23.
- `back-pivot`: 22:18:24–22:18:29.
- `forward`: 22:18:31–22:18:38.
- `right` (10 s): 22:18:39–22:18:50.
- `left` (10 s): 22:18:57–22:19:08.
- `active-neutral`: 22:19:09–22:19:20.
- half-right: 22:19:21–22:19:26.
- Extra unnarrated forward holds: 22:19:51–22:20:14.

**Results (above zero force, raw − 372):**
| Lean | Front | Back | Left | Right | Total | CoP x (R−L)/Σ | CoP y (F−B)/Σ |
|---|---|---|---|---|---|---|---|
| empty | 2 | 50 | 8 | 6 | 66 | | |
| passive-neutral | 13 | 282 | 237 | 412 | 944 | 0.19 | −0.28 |
| active-neutral | 15 | 302 | 306 | 504 | 1127 | 0.18 | −0.25 |
| forward | 237 | 157 | 477 | 668 | 1539 | 0.12 | 0.05 |
| back-touch | 0 | 440 | 224 | 342 | 1006 | 0.12 | −0.44 |
| back-pivot | 0 | 383 | 206 | 313 | 902 | 0.12 | −0.42 |
| left | 37 | 317 | 745 | 264 | 1363 | −0.35 | −0.21 |
| right | 13 | 380 | 145 | 1003 | 1541 | 0.56 | −0.24 |
| half-right | 23 | 357 | 224 | 783 | 1387 | 0.40 | −0.24 |

**Interpretation:**
1. The plate rests on the two side sensors plus the back; the front is ~0 at neutral (the seesaw model is confirmed with real side data).
2. On `forward`, left and right rise ≈+240 each, as much as the front (+224). The side sensors are a forward signal (Charles's idea).
3. Left/right leans swing the side sensors by +500 to +600, 4–5× the front/back swings. That's why side gains of 0.01 still turned well.
4. **Centre of pressure fixes the posture problem in this data:**
   - `passive-neutral` vs `active-neutral` differ by only 0.03 in CoP y, against a back-lean swing of 0.16 and a forward swing of 0.33. In raw back counts the gap had been about half a back lean.
   - `back-pivot` lands with `back-touch` (−0.42 vs −0.44) instead of reading weaker.
   - Total load varies a lot between leans (902 to 1541), which is exactly what dividing by the live total cancels.
   - Strong support for issue #13 (centre-of-pressure lean math).
5. Asymmetry: right carries ≈175 more than left at neutral, and the right lean peaks higher (+590 vs +510 over neutral). Rider posture or sensor/amp gain differences; worth one swap test (swap the L and R sensor cables).
6. Forward is still the weakest axis: CoP y swing +0.33 forward vs −0.16 back from passive-neutral, but the forward reading needs a big lean.

## Plan: sensitivity sweeps (Charles, 2026-11-21)
**Goal:** know how sensitive the sensors are at *every* lean position, not just the held end points. Ideally the rider can make micro-movements anywhere in the range and the chair responds.

**Data:** `DBG R` lines give raw counts for all four sensors at 20 Hz (firmware after commit "Telemetry: 20 Hz raw sensor line"). Query the `raw` table in `chair_sql.py`.

**Protocol (one sweep per direction, rider counts out loud):** start in `active-neutral`, lean **slowly and evenly** to full over a 5-count, hold 2, return evenly over a 5-count, then hold neutral 3 s. Do it for forward, back (to `back-touch`), left and right, twice each. ≈2 min total.

**Analysis:**
- Treat sweep progress as linear in time (an even count).
- Plot centre of pressure (and each raw sensor) against progress 0→100 %.
- The slope is the sensitivity. Flat stretches are dead zones; steep stretches are where the chair will feel twitchy.
- Show all four directions on one chart, with a sensitivity strip under each.

**Better ground truth, later:** strap a phone to the rider's chest running a sensor-logging app (e.g. phyphox) to record torso angle, then plot signal vs actual lean angle instead of time.

## S1: sensitivity sweep, Mason (2026-11-21, 22:40:10–22:41:21)
**Setup:** as E4, with the firmware's 20 Hz `DBG R` raw line. Data: `data/2026-11-21-S1-sweep.csv`; chart `S1-sweep-sensitivity.html`; analysis `tools/sweep_analysis.py`.

**Protocol:** active-neutral, then one slow 5-count lean per direction to full and back. The protocol called for two; Mason did one. Full forward = chest almost to knees. Back = back-touch.
- Transcript times run about 6 s ahead of the data, so segment boundaries were taken from the signal (wall time).
- Forward: 22:40:23.4–22:40:28.4, hold to 22:40:37.
- Back: 22:40:41.4–22:40:49.4.
- Right: 22:40:59.4–22:41:03.5.
- Left: 22:41:11.4–22:41:15.4.
- Neutral hold for the noise estimate: 22:41:21.4–22:41:26.4.

**Results:** centre of pressure (raw − 372, divided by the live total). Change from neutral at full lean, and how many steps the chair could tell apart:
| Direction | Range | Steps (range / 2σ noise) | Shape |
|---|---|---|---|
| forward | +0.19 in y | ≈10 | steady, slightly front-loaded |
| back | −0.11 in y | ≈6 | rises, flat from 20–60 %, then climbs |
| right | +0.22 in x | ≈22 | steady |
| left | −0.22 in x | ≈22 | steady |

Noise at rest: σx 0.005, σy 0.010.

**Interpretation:**
1. **No dead zone at the start** in any direction with centre-of-pressure math. The "nothing, then everything" feel is from the current firmware math (deadband, exponent curve, unequal gains), not the sensors. More support for #13.
2. **Back is the weak axis:** smallest range, a mid-lean plateau, and only ~6 steps of resolution. It's the first thing a sensor-position change (B5, or the side line moved back) should improve.
3. After the back lean, CoP y didn't return to neutral (−0.42 vs −0.33) until the next lean. Posture or hysteresis; watch for it in later sweeps.
4. Mason leans a lot (his preference). Charles thinks ideal is about half that. With ~22 steps on left/right, a half lean still leaves ~11 steps; back would have only ~3.

## E5: back sensor at B5, sweep (2026-11-21, 22:55:20–22:56:38)
**Setup:** as S1 but config `F5 B5 L5 R5` (B5 is where the back sensor sat two days ago). Backrest strap height not standardized (it was moved on 11/20). Empty seat measured after the sweep: raw F/B/L/R = 375/417/377/377 (back preload +45 vs about +50–55 at B2).

**Protocol (wall time from the signal):**
- passive-neutral: 22:55:22–22:55:31.
- active-neutral: 22:55:33–22:55:40 (also the noise window).
- forward: 22:55:40.9–22:55:45.4.
- right: 22:55:54.4–22:55:59.0.
- left: 22:56:04.6–22:56:09.2.
- back-touch ×2: 22:56:13.4–22:56:18.2 and 22:56:22.4–22:56:26.9.
- Mason stood up at 22:56:38.

**Results vs S1 (B2):**
| Direction | S1 range | E5 range | S1 steps | E5 steps |
|---|---|---|---|---|
| forward | 0.19 | 0.28 | 9.7 | 18.0 |
| back | 0.11 | 0.16 (both leans) | 5.7 | 10.2 / 10.1 |
| left | 0.22 | 0.33 | 22.1 | 27.8 |
| right | 0.22 | 0.29 | 22.2 | 24.5 |

**Correction (after Mason and Charles questioned why the unmoved sensors also "improved"):** use left/right as a control. They weren't moved, yet they grew 31–50%, about as much as back (+40%). Back range relative to the sides is unchanged: back/left = 0.50 (S1) vs 0.48 (E5), back/right = 0.50 vs 0.55. **So most of the range gain is Mason leaning further or faster at B5, not the sensor position.** What still favours B5 is the shape: the mid-lean plateau is gone in both B5 back leans. Error bands: sensor noise is tiny (σ 0.006–0.010); the uncertainty is lean-to-lean variation. The two B5 back leans agree at the end (0.158 vs 0.156) but differ by up to 0.05 mid-lean. From B6 on: 3 reps per direction, and report back range normalized to the side ranges.

**Original interpretation (superseded):** B5 beats B2. Back range is up ~40%, and the mid-lean plateau is gone in both back leans (repeatable). Caveat: one sweep per config, and Mason leaned somewhat faster and maybe deeper at B5, which inflates every range a little. The change in the back curve's shape is the most trustworthy part. **Next:** try B6 (Charles); if it improves again, keep going outward.

## E6: back sensor at B6, repeated sweeps (2026-11-21, 23:02–23:07:30)
**Setup:** config `F5 B6 L5 R5` (B6 never tried before). The plate slid slightly forward during setup. Empty seat 23:02:15–23:04:10: raw 373/428/376/377 (back preload +56).
**Protocol:** passive-neutral, active-neutral (23:05:51–23:05:57, the baseline), forward ×2, right ×2, left ×2 (one too fast and redone), back-touch ×3.
**Method:** sweeps found automatically by `tools/auto_sweeps.py` (CoP leaves neutral by > 0.06, 0.25 s smoothing); range = excursion at full lean. Return-overshoot and stand-up artefacts discarded. E5 was re-measured the same way; S1 sides by hand.

| Config | Back (each lean) | Forward | Left | Right |
|---|---|---|---|---|
| B2 (S1) | 0.11–0.18 (depends on the baseline) | 0.19, 0.20 | 0.22 | 0.22 |
| B5 (E5) | 0.137, 0.150, 0.158 | 0.288 | 0.345 | 0.325 |
| B6 (E6) | 0.151, 0.183, 0.158 | 0.252, 0.244 | 0.316, 0.351 | 0.313, 0.299 |

Noise at rest σx 0.005, σy 0.011.

**Interpretation:** no back-sensor position clearly wins. The back-lean dots overlap across B2/B5/B6, and the lean-to-lean spread (≈0.03) is as big as the differences between positions. Back stays at ≈ half the side range wherever the sensor sits, so the limit looks physical (the backrest takes the load during a back lean), not a hole-position problem. Per Charles's rule ("if B6 isn't clearly better, keep B5"): **keep B5**. Higher-value next steps: the centre-of-pressure math (#13), drilling side holes behind centre (seesaw), or sensing load on the backrest itself.

### Notes after E6 (Charles & Mason, 23:08–23:12)
- **Charles/Mason's view on back:** the limit is mostly how far you *can* lean back. The backrest stops the lean at about half (or less) of a forward lean. That fits the data as well as "the backrest takes the load"; without a torso-angle measurement the two can't be separated. Either way it's physical; the team accepts it ("we're not gonna lean as much back anyway").
- **Seating position between runs** shifts the side baseline. Neutral CoP x was 0.077 (S1), 0.127 (E5), 0.067 (E6). That's why neutral must be re-zeroed each time the rider sits (calibration), and why the sides need reps.
- **Idea: square formation** (sensors at the corners instead of a plus). Every lean then loads two sensors and the seesaw pivot lines change. Worth a test later.
- Chart `E4-sensor-deltas.html`: change per sensor from active-neutral for each lean. Sides carry ~2/3 of the forward signal and as much of the back signal as the back sensor does.
- **Next (Charles):** front-sensor position. Keep B6 (no different from B5) and move only the front (F5 → F2), so one variable changes. Prediction from the seesaw model: the front still reads ≈0 at neutral as long as the rider's centre of mass is behind the side line.

## Math comparison on real sweeps (E6 data; `math-comparison.html`, `tools/compare_math.py`)
Four versions of the lean math run on the same E6 sweeps (Mason, F5 B6 L5 R5), calibrated at active-neutral:
- **A** current firmware with tonight's gains (F7 B3 L0.01 R0.01)
- **B** current firmware with default gains (F2 B3 L1 R1)
- **C** centre of pressure (raw − 372 over the live total), each direction scaled by its calibrated range
- **D** all-sensor sums (fore-aft = ΔF + ΔL + ΔR − ΔB, turn = ΔR − ΔL), each direction scaled by its calibrated range (close to the council branch's "per-pair load gain over per-direction range")

| Direction | Steps A | B | C | D | Max command A | B | C | D |
|---|---|---|---|---|---|---|---|---|
| forward | 6.3 | 9.4 | 9.6 | 10.1 | 1 (saturates at 36 % of the lean) | 1 (at 83 %) | 1 (90 %) | 1 (90 %) |
| back | 4.9 | 5.3 | 6.1 | 5.8 | 0.75 | 0.56 | 0.99 | 0.99 |
| left | 0 | 28.8 | 27.0 | 28.6 | 0.007 (dead) | 0.64 | 0.97 | 0.96 |
| right | 0 | 27.7 | 25.1 | 28.2 | 0.006 (dead) | 0.63 | 0.98 | 0.98 |

**Findings:**
1. With tonight's gains, lean turning is effectively OFF, forward is on/off (full command at a third of the lean), and back can't reach full reverse.
2. **Resolution is about the same for every sensible version** (≈10 forward, ≈6 back, ≈27 left/right). Combining sensors didn't add resolution, because the "noise" is the rider's own sway, which is correlated across sensors. **The limit is how still a person can sit.**
3. The math decides how that resolution is *used*. C and D give every direction a smooth 0 → full response across the whole lean. C also cancels posture and weight (E4). The council branch's approach is close to D, and this data doesn't argue against it.

## Where the rider's weight actually sits, in inches (from E6, F5 B6 L5 R5)
With four point supports, the centre of pressure in plate coordinates is exact by moment balance: x = Σ(force·x)/Σforce, y = Σ(force·y)/Σforce. This assumes counts ∝ force and equal sensor gains (not yet checked; see the known-weight test). Sensor positions: F (0, +6.5"), B (0, −7.5"), L (−6.5", 0), R (+6.5", 0).
- Active-neutral: **x +0.44", y −2.19"** (passive: +0.68", −2.29"). Mason's weight sits ≈2.2" behind (0,0) and slightly right.
- Envelope across every lean in E6 (2–98 %): **x −1.6"…+2.4", y −3.5"…−0.5"**.
- **The centre of pressure never crosses y = 0**, the line through the side sensors. That's why the front sensor reads ~zero: the seesaw model is confirmed in physical units.

**Square layout sizing (proposal):**
- Centre the square on the rider's neutral, ≈(+0.3", −2.0"), not on (0,0).
- Keep the whole lean envelope (≈4.4" wide × 3.2" deep) in the inner half of the square, so every corner stays loaded. That means corners ≈5" from the centre in x and y: x = ±5", y ≈ +3" (front pair) and ≈ −7" (back pair).
- Check that it fits the plate and seat before drilling, and repeat with a heavier rider (Charles).

## E7: Charles (bigger rider), feet on footrest vs feet hanging (2026-11-21, 23:38:50–23:48:40)
**Setup:** F5 B6 L5 R5, no plate bolts. Backrest strap raised and the backrest loosened partway through (≈23:41–23:42), because Charles is bigger and the backrest fought him early. Data: `data/2026-11-21-E7-charles-sweeps.csv`; centre of pressure in inches via `tools/cop_inches.py`. The rider is about Mason's size or a little smaller (Charles); Charles is noticeably heavier (seat total ≈1500–2400 counts vs Mason's ≈1000–1500).

**Protocol:**
- Feet on footrest (knees up), 23:38:58–23:44:30: passive, active, forward ×2, right ×2, left ×3, back several (some redone after the strap and backrest changes; one with legs tense, then "legs deactivated").
- Feet hanging off the footrest, 23:44:32–23:48:40: passive, active, forward ×2, right, left ×2, back several.

**Results (centre of pressure, inches from (0,0); 3 s averages):**
| | Feet on footrest | Feet hanging |
|---|---|---|
| neutral y | −1.4 (passive) … −2.0 (active) | −0.1 … −0.4 |
| forward peak y | −0.4 … −0.65 (still behind the side line) | +0.8 … +1.1 (crosses it; front loaded) |
| back lean y | −2.3 … −2.5 (only ≈0.4" behind neutral) | −0.5 … −0.7 (≈0.3" behind neutral); one pivot at −2.4 |
| right peak x | +1.9 | +1.8 |
| left peak x | −0.6 | −0.5 |

**Interpretation:**
1. **Foot position moves the rider's neutral about 1.5" forward** (legs' weight lands on the thighs and front of the seat). With feet hanging, the centre of pressure reaches the side line and forward leans load the front sensor. **Footrest height is a big lever on the seesaw problem**, and the rider's legs will be passive dead weight, so set the footrest deliberately and record its height.
2. Charles's back lean moves the centre of pressure only ≈0.3–0.5", against ≈1.3–1.5" forward. Back is the weakest axis for a bigger rider too. He noted his legs tense up when leaning back; with legs relaxed (closer to the rider) he pivots on the backrest.
3. **Both riders lean further right than left** (Charles +1.9" vs −0.6"; Mason was right-heavy too). Two different riders sharing a bias points to a sensor/amplifier gain difference or seat geometry, not posture. The known-weight test will tell which.
4. Square sizing across both riders and both foot positions: centre-of-pressure envelope x −1.6"…+2.4", y −3.5"…+1.1". Centre ≈(+0.4", −1.2"); corners ≈5" out → x ≈ −4.6"/+5.4", y ≈ +3.8" (front pair) / −6.2" (back pair). Re-check after the gain test.

## E8: known-weight sensor check (2026-11-21, 23:51:55–23:55:44)
**Setup:** plate and seat off. A 7.2 lb water jug (Charles's scale) sitting on the ring end of a 7/16" wrench, the ring centred on each sensor in turn. Data: `data/2026-11-21-E8-known-weight.csv`.
**Results (settled reading over each sensor's own zero):**
| Sensor | Window | Zero | Loaded | Δ counts | Relative gain | Correction |
|---|---|---|---|---|---|---|
| front | 23:52:07–13 | 372 | 443 | 71 | 0.89 | ×1.12 |
| back | 23:52:20–25 | 372 | 448 | 76 | 0.96 | ×1.05 |
| left (redo) | 23:55:24–44 | 375 | 468 | 93 | 1.17 | ×0.85 |
| right | 23:53:06–14 | 372 | 450 | 78 | 0.98 | ×1.02 |

The first left attempt (23:52:29–51) was unstable because the jug wasn't sitting steadily. Readings take ~5–8 s to settle after placement (creep). One trial each, so ±≈10 %.

**Interpretation (superseded by E8b):** the sensors aren't wildly different, but **left reads ≈20 % higher** than the others for the same weight. That would make riders look *left*-biased, yet both riders measured *right*-biased. So **the right bias is real, not a sensor artefact** (posture, maybe handedness as Mason suggested, or the seat sits off-centre), and the gain-corrected bias is slightly larger. Use these correction factors in the centre-of-pressure math and when placing the square. The gain-corrected square centre shifts ≈0.1–0.2" further right.

## E8b: known-weight redo with steadier contact (2026-11-21, 23:58:46–23:59:46)
**Setup:** Charles reworked the rig for more consistent contact area (jug plus a water bottle, ≈9 lb on his scale). Order: front, left, right, back. Data: `data/2026-11-21-E8b-known-weight-redo.csv`.
| Sensor | Window | Zero | Loaded | Δ counts (E8b, ≈9 lb) | Δ counts (E8, 7.2 lb) |
|---|---|---|---|---|---|
| front | 58:48–58:56 | 371.6 | ≈570 | ≈198 | 71 |
| left | 59:00–59:09 | 374.7 | ≈527 | ≈152 | 93 |
| right | 59:16–59:24 | 372.1 | ≈522 | ≈150 | 78 |
| back | 59:38–59:46 | 372.4 | ≈479 | ≈107 | 76 |

**Interpretation:**
- The two runs disagree a lot. Front/back went from equal (E8) to ≈2:1 (E8b), and every reading roughly doubled for 25 % more weight.
- **The force sensors' reading depends strongly on contact area and shape, not just weight** (Charles flagged this). So a hand-placed jug test can't pin sensor gains better than ≈±30 %.
- **Left vs right agree** in the careful run (152 vs 150), and left was never low. **The riders' right bias is real.**
- **Front vs back gains are unknown** (equal in one run, 2:1 in the other). That matters for fore-aft centre of pressure and the square's front/back placement.
- **Better test (tomorrow):** with the plate and its rubber stoppers on, the real contact, put a known weight at marked spots directly over each sensor and at (0,0). That measures the system as it's actually used, and gives each sensor's gain plus a centre-of-pressure accuracy check in one go.

## E12: plate-on known weight, rectangle layout (2026-11-22)
Data: `data/2026-11-22-E12-plate-on-28lb.csv` (the bare-sensor weight repeat is E11, `data/2026-11-22-E11-weight-repeat.csv`; it isn't written up here yet).

**Setup:** rectangle (corner) layout, plate and seat on. A 28.2 lb weight set at four marked spots in plate inches (origin at the old plus centre, x right, y forward). Gains all 1.0. Wall times are not in this write-up; the results below are as reported by the team.

| True spot | Measured CoP | Error |
|---|---|---|
| (0, −2.19) | (−0.15, −1.95) | 0.28" |
| (−2.375, 0) | (−2.43, +0.19) | 0.20" |
| (0, +2.25) | (+0.40, +2.37) | 0.42" |
| (+2.125, 0) | (+2.38, +0.31) | 0.40" |

**Interpretation:** the centre-of-pressure math locates a known load within 0.4" (typically 0.25") with unit gains, through the real plate and stoppers. E10's "back-right reads half" was a bare-sensor contact artefact, so the firmware keeps gains at 1.0. Rider calibration (neutral, relaxed posture, reach) is relative, so a fixed offset of this size cancels; what remains is small next to a 1.3–2" lean.

## Corner-layout firmware replay of E9 (`tools/replay_cop.py`)
Runs the E9 sweep through the firmware's corner math (constants read from `lean_layout.h` and `DriveSettings.cpp`), calibrated on the active-neutral hold. Speed and turn are after the response curves.

| Hold | Neutral only: speed / turn | With relaxed posture (`--passive passive-neutral`): speed / turn |
|---|---|---|
| passive-neutral | **+0.140** / 0 (slump creeps) | **0** / 0 |
| active-neutral | 0 / 0 | 0 / 0 |
| forward | +0.987 / +0.073 | +0.495 / +0.073 (reach measured from relaxed; 2.0" default is long, a hold calibration gives 1.32" and full speed) |
| right | −0.062 / +0.982 | −0.092 / +0.982 |
| left | 0 / −0.968 | −0.004 / −0.968 |
| back-touch | −0.977 / +0.011 | −0.979 / +0.011 |

`--jumps`: the largest healthy CoP change between 20 Hz samples was 0.54" (11.7 in/s); getting out of the seat reached 1.17" per 50 ms. A channel dropping to 372 counts jumps the CoP by F ≥1.96", B ≥1.07", R ≥0.53" (1.06" for 95% of postures), L ≥0.22" (1.13" median). The firmware stops and disarms on a step over 0.75" between control ticks.
