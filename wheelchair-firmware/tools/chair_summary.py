"""Compact summary of the live chair tap (logs/live/current.csv), instead of raw rows.

    python3 tools/chair_summary.py            # last 10 s
    python3 tools/chair_summary.py 60         # last 60 s
    python3 tools/chair_summary.py all        # whole tap session
    CHAIR_TAP=other.csv python3 tools/chair_summary.py   # another log

Prints link health, mode and mode changes, the sensor layout, per-signal
min/mean/max, the centre of pressure (corner layout), wheel target vs actual,
VESC health (temperatures, voltage, currents and peaks, faults, status-frame
ages, GET_VALUES poll), the Teensy (loop time, Bluetooth drops), events,
warnings, and the firmware's text messages, de-duplicated (the 20 Hz DBG R,
DBG COP and DBG M lines and the 1 Hz health lines are summarised, not listed).
Runs under plain WSL python (it only reads the file chair_tap.py writes).
"""

import csv
import datetime as dt
import os
import sys
from collections import OrderedDict
import re

# Summarised below instead of listed with the firmware messages.
SUMMARISED = ("DBG R ", "DBG COP ", "DBG M ", "DBG VESC ", "DBG VVAL ", "DBG SYS ", "DBG IO ", "DBG VCONF ",
              "DBG VESCPROBE ", "DBG EVT ")
STATUS_NAMES = ("STATUS_1", "STATUS_2", "STATUS_3", "STATUS_4", "STATUS_5")


def kv(msg):
    """key=value pairs of a DBG line; None if garbled on the radio (a key repeats because two
    lines ran together, or a value is empty because the tail was lost)."""
    pairs = [t.split("=", 1) for t in msg.split()[2:] if "=" in t]
    d = dict(pairs)
    return d if len(d) == len(pairs) and all(d.values()) else None


def peak(values):
    """The signed value with the largest magnitude."""
    v = [x for x in values if x is not None]
    return max(v, key=abs) if v else None


def fmt(v, spec=".1f", none="—"):
    return none if v is None else format(v, spec)


def vesc_health(vesc, vval, faults, motor):
    """VESC section: per controller id, latest + window extremes. Returns warnings."""
    warn = []
    ids = sorted({int(num(d.get("id")) or 0) for d in vesc + vval if num(d.get("id")) is not None})
    if not ids and not faults:
        return warn
    print("VESC health (latest; peak = largest magnitude in the window):")
    for vid in ids:
        st = [d for d in vesc if num(d.get("id")) == vid]
        va = [d for d in vval if num(d.get("id")) == vid and d.get("age") != "-1"]
        last = st[-1] if st else {}
        silent = not st or num(last.get("frames")) == 0  # older firmware printed age=0 when silent
        age = None if silent else num(last.get("age"))
        fets = [x for x in (num(d.get("fetC")) for d in st if num(d.get("frames"))) if x is not None]
        vins = [x for x in (num(d.get("vin")) for d in st if num(d.get("frames"))) if x]  # 0 = no STATUS_5
        print(f"  id {vid}: status age {fmt(age, '.0f')} ms, FET {last.get('fetC', '—')} C (max {fmt(max(fets, default=None))}), "
              f"motor {last.get('motC', '—')} C, vin {last.get('vin', '—')} V (min {fmt(min(vins, default=None))})")
        ipk = peak([num(d.get("ipk", d.get("current"))) for d in st])
        iinpk = peak([num(d.get("iinpk", d.get("iin"))) for d in st])
        print(f"        motor current peak {fmt(ipk)} A, input current peak {fmt(iinpk)} A, "
              f"Ah {last.get('ah', '—')}, Wh {last.get('wh', '—')}")
        ages = last.get("ages")
        if ages:
            missing = [STATUS_NAMES[i] for i, a in enumerate(ages.split("/")) if a == "-1"]
            stale = [STATUS_NAMES[i] for i, a in enumerate(ages.split("/")) if a not in ("-1",) and (num(a) or 0) > 1000]
            if missing:
                print(f"        never received: {', '.join(missing)} (not enabled in the VESC's CAN status settings)")
            if stale:
                warn.append(f"VESC {vid}: {', '.join(stale)} older than 1 s")
        if age is None or age < 0:
            warn.append(f"VESC {vid}: no CAN status frames (powered off, CAN unplugged, or wrong id)")
        elif age > 500:
            warn.append(f"VESC {vid}: last status frame {age:.0f} ms ago (silent?)")
        if va:
            v = va[-1]
            n = v.get("n", "0/0/0").split("/")
            print(f"        GET_VALUES: fault now {v.get('fault')}, episodes since boot {v.get('faults')}, "
                  f"polls asked/replied/lost {'/'.join(n)}, slowest reply {v.get('lat')} ms")
            if v.get("fault") not in (None, "0"):
                warn.append(f"VESC {vid}: fault code {v.get('fault')} active")
        elif vval:
            warn.append(f"VESC {vid}: no GET_VALUES reply in the window")
    if motor:
        for side, i in (("left", 0), ("right", 1)):
            cur = peak([m[1 + i] for m in motor])
            duty = peak([m[3 + i] for m in motor])
            print(f"  {side} 20 Hz: motor current peak {fmt(cur)} A, duty peak {fmt(duty, '.3f')}")
    for host, msg in faults:
        d = kv(msg) or {}
        print(f"  FAULT {host} id={d.get('id')} {d.get('name')} (code {d.get('code')}): motor {d.get('imot')} A, "
              f"input {d.get('iin')} A, erpm {d.get('erpm')}, duty {d.get('duty')}, vin {d.get('vin')} V")
        if d.get("code") not in (None, "0"):
            warn.append(f"VESC {d.get('id')} faulted: {d.get('name')} at {host}")
    return warn


