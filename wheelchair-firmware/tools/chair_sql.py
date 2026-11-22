"""Ask questions of a chair telemetry log in SQL instead of reading raw rows.

    python3 tools/chair_sql.py "SELECT ..."                 # live tap (logs/live/current.csv)
    python3 tools/chair_sql.py --file logs/live/X.csv "SELECT ..."
    python3 tools/chair_sql.py --schema                     # tables and columns
    python3 tools/chair_sql.py --trend 5                    # 5-second buckets of the main signals

Tables (SQLite, in memory, rebuilt on every call):
  t      one row per telemetry sample: s (seconds since log start), host_time,
         time_ms, F, B, L, R, total, userWeight, speed, turn, *TargetRPM,
         (F/B/L/R are channels. PLUS layout: front/back/left/right, after floors and gains.
          CORNER layout: back-left/back-right/front-left/front-right force above the empty plate.)
         *SmoothedRPM, *ActualRPM, *BoostCurrent, stopped, mode
  msg    firmware text lines: s, host_time, kind (first word; CORRUPT = row damaged on the radio link), text
  sens   parsed 'DBG SENSORS' lines: s, rawF, rawB, rawL, rawR, floorF..floorR,
         W, seatLoad, seated, calibrated, calActive, railed, layout (PLUS/CORNER)
  corner 1 Hz 'DBG CORNER' lines (corner layout), inches: neutralX/Y, passiveX/Y (relaxed
         posture), deadL/R/B/F (box with no output), fullL/R/B/F (full-lean box), reachCal,
         emptyF/B/L/R (baseline in use), autoZero (1 = set by the empty-seat zero). NULL = nan
  raw    20 Hz 'DBG R' lines: s, host_time, time_ms, F, B, L, R (raw ADC counts; zero force ≈ 372)
  cop    20 Hz 'DBG COP' lines (corner layout): s, host_time, time_ms, x, y (centre of pressure,
         inches, x right / y forward), dx, dy (from the calibrated neutral; NULL before), total
  vesc   1 Hz 'DBG VESC' per VESC, from the CAN status frames: s, id, age (ms since any status
         frame, -1 never), frames, erpm, current, duty, fetC, vin, motC, iin (input current),
         ipk/iinpk (largest motor/input current since the previous line, signed), ah/ahc, wh/whc
         (used/charged since VESC boot), tach, pid, age1..age5 (ms since STATUS_1..5, -1 never), side (L/R).
         Lines from older firmware fill only id..vin.
  vval   1 Hz 'DBG VVAL' per VESC, from COMM_GET_VALUES: id, age (ms since the reply, -1 none),
         fault (code now), fetC, motC, imot/iin/curD/curQ (means since the previous poll), duty, erpm,
         vin, ah, ahc, wh, whc, tach, tachAbs, pid, mos1..3, voltD, voltQ, st (bit0 timeout, bit1 kill),
         faults (episodes since boot), lastFault, asked, replies, lost, lat (slowest reply, ms), side
  vfault 'DBG VFAULT', one per fault change: id, code, name (NONE = cleared), time_ms, imot, iin,
         erpm, duty, vin, fetC, n (episodes on that VESC since boot), side
  motor  20 Hz 'DBG M': time_ms, curL/R (motor A), dutyL/R, iinL/R (input A), pidL/R (smoother RPM
         before the clamp to measured ± MAX_RPM_COMMAND_DIFF), accL/R, rateL/R (RPM/s), intL/R,
         lim (slewed speed cap, m/s)
  sys    1 Hz 'DBG SYS': up (s), loops, loopMax (us), ctrlMin/ctrlMax (control tick spacing, ms),
         cpuC, canRx, canTxFail, tec, rec, btBytes, btLines, dropBytes, dropLines (Bluetooth,
         since boot), btFree (least TX space this second), cfg (config reply bytes)
  io     1 Hz 'DBG IO': spdPos, spdRaw, spdState, profPos, profRaw, profState (S settled,
         F fault, - settling), btnLean, btnRemote, btnDisarm, btnCal (1 = held), pend, act
         (profile slots, 0 none), sel, cur (modes), seated
  evt    'DBG EVT' events: time_ms, kind (btn, select, mode, rail-disarm, cop-jump, cal-press,
         cal-end, profile, settings), detail
Output is capped at 80 rows; aggregate with GROUP BY CAST(s/N AS INT) for trends.
"""

import argparse
import csv
import datetime as dt
import os
import re
import sqlite3

LIVE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "live", "current.csv")
SENS = re.compile(r"DBG SENSORS raw=(-?\d+)/(-?\d+)/(-?\d+)/(-?\d+) floors=(-?\d+)/(-?\d+)/(-?\d+)/(-?\d+) "
                  r"W=(-?\d+) seatLoad=(-?\d+) seated=(\d) calibrated=(\d) calActive=(\d)(?: railed=(\d))?"
                  r"(?: layout=(\w+))?")
