"""Replay a tap log through the firmware's corner-layout lean math, offline.

usage: replay_cop.py [tap.csv] [--neutral HOLD] [--deadzone IN] [--trace [BUCKET_S]]

Defaults to the E9 rectangle sweep (2026-11-22). Calibrates neutral the way the
chair does (average raw counts during the neutral hold, rounded like
LeanDriveMode::calibrate), then runs every 20 Hz `DBG R` sample through the
corner math and prints the mean output for each hold:

  x, y      centre of pressure, inches (plate coordinates: x right, y forward)
  dx, dy    offset from the calibrated neutral
  lean      speed/turn from -1 to 1 before the response curves (cornerCommand)
  speed,turn after the response curves and reverse-turn flip (shapeCommand)
  left,right wheel commands after turn mixing, as fractions of the speed knob

Constants are read from the firmware source, never copied: sensor positions,
empty-plate baselines, reach and deadzone from src/driveModes/lean_layout.h;
response curves and turn mix from src/config/DriveSettings.cpp. Gains are the
firmware default (1.0). Change the firmware and this script follows.
tests/native/corner_layout_test.cpp checks the C++ gives the same numbers.
"""
import argparse
import csv
import math
import os
import re
import statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "..", "wheelchair-firmware", "src")
E9 = os.path.join(HERE, "..", "data", "2026-11-22-E9-E10-rectangle-sweep-and-weights.csv")

# Hold windows called out during the E9 sweep (laptop time).
HOLDS = [
    ("passive-neutral", "14:29:31", "14:29:38"),
    ("active-neutral", "14:29:41", "14:29:46"),
    ("forward", "14:29:59", "14:30:03"),
    ("right", "14:30:09", "14:30:12"),
    ("left", "14:30:17", "14:30:20"),
    ("back-touch", "14:30:25", "14:30:27"),
]
SWEEP = ("14:29:30", "14:31:00")
NUM = r"(-?\d+(?:\.\d*)?)f?"


def firmware_constants():
    layout = open(os.path.join(FW, "driveModes", "lean_layout.h")).read()
    settings = open(os.path.join(FW, "config", "DriveSettings.cpp")).read()

    def scalar(text, name):
        m = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*{NUM};", text)
        if not m:
            raise SystemExit(f"replay_cop: can't find {name} in the firmware source")
        return float(m.group(1))

    def array(text, name):
        m = re.search(rf"constexpr\s+float\s+{name}\[\]\s*=\s*\{{([^}}]*)\}};", text)
        if not m:
            raise SystemExit(f"replay_cop: can't find {name}[] in the firmware source")
        return [scalar(text, v.strip()) if re.fullmatch(r"[A-Za-z_]\w*", v.strip()) else float(v.strip().rstrip("f"))
                for v in m.group(1).split(",")]

    block = re.search(r"CornerPositions\s*=\s*\{\{(.*?)\}\};", layout, re.S).group(1)
    positions = [(float(x), float(y)) for x, y in re.findall(rf"\{{{NUM},\s*{NUM}\}}", block)]
    assert len(positions) == 4, "expected four corner positions (F, B, L, R)"
    speed_knee = array(settings, "DefaultForewardBackKnee")
    turn_knee = array(settings, "DefaultLeftRightKnee")
    return {
        "positions": positions,  # F, B, L, R channels
        "empty": [scalar(layout, f"DefaultEmptyPlate{c}") for c in "FBLR"],
        "reach": {d: scalar(layout, f"DefaultReach{d.capitalize()}") for d in ("forward", "back", "left", "right")},
        "deadzone": scalar(layout, "DefaultDeadzone"),
        "min_total": scalar(layout, "MinCopTotal"),
        "speed_exp": math.log(speed_knee[1]) / math.log(speed_knee[0]),
        "turn_exp": math.log(turn_knee[1]) / math.log(turn_knee[0]),
        "mix": array(settings, "DefaultTurnMix"),
    }


# ---- The firmware math, one function per lean_layout.cpp / LeanDriveMode step ----

def centre_of_pressure(raw, c):  # cornerForces + centreOfPressure
    forces = [max(0, r - e) * 1.0 for r, e in zip(raw, c["empty"])]
    total = sum(forces)
    if total < c["min_total"]:
        return None
    return (sum(f * p[0] for f, p in zip(forces, c["positions"])) / total,
            sum(f * p[1] for f, p in zip(forces, c["positions"])) / total, total)


def axis_lean(offset, positive_reach, negative_reach, deadzone):
    reach = positive_reach if offset >= 0 else negative_reach
    beyond = abs(offset) - deadzone
    if beyond <= 0 or reach <= deadzone:
        return 0.0
    return math.copysign(min(beyond / (reach - deadzone), 1.0), offset)


def shape(speed, turn, c):  # shapeCommand with deadband 0
    speed = math.copysign(abs(speed) ** c["speed_exp"], speed)
    turn = math.copysign(abs(turn) ** c["turn_exp"], turn)
    if speed < -0.3:  # reverse-turn flip, tuned by feel on the chair
        turn = -turn
    if speed < -0.9:
        turn = 0.0
    return speed, turn


def mix(speed, turn, c):  # LeanDriveMode::getTargetsFromSample turn mixing
    m = c["mix"]
    mag = abs(speed)
    seg = 0 if mag < m[2] else 2
    gain = m[seg + 1] + (mag - m[seg]) / (m[seg + 2] - m[seg]) * (m[seg + 3] - m[seg + 1])
    clamp = lambda v: max(-1.0, min(1.0, v))
    return clamp(speed + gain * turn), clamp(speed - gain * turn)


