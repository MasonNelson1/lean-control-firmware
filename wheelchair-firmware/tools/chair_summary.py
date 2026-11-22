"""Compact summary of the live chair tap (logs/live/current.csv), instead of raw rows.

    python3 tools/chair_summary.py            # last 10 s
    python3 tools/chair_summary.py 60         # last 60 s
    python3 tools/chair_summary.py all        # whole tap session

Prints link health, mode and mode changes, per-signal min/mean/max, wheel
target vs actual, warnings, and the firmware's text messages, de-duplicated.
Runs under plain WSL python (it only reads the file chair_tap.py writes).
"""

import csv
import datetime as dt
import os
import sys
from collections import OrderedDict

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "live", "current.csv")
NUM = ["F", "B", "L", "R", "total", "userWeight", "speed", "turn", "leftTargetRPM", "rightTargetRPM",
       "leftSmoothedRPM", "rightSmoothedRPM", "leftActualRPM", "rightActualRPM",
       "leftBoostCurrent", "rightBoostCurrent"]


def secs(stamp):
    t = dt.datetime.strptime(stamp, "%H:%M:%S.%f")
    return t.hour * 3600 + t.minute * 60 + t.second + t.microsecond / 1e6


def num(x):
    try:
        v = float(x)
        return None if v != v else v  # drop nan
    except (TypeError, ValueError):
        return None


def stats(values):
    v = [x for x in values if x is not None]
    if not v:
        return "n/a"
    return f"{min(v):8.2f} {sum(v) / len(v):8.2f} {max(v):8.2f}"


def main():
    window = sys.argv[1] if len(sys.argv) > 1 else "10"
    with open(PATH, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        lines = [r for r in reader if r]
    if not lines:
        print("no data yet")
        return

    now_wall = dt.datetime.now()
    last_t = secs(lines[-1][0])
    age = (now_wall.hour * 3600 + now_wall.minute * 60 + now_wall.second + now_wall.microsecond / 1e6) - last_t
    if window != "all":
        lines = [r for r in lines if last_t - secs(r[0]) <= float(window)]

    rows, texts = [], OrderedDict()
    for r in lines:
        if len(r) == len(header):
            rows.append(dict(zip(header, r)))
        else:
            msg = ",".join(r[1:]).strip()
            first, count = texts.get(msg, (r[0], 0))
            texts[msg] = (first, count + 1)

    span = (secs(lines[-1][0]) - secs(lines[0][0])) or 1e-9
    print(f"window {lines[0][0]} -> {lines[-1][0]} ({span:.1f} s), {len(rows)} rows "
          f"({len(rows) / span:.1f} Hz), last line {age:.1f} s ago" + ("  ** STALE: tap or link down? **" if age > 2 else ""))
    if not rows:
        print("no telemetry rows in window")
    else:
        modes, changes, prev = [], [], None
        for r in rows:
            if r["mode"] != prev:
                changes.append(f"{r['host_time']} {r['mode']}")
                prev = r["mode"]
        print("mode now:", rows[-1]["mode"], "| changes:", ", ".join(changes) if len(changes) > 1 else "none")
        print(f"{'signal':18} {'min':>8} {'mean':>8} {'max':>8}")
        cols = {k: [num(r[k]) for r in rows] for k in NUM}
        for k in NUM:
            print(f"{k:18} {stats(cols[k])}")
        stopped = sum(r["stopped"] == "1" for r in rows)
        print(f"stopped flag: {stopped}/{len(rows)} rows")

        warn = []
        for side in ("left", "right"):
            tgt = [abs(x) for x in cols[side + "TargetRPM"] if x is not None]
            act = [abs(x) for x in cols[side + "ActualRPM"] if x is not None]
            if tgt and max(tgt) > 5 and (not act or max(act) < 0.5):
                warn.append(f"{side} wheel commanded up to {max(tgt):.0f} RPM but reports ~0: VESC not reporting or not spinning")
        for k in ("F", "B", "L", "R"):
            v = [x for x in cols[k] if x is not None]
            if v and rows[-1]["mode"] != "DISARM" and max(v) - min(v) == 0:
                warn.append(f"sensor {k} perfectly flat while not disarmed")
        for w in warn:
            print("WARN:", w)

    if texts:
        print("firmware messages (first seen, count):")
        for msg, (first, count) in texts.items():
            print(f"  {first} x{count:<4} {msg}")


if __name__ == "__main__":
    main()