def teensy_health(sys_lines, events):
    warn = []
    if sys_lines:
        first, last = sys_lines[0], sys_lines[-1]
        drop = lambda d: [int(num(x) or 0) for x in d.get("drop", "0/0").split("/")]
        dropped = [a - b for a, b in zip(drop(last), drop(first))]
        sent = [int(num(x) or 0) for x in last.get("bt", "0/0").split("/")]
        sent0 = [int(num(x) or 0) for x in first.get("bt", "0/0").split("/")]
        span = max(1, (num(last.get("up")) or 0) - (num(first.get("up")) or 0))
        loop_max = max(num(d.get("loopMax")) or 0 for d in sys_lines)
        ctrl = [d.get("ctrl", "0/0").split("/") for d in sys_lines]
        ctrl_max = max(num(c[1]) or 0 for c in ctrl if len(c) == 2)
        bt_free = min(num(d.get("btFree")) or 0 for d in sys_lines)
        print(f"Teensy: up {last.get('up')} s, CPU {last.get('cpuC')} C, loop max {loop_max:.0f} us, "
              f"control tick max {ctrl_max:.0f} ms, CAN rx {last.get('canRx')} tx-fail {last.get('canTxFail')} "
              f"err {last.get('tec')}/{last.get('rec')}")
        print(f"Bluetooth: {(sent[0] - sent0[0]) / span:.0f} B/s sent, dropped {dropped[1]} lines "
              f"({dropped[0]} B) in the window, least TX space {bt_free:.0f} B")
        if dropped[1]:
            warn.append(f"Bluetooth TX buffer overflowed: {dropped[1]} lines dropped by the Teensy")
        if num(last.get("canTxFail")) and num(first.get("canTxFail")) != num(last.get("canTxFail")):
            warn.append("CAN TX queue full: motor command frames refused")
    if events:
        print("events:")
        for host, msg in events[-15:]:
            print(f"  {host} {msg[len('DBG EVT '):]}")
        if len(events) > 15:
            print(f"  ... {len(events) - 15} earlier")
    return warn

# Corner layout: which corner each channel sits at (lean::CornerPositions).
CORNER_NAMES = {"F": " back-left", "B": " back-right", "L": " front-left", "R": " front-right"}
PATH = os.environ.get("CHAIR_TAP") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "live", "current.csv")
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

    rows, texts, cops, layout = [], OrderedDict(), [], None
    vesc, vval, faults, motor, sys_lines, events = [], [], [], [], [], []
    for r in lines:
        if len(r) == len(header):
            rows.append(dict(zip(header, r)))
            continue
        msg = ",".join(r[1:]).strip()
        for prefix, into in (("DBG VESC ", vesc), ("DBG VVAL ", vval), ("DBG SYS ", sys_lines)):
            if msg.startswith(prefix):
                d = kv(msg)
                if d is not None:
                    into.append(d)
        if msg.startswith("DBG VFAULT "):
            faults.append((r[0], msg))
        if msg.startswith("DBG EVT "):
            events.append((r[0], msg))
        if msg.startswith("DBG M "):
            parts = msg.split()
            if len(parts) == 18:
                motor.append([num(v) for v in parts[2:]])
        if msg.startswith("DBG COP "):
            parts = msg.split()
            if len(parts) == 8:
                cops.append([num(v) for v in parts[3:8]])
        if msg.startswith(SUMMARISED):
            continue
        m = re.search(r" layout=(\w+)", msg) if msg.startswith("DBG SENSORS ") else None
        if m:
            layout = m.group(1)
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
        print("sensor layout:", layout or "not reported (firmware before 2026-11-22: plus)")
        names = CORNER_NAMES if layout == "CORNER" else {}
        print(f"{'signal':18} {'min':>8} {'mean':>8} {'max':>8}")
        cols = {k: [num(r[k]) for r in rows] for k in NUM}
        for k in NUM:
            print(f"{(k + names.get(k, '')):18} {stats(cols[k])}")
        if cops:
            print(f"centre of pressure, inches ({len(cops)} samples):")
            for i, name in enumerate(("x", "y", "dx from neutral", "dy from neutral", "total load")):
                print(f"  {name:16} {stats([c[i] for c in cops])}")
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
        warn += vesc_health(vesc, vval, faults, motor)
        warn += teensy_health(sys_lines, events)
        for w in warn:
            print("WARN:", w)

    if texts:
        print("firmware messages (first seen, count):")
        for msg, (first, count) in texts.items():
            print(f"  {first} x{count:<4} {msg}")


if __name__ == "__main__":
    main()
