"""Replay a tap log through the firmware's corner-layout lean math, offline.

usage: replay_cop.py [tap.csv] [--hold NAME=HH:MM:SS-HH:MM:SS ...] [--neutral HOLD]
                     [--passive HOLD] [--deadzone IN] [--trace [BUCKET_S]] [--jumps]

Defaults to the E9 rectangle sweep (2026-11-22) and its hold windows; pass
--hold (repeatable) for any other log. Calibrates neutral the way the chair
does (average raw counts during the neutral hold, rounded like
LeanDriveMode::calibrate), optionally records a relaxed posture from --passive
(the second calibrate press), then runs every 20 Hz `DBG R` sample through the
corner math and prints the mean output for each hold:

  x, y      centre of pressure, inches (plate coordinates: x right, y forward)
  dx, dy    distance beyond the neutral zone (with no relaxed posture: from neutral)
  lean      speed/turn from -1 to 1 before the response curves (cornerCommand)
  speed,turn after the response curves and reverse-turn flip (shapeCommand)
  left,right wheel commands after turn mixing, as fractions of the speed knob

--jumps prints the largest CoP change between consecutive samples and how far
the CoP jumps when each channel drops out (backs lean::MaxCopStep).

Constants are read from the firmware source, never copied: sensor positions,
empty-plate baselines, reach, deadzone and relaxed margin from
src/driveModes/lean_layout.h;
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

# Hold windows called out during the E9 sweep (laptop time); the default --hold set.
E9_HOLDS = [
    ("passive-neutral", "14:29:31", "14:29:38"),
    ("active-neutral", "14:29:41", "14:29:46"),
    ("forward", "14:29:59", "14:30:03"),
    ("right", "14:30:09", "14:30:12"),
    ("left", "14:30:17", "14:30:20"),
    ("back-touch", "14:30:25", "14:30:27"),
]
E9_SWEEP = ("14:29:30", "14:31:00")
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
        "margin": scalar(layout, "DefaultPassiveMargin"),
        "min_total": scalar(layout, "MinCopTotal"),
        "speed_exp": math.log(speed_knee[1]) / math.log(speed_knee[0]),
        "turn_exp": math.log(turn_knee[1]) / math.log(turn_knee[0]),
        # council-corner and later: no turn mix (turn is a rate in DriveController);
        # left/right are then plain speed ± turn.
        "mix": array(settings, "DefaultTurnMix") if "DefaultTurnMix" in settings else None,
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
    clamp = lambda v: max(-1.0, min(1.0, v))
    if m is None:  # firmware without the turn-mix curve
        return clamp(speed + turn), clamp(speed - turn)
    mag = abs(speed)
    seg = 0 if mag < m[2] else 2
    gain = m[seg + 1] + (mag - m[seg]) / (m[seg + 2] - m[seg]) * (m[seg + 3] - m[seg + 1])
    clamp = lambda v: max(-1.0, min(1.0, v))
    return clamp(speed + gain * turn), clamp(speed - gain * turn)


def neutral_zone(active, passive, c):  # lean::neutralZone
    if passive is None:
        return active[0], active[1], active[1], c["deadzone"], c["deadzone"]
    return active[0], min(active[1], passive[1]), max(active[1], passive[1]), c["deadzone"], c["margin"]


def command(raw, zone, c):  # cornerCommand + shapeCommand + mix
    cop = centre_of_pressure(raw, c)
    if cop is None:
        return None
    x0, y_back, y_fwd, x_dead, y_dead = zone
    dx = cop[0] - x0
    dy = cop[1] - y_fwd if cop[1] > y_fwd else cop[1] - y_back if cop[1] < y_back else 0.0
    r = c["reach"]
    lean = (axis_lean(dy, r["forward"], r["back"], y_dead), axis_lean(dx, r["right"], r["left"], x_dead))
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


def hold_arg(text):
    try:
        name, span = text.split("=", 1)
        start, end = span.split("-")
        secs(start), secs(end)
        return name, start, end
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected NAME=HH:MM:SS-HH:MM:SS, got {text!r}")


def average(window_raw):  # LeanDriveMode averageRaw: (sum + n/2) / n
    return [(sum(r[i] for r in window_raw) + len(window_raw) // 2) // len(window_raw) for i in range(4)]


def jumps(samples, start, end, c):
    a, b = secs(start), secs(end)
    run = [(t, raw) for t, raw in samples if a <= t <= b]
    steps = []
    for (t0, r0), (t1, r1) in zip(run, run[1:]):
        c0, c1 = centre_of_pressure(r0, c), centre_of_pressure(r1, c)
        if c0 and c1 and t1 - t0 < 0.1:
            steps.append((math.hypot(c1[0] - c0[0], c1[1] - c0[1]), t1 - t0))
    steps.sort(reverse=True)
    print(f"\nlargest CoP change between consecutive samples, {start}-{end}:")
    print("  " + ", ".join(f'{d:.2f}" in {dt * 1000:.0f} ms' for d, dt in steps[:5]))
    print(f"  peak rate {max(d / dt for d, dt in steps if dt > 0.02):.1f} in/s")
    print("CoP jump if a channel drops to 372 (amp offset) at each sample (min / 5th pct / median, inches):")
    for i, name in enumerate("FBLR"):
        moves = []
        for _, raw in run:
            before = centre_of_pressure(raw, c)
            dead = list(raw)
            dead[i] = 372
            after = centre_of_pressure(dead, c)
            if before and after:
                moves.append(math.hypot(after[0] - before[0], after[1] - before[1]))
        moves.sort()
        print(f"  {name}: {moves[0]:.2f} / {moves[len(moves) // 20]:.2f} / {moves[len(moves) // 2]:.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?", default=E9)
    ap.add_argument("--hold", action="append", type=hold_arg, metavar="NAME=START-END",
                    help="a held posture's window (laptop time); repeat. Default: the E9 holds")
    ap.add_argument("--neutral", default="active-neutral", help="hold to calibrate neutral on")
    ap.add_argument("--passive", help="hold to record as the relaxed posture (second press)")
    ap.add_argument("--deadzone", type=float, help="override the firmware default, inches")
    ap.add_argument("--sweep", type=hold_arg, metavar="NAME=START-END",
                    help="span for --trace and --jumps. Default: the E9 sweep")
    ap.add_argument("--trace", nargs="?", const=0.5, type=float, help="also print the whole sweep in buckets")
    ap.add_argument("--jumps", action="store_true", help="tick-to-tick CoP change and dead-channel jumps")
    args = ap.parse_args()

    c = firmware_constants()
    if args.deadzone is not None:
        c["deadzone"] = args.deadzone
    hold_list = args.hold or E9_HOLDS
    sweep = (args.sweep[1], args.sweep[2]) if args.sweep else E9_SWEEP
    samples = list(raw_samples(args.log))
    holds = {name: window(samples, a, b) for name, a, b in hold_list}
    for name in filter(None, (args.neutral, args.passive)):
        if not holds.get(name):
            raise SystemExit(f"replay_cop: no samples for hold {name!r}; holds are {', '.join(holds)}")

    # Calibrate like the chair: average raw counts, rounded half up like
    # LeanDriveMode::calibrate's (sum + n/2) / n.
    floors = average(holds[args.neutral])
    neutral = centre_of_pressure(floors, c)
    print(f"neutral ({args.neutral}): raw F/B/L/R {floors} -> CoP x={neutral[0]:+.2f} y={neutral[1]:+.2f} in")
    passive = None
    if args.passive:
        passive = centre_of_pressure(average(holds[args.passive]), c)
        print(f"relaxed ({args.passive}): CoP x={passive[0]:+.2f} y={passive[1]:+.2f} in")
    zone = neutral_zone(neutral, passive, c)
    r = c["reach"]
    print(f"reach fwd {r['forward']} back {r['back']} left {r['left']} right {r['right']} in; "
          f"no speed for y {zone[1] - zone[4]:+.2f}..{zone[2] + zone[4]:+.2f}, no turn for x {zone[0] - zone[3]:+.2f}..{zone[0] + zone[3]:+.2f}; "
          f"curves speed^{c['speed_exp']:.3f} turn^{c['turn_exp']:.3f}\n")

    head = f"{'hold':16} {'n':>3} {'x':>6} {'y':>6} {'dx':>6} {'dy':>6} {'lean sp':>8} {'lean tn':>8} {'speed':>7} {'turn':>7} {'left':>6} {'right':>6}"
    print(head)
    print("-" * len(head))
    for name, _, _ in hold_list:
        out = [o for o in (command(raw, zone, c) for raw in holds[name]) if o]
        if not out:
            print(f"{name:16} no samples")
            continue
        mean = lambda f: st.mean(f(o) for o in out)
        print(f"{name:16} {len(out):3d} {mean(lambda o: o[0][0]):+6.2f} {mean(lambda o: o[0][1]):+6.2f} "
              f"{mean(lambda o: o[1][0]):+6.2f} {mean(lambda o: o[1][1]):+6.2f} "
              f"{mean(lambda o: o[2][0]):+8.3f} {mean(lambda o: o[2][1]):+8.3f} "
              f"{mean(lambda o: o[3][0]):+7.3f} {mean(lambda o: o[3][1]):+7.3f} "
              f"{mean(lambda o: o[4][0]):+6.3f} {mean(lambda o: o[4][1]):+6.3f}")

    # Sway inside the calibration hold: how far a still rider wanders from neutral.
    cal_out = [command(raw, zone, c) for raw in holds[args.neutral]]
    dxs = [abs(o[1][0]) for o in cal_out if o]
    dys = [abs(o[1][1]) for o in cal_out if o]
    moving = sum(1 for o in cal_out if o and (o[3][0] or o[3][1]))
    print(f"\n{args.neutral} sway from neutral: |dx| max {max(dxs):.2f} p95 {sorted(dxs)[int(.95 * len(dxs))]:.2f}, "
          f"|dy| max {max(dys):.2f} p95 {sorted(dys)[int(.95 * len(dys))]:.2f} in; "
          f"{moving}/{len(cal_out)} samples outside the deadzone")

    if args.jumps:
        jumps(samples, *sweep, c)

    if args.trace:
        print(f"\nsweep {sweep[0]}-{sweep[1]}, {args.trace}s buckets: time, dx, dy, speed, turn")
        a, b = secs(sweep[0]), secs(sweep[1])
        buckets = {}
        for t, raw in samples:
            if a <= t <= b:
                o = command(raw, zone, c)
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