CORNER = re.compile(r"^DBG CORNER neutral=(\S+)/(\S+) passive=(\S+)/(\S+) dead=(\S+)/(\S+)/(\S+)/(\S+) "
                    r"full=(\S+)/(\S+)/(\S+)/(\S+) reachCal=(\d) empty=(\d+)/(\d+)/(\d+)/(\d+) auto=(\d)$")
RAWR = re.compile(r"^DBG R (\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)$")
COP = re.compile(r"^DBG COP (\d+) (\S+) (\S+) (\S+) (\S+) (\S+)$")
MOTOR = re.compile(r"^DBG M (\d+)" + r" (\S+)" * 15 + "$")
EVT = re.compile(r"^DBG EVT (\d+) (\S+) ?(.*)$")


def slash(n):
    return lambda v: v.split("/") if len(v.split("/")) == n else None


def buttons(v):
    return list(v) if len(v) == 4 and set(v) <= {"0", "1"} else None


# key=value lines: table, then (key, columns, splitter); the first `need` keys must be present
# (older firmware sends fewer), the rest are NULL when missing.
KV = {
    "DBG VESC ": ("vesc", 8, [
        ("id", ["id"]), ("age", ["age"]), ("frames", ["frames"]), ("erpm", ["erpm"]), ("current", ["current"]),
        ("duty", ["duty"]), ("fetC", ["fetC"]), ("vin", ["vin"]), ("motC", ["motC"]), ("iin", ["iin"]),
        ("ipk", ["ipk"]), ("iinpk", ["iinpk"]), ("ah", ["ah", "ahc"], slash(2)), ("wh", ["wh", "whc"], slash(2)),
        ("tach", ["tach"]), ("pid", ["pid"]), ("ages", ["age1", "age2", "age3", "age4", "age5"], slash(5)),
        ("side", ["side"])]),
    "DBG VVAL ": ("vval", 2, [
        ("id", ["id"]), ("age", ["age"]), ("fault", ["fault"]), ("fetC", ["fetC"]), ("motC", ["motC"]),
        ("imot", ["imot"]), ("iin", ["iin"]), ("iD", ["curD"]), ("iQ", ["curQ"]), ("duty", ["duty"]), ("erpm", ["erpm"]),
        ("vin", ["vin"]), ("ah", ["ah", "ahc"], slash(2)), ("wh", ["wh", "whc"], slash(2)),
        ("tach", ["tach", "tachAbs"], slash(2)), ("pid", ["pid"]), ("mos", ["mos1", "mos2", "mos3"], slash(3)),
        ("vD", ["voltD"]), ("vQ", ["voltQ"]), ("st", ["st"]), ("faults", ["faults"]), ("last", ["lastFault"]),
        ("n", ["asked", "replies", "lost"], slash(3)), ("lat", ["lat"]), ("side", ["side"])]),
    "DBG VFAULT ": ("vfault", 3, [
        ("id", ["id"]), ("code", ["code"]), ("name", ["name"]), ("ms", ["time_ms"]), ("imot", ["imot"]),
        ("iin", ["iin"]), ("erpm", ["erpm"]), ("duty", ["duty"]), ("vin", ["vin"]), ("fetC", ["fetC"]), ("n", ["n"]),
        ("side", ["side"])]),
    "DBG SYS ": ("sys", 16, [
        ("up", ["up"]), ("loops", ["loops"]), ("loopMax", ["loopMax"]), ("ctrl", ["ctrlMin", "ctrlMax"], slash(2)),
        ("cpuC", ["cpuC"]), ("canRx", ["canRx"]), ("canTxFail", ["canTxFail"]), ("tec", ["tec"]), ("rec", ["rec"]),
        ("bt", ["btBytes", "btLines"], slash(2)), ("drop", ["dropBytes", "dropLines"], slash(2)),
        ("btFree", ["btFree"]), ("cfg", ["cfg"])]),
    "DBG IO ": ("io", 10, [
        ("spd", ["spdPos", "spdRaw", "spdState"], slash(3)), ("prof", ["profPos", "profRaw", "profState"], slash(3)),
        ("btn", ["btnLean", "btnRemote", "btnDisarm", "btnCal"], buttons), ("pend", ["pend"]), ("act", ["act"]),
        ("sel", ["sel"]), ("cur", ["cur"]), ("seated", ["seated"])]),
}


def kv_row(text, need, spec):
    """Values for a key=value line, or None when it is garbled (a required key missing,
    a value with the wrong shape, or a key that appears twice: two lines run together)."""
    pairs = [t.split("=", 1) for t in text.split()[2:] if "=" in t]
    tokens = dict(pairs)
    if len(tokens) != len(pairs):
        return None
    row = []
    for i, (key, cols, *split) in enumerate(spec):
        if key not in tokens:
            if i < need:
                return None
            row += [None] * len(cols)
            continue
        parts = split[0](tokens[key]) if split else [tokens[key]]
        if parts is None or "" in parts:
            return None
        row += [num(p) for p in parts]
    return row


