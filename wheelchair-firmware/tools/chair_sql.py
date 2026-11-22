"""Ask questions of a chair telemetry log in SQL instead of reading raw rows.

    python3 tools/chair_sql.py "SELECT ..."                 # live tap (logs/live/current.csv)
    python3 tools/chair_sql.py --file logs/live/X.csv "SELECT ..."
    python3 tools/chair_sql.py --schema                     # tables and columns
    python3 tools/chair_sql.py --trend 5                    # 5-second buckets of the main signals

Tables (SQLite, in memory, rebuilt on every call):
  t      one row per telemetry sample: s (seconds since log start), host_time,
         time_ms, F, B, L, R, total, userWeight, speed, turn, *TargetRPM,
         *SmoothedRPM, *ActualRPM, *BoostCurrent, stopped, mode
  msg    firmware text lines: s, host_time, kind (first word; CORRUPT = row damaged on the radio link), text
  sens   parsed 'DBG SENSORS' lines: s, rawF, rawB, rawL, rawR, floorF..floorR,
         W, seatLoad, seated, calibrated, calActive
  vesc   parsed 'DBG VESC' lines: s, id, age, frames, erpm, current, duty, fetC, vin
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
                  r"W=(-?\d+) seatLoad=(-?\d+) seated=(\d) calibrated=(\d) calActive=(\d)")
VESC = re.compile(r"DBG VESC id=(\d+) age=(\d+) frames=(\d+) erpm=(-?\d+) current=(\S+) duty=(\S+) fetC=(\S+) vin=(\S+)")
TREND = """SELECT CAST(s/{n} AS INT)*{n} AS s0, MIN(host_time) AS at, mode,
  ROUND(AVG(F),1) F, ROUND(AVG(B),1) B, ROUND(AVG(L),1) L, ROUND(AVG(R),1) R,
  ROUND(AVG(speed),3) speed, ROUND(AVG(turn),3) turn,
  ROUND(AVG(leftActualRPM),1) lRPM, ROUND(AVG(rightActualRPM),1) rRPM, COUNT(*) n
FROM t GROUP BY s0 ORDER BY s0"""


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
        db.execute("CREATE TABLE sens (s, rawF, rawB, rawL, rawR, floorF, floorB, floorL, floorR, "
                   "W, seatLoad, seated, calibrated, calActive)")
        db.execute("CREATE TABLE vesc (s, id, age, frames, erpm, current, duty, fetC, vin)")
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
            db.execute("INSERT INTO msg VALUES (?,?,?,?)", (s, r[0], "CORRUPT" if text[:1].isdigit() else text.split(" ")[0], text))
            m = SENS.search(text)
            if m:
                db.execute(f"INSERT INTO sens VALUES ({','.join('?' * 14)})", [s] + [int(g) for g in m.groups()])
            m = VESC.search(text)
            if m:
                db.execute(f"INSERT INTO vesc VALUES ({','.join('?' * 9)})", [s] + [num(g) for g in m.groups()])
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