def command(raw, neutral, c):
    cop = centre_of_pressure(raw, c)
    if cop is None:
        return None
    dx, dy = cop[0] - neutral[0], cop[1] - neutral[1]
    r = c["reach"]
    lean = (axis_lean(dy, r["forward"], r["back"], c["deadzone"]), axis_lean(dx, r["right"], r["left"], c["deadzone"]))
    speed, turn = shape(*lean, c)
    return cop, (dx, dy), lean, (speed, turn), mix(speed, turn, c)


# ---- Log handling ----

def secs(stamp):
    h, m, s = stamp.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def raw_samples(path):
    for row in csv.reader(open(path)):
        if len(row) >= 2 and row[1].startswith("DBG R "):
            parts = row[1].split()
            if len(parts) == 7:
                try:
                    yield secs(row[0]), [int(v) for v in parts[3:7]]
                except ValueError:
                    pass  # line damaged on the radio link


def window(samples, start, end):
    a, b = secs(start), secs(end)
    return [raw for t, raw in samples if a <= t <= b]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?", default=E9)
    ap.add_argument("--neutral", default="active-neutral", help="hold to calibrate on")
    ap.add_argument("--deadzone", type=float, help="override the firmware default, inches")
    ap.add_argument("--trace", nargs="?", const=0.5, type=float, help="also print the whole sweep in buckets")
    args = ap.parse_args()

    c = firmware_constants()
    if args.deadzone is not None:
        c["deadzone"] = args.deadzone
    samples = list(raw_samples(args.log))
    holds = {name: window(samples, a, b) for name, a, b in HOLDS}

    # Calibrate like the chair: average raw counts, rounded half up like
    # LeanDriveMode::calibrate's (sum + n/2) / n.
    cal = holds[args.neutral]
    floors = [(sum(r[i] for r in cal) + len(cal) // 2) // len(cal) for i in range(4)]
    neutral = centre_of_pressure(floors, c)
    print(f"neutral ({args.neutral}): raw F/B/L/R {floors} -> CoP x={neutral[0]:+.2f} y={neutral[1]:+.2f} in")
    r = c["reach"]
    print(f"reach fwd {r['forward']} back {r['back']} left {r['left']} right {r['right']} in, "
          f"deadzone {c['deadzone']} in, curves speed^{c['speed_exp']:.3f} turn^{c['turn_exp']:.3f}\n")

    head = f"{'hold':16} {'n':>3} {'x':>6} {'y':>6} {'dx':>6} {'dy':>6} {'lean sp':>8} {'lean tn':>8} {'speed':>7} {'turn':>7} {'left':>6} {'right':>6}"
    print(head)
    print("-" * len(head))
    for name, _, _ in HOLDS:
        out = [o for o in (command(raw, neutral, c) for raw in holds[name]) if o]
        mean = lambda f: st.mean(f(o) for o in out)
        print(f"{name:16} {len(out):3d} {mean(lambda o: o[0][0]):+6.2f} {mean(lambda o: o[0][1]):+6.2f} "
              f"{mean(lambda o: o[1][0]):+6.2f} {mean(lambda o: o[1][1]):+6.2f} "
              f"{mean(lambda o: o[2][0]):+8.3f} {mean(lambda o: o[2][1]):+8.3f} "
              f"{mean(lambda o: o[3][0]):+7.3f} {mean(lambda o: o[3][1]):+7.3f} "
              f"{mean(lambda o: o[4][0]):+6.3f} {mean(lambda o: o[4][1]):+6.3f}")

    # Sway inside the calibration hold: how far a still rider wanders from neutral.
    cal_out = [command(raw, neutral, c) for raw in cal]
    dxs = [abs(o[1][0]) for o in cal_out if o]
    dys = [abs(o[1][1]) for o in cal_out if o]
    moving = sum(1 for o in cal_out if o and (o[3][0] or o[3][1]))
    print(f"\n{args.neutral} sway from neutral: |dx| max {max(dxs):.2f} p95 {sorted(dxs)[int(.95 * len(dxs))]:.2f}, "
          f"|dy| max {max(dys):.2f} p95 {sorted(dys)[int(.95 * len(dys))]:.2f} in; "
          f"{moving}/{len(cal_out)} samples outside the deadzone")

    if args.trace:
        print(f"\nsweep {SWEEP[0]}-{SWEEP[1]}, {args.trace}s buckets: time, dx, dy, speed, turn")
        a, b = secs(SWEEP[0]), secs(SWEEP[1])
        buckets = {}
        for t, raw in samples:
            if a <= t <= b:
                o = command(raw, neutral, c)
                if o:
                    buckets.setdefault(int((t - a) // args.trace), []).append(o)
        for k in sorted(buckets):
            v = buckets[k]
            t = a + k * args.trace
            print(f"{int(t // 3600):02d}:{int(t % 3600 // 60):02d}:{t % 60:04.1f} "
                  f"{st.mean(o[1][0] for o in v):+6.2f} {st.mean(o[1][1] for o in v):+6.2f} "
                  f"{st.mean(o[3][0] for o in v):+7.3f} {st.mean(o[3][1] for o in v):+7.3f}")


if __name__ == "__main__":
    main()