def secs(stamp):
    t = dt.datetime.strptime(stamp, "%H:%M:%S.%f")
    return t.hour * 3600 + t.minute * 60 + t.second + t.microsecond / 1e6


def num(x):
    try:
        v = float(x)
        return None if v != v else v
    except ValueError:
        return x


def load(path):
    db = sqlite3.connect(":memory:")
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        cols = ["s"] + header
        db.execute(f"CREATE TABLE t ({', '.join(cols)})")
        db.execute("CREATE TABLE msg (s, host_time, kind, text)")
        db.execute("CREATE TABLE sens (s, host_time, rawF, rawB, rawL, rawR, floorF, floorB, floorL, floorR, "
                   "W, seatLoad, seated, calibrated, calActive, railed, layout)")
        db.execute("CREATE TABLE corner (s, host_time, neutralX, neutralY, passiveX, passiveY, deadL, deadR, deadB, deadF, "
                   "fullL, fullR, fullB, fullF, reachCal, emptyF, emptyB, emptyL, emptyR, autoZero)")
        db.execute("CREATE TABLE raw (s, host_time, time_ms, F, B, L, R)")
        db.execute("CREATE TABLE cop (s, host_time, time_ms, x, y, dx, dy, total)")
        db.execute("CREATE TABLE motor (s, host_time, time_ms, curL, curR, dutyL, dutyR, iinL, iinR, pidL, pidR, "
                   "accL, accR, rateL, rateR, intL, intR, lim)")
        db.execute("CREATE TABLE evt (s, host_time, time_ms, kind, detail)")
        for table, _, spec in KV.values():
            names = [c for item in spec for c in item[1]]
            db.execute(f"CREATE TABLE {table} (s, host_time, {', '.join(names)})")
        t0 = None
        for r in reader:
            if not r:
                continue
            s = secs(r[0])
            t0 = s if t0 is None else t0
            s = round(s - t0 if s >= t0 else s + 86400 - t0, 3)
            if len(r) == len(header):
                db.execute(f"INSERT INTO t VALUES ({','.join('?' * len(cols))})",
                           [s, r[0]] + [num(x) for x in r[1:]])
                continue
            text = ",".join(r[1:]).strip()
            m = RAWR.match(text)
            if m:
                db.execute("INSERT INTO raw VALUES (?,?,?,?,?,?,?)", [s, r[0]] + [int(g) for g in m.groups()])
                continue
            m = COP.match(text)
            if m:
                db.execute("INSERT INTO cop VALUES (?,?,?,?,?,?,?,?)", [s, r[0]] + [num(g) for g in m.groups()])
                continue
            m = CORNER.match(text)
            if m:
                db.execute(f"INSERT INTO corner VALUES ({','.join('?' * 20)})", [s, r[0]] + [num(g) for g in m.groups()])
                continue
            m = MOTOR.match(text)
            if m:
                db.execute(f"INSERT INTO motor VALUES ({','.join('?' * 18)})", [s, r[0]] + [num(g) for g in m.groups()])
                continue
            db.execute("INSERT INTO msg VALUES (?,?,?,?)", (s, r[0], "CORRUPT" if text[:1].isdigit() else text.split(" ")[0], text))
            m = SENS.search(text)
            if m:
                g = m.groups()
                ints = [None if v is None else int(v) for v in g[:14]]
                db.execute(f"INSERT INTO sens VALUES ({','.join('?' * 17)})", [s, r[0]] + ints + [g[14]])
            m = EVT.match(text)
            if m:
                db.execute("INSERT INTO evt VALUES (?,?,?,?,?)", [s, r[0], int(m[1]), m[2], m[3]])
            for prefix, (table, need, spec) in KV.items():
                if text.startswith(prefix):
                    row = kv_row(text, need, spec)
                    if row is not None:
                        db.execute(f"INSERT INTO {table} VALUES ({','.join('?' * (len(row) + 2))})", [s, r[0]] + row)
                    break
    return db


def show(cur, limit=80):
    names = [d[0] for d in cur.description]
    rows = cur.fetchmany(limit + 1)
    print(" | ".join(names))
    for row in rows[:limit]:
        print(" | ".join("" if v is None else (f"{v:.4g}" if isinstance(v, float) else str(v)) for v in row))
    if len(rows) > limit:
        print(f"... more rows; aggregate or add LIMIT")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sql", nargs="?")
    ap.add_argument("--file", default=LIVE)
    ap.add_argument("--schema", action="store_true")
    ap.add_argument("--trend", type=float)
    a = ap.parse_args()
    db = load(a.file)
    if a.schema:
        for (name, sql) in db.execute("SELECT name, sql FROM sqlite_master"):
            n = db.execute(f"SELECT COUNT(*) FROM {name}").fetchone()[0]
            print(f"{sql}  -- {n} rows")
        return
    if a.trend:
        n = a.trend
        show(db.execute(TREND.format(n=n)))
        return
    show(db.execute(a.sql))


if __name__ == "__main__":
    main()
