"""chair_sql.py and chair_summary.py read the firmware's telemetry lines.

The sample lines are in the exact formats the firmware prints (src/vesc_monitor.h,
src/main.cpp; tests/native/vesc_monitor_test.cpp checks the same prefixes).

    python3 -m unittest discover -s tools/tests
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import chair_sql  # noqa: E402
import chair_summary  # noqa: E402

HEADER = ("host_time,time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,leftSmoothedRPM,"
          "rightSmoothedRPM,stopped,leftActualRPM,rightActualRPM,leftBoostCurrent,rightBoostCurrent,mode")
LINES = [
    "61000,1,2,3,4,10,370,0.1000,0.0000,20.0000,20.0000,15.0000,15.0000,0,14.0000,15.0000,0.0000,0.0000,LEAN",
    "DBG VESC id=2 age=20 frames=1 erpm=-8000 current=-23.0 duty=0.123 fetC=35.2 vin=52.7 motC=-270.0 iin=-4.5 "
    "ipk=-48.0 iinpk=-6.1 ah=0.1234/0.0000 wh=6.432/0.000 tach=-987654 pid=0.0 ages=20/21/22/23/180",
    # Older firmware: only the first eight fields.
    "DBG VESC id=26 age=3 frames=5197 erpm=0 current=0.0 duty=0.000 fetC=22.5 vin=0.0",
    # Garbled on the radio: tail lost and the next line run in.
    "DBG VESC id=2 age=0 frames=0 erpm=0 current=0.0 duty=0.000 fetC=0.0 vin= 425 409",
    "DBG VVAL id=2 age=80 fault=0 fetC=25.4 motC=-270.0 imot=-41.23 iin=-3.21 iD=0.12 iQ=-41.23 duty=-0.456 "
    "erpm=-8123 vin=52.3 ah=0.1234/0.0567 wh=6.432/2.500 tach=-123456/654321 pid=0.00 mos=30.1/30.2/30.3 "
    "vD=1.23 vQ=-20.50 st=0 faults=1 last=4 n=12/11/1 lat=9",
    "DBG VVAL id=26 age=-1 n=0/0/0",
    "DBG VFAULT id=2 code=4 name=ABS_OVER_CURRENT ms=2000 imot=-48.70 iin=-3.21 erpm=-8123 duty=-0.456 vin=52.3 "
    "fetC=25.4 n=1",
    "DBG M 61023 -23.0 1.0 -0.123 0.010 -4.5 0.2 -50.1 3.0 -512 10 -500 9 0.0 0.0 1.42",
    "DBG SYS up=61 loops=48000 loopMax=850 ctrl=5/6 cpuC=48.2 canRx=123456 canTxFail=0 tec=0 rec=0 "
    "bt=400000/9000 drop=0/0 btFree=600 cfg=0",
    "DBG SYS up=62 loops=48000 loopMax=900 ctrl=5/7 cpuC=48.3 canRx=124000 canTxFail=0 tec=0 rec=0 "
    "bt=406000/9130 drop=120/1 btFree=12 cfg=0",
    "DBG IO spd=3/2048/S prof=1/150/S btn=0100 pend=0 act=1 sel=LEAN cur=DISARM seated=0",
    "DBG EVT 61000 mode DISARM sel=LEAN why=seat-empty",
    "DBG EVT 61010 btn lean down",
]


class ChairToolsTest(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".csv")
        with os.fdopen(fd, "w") as f:
            f.write(HEADER + "\n")
            for i, line in enumerate(LINES):
                f.write(f"12:00:{i:02d}.000,{line}\n")

    def tearDown(self):
        os.remove(self.path)

    def test_sql_tables(self):
        db = chair_sql.load(self.path)
        rows = db.execute("SELECT id, age, frames, motC, iin, ipk, ah, ahc, tach, age1, age5 FROM vesc ORDER BY s").fetchall()
        self.assertEqual(rows, [(2, 20, 1, -270, -4.5, -48, 0.1234, 0, -987654, 20, 180),
                                (26, 3, 5197, None, None, None, None, None, None, None, None)])
        v = db.execute("SELECT fault, imot, curD, curQ, tach, tachAbs, mos3, voltQ, lastFault, asked, replies, lost, lat "
                       "FROM vval WHERE id = 2").fetchone()
        self.assertEqual(v, (0, -41.23, 0.12, -41.23, -123456, 654321, 30.3, -20.5, 4, 12, 11, 1, 9))
        self.assertEqual(db.execute("SELECT age, asked FROM vval WHERE id = 26").fetchone(), (-1, 0))
        self.assertEqual(db.execute("SELECT id, code, name, time_ms, imot FROM vfault").fetchone(),
                         (2, 4, "ABS_OVER_CURRENT", 2000, -48.7))
        self.assertEqual(db.execute("SELECT time_ms, curL, pidL, accL, rateR, lim FROM motor").fetchone(),
                         (61023, -23, -50.1, -512, 9, 1.42))
        self.assertEqual(db.execute("SELECT ctrlMax, dropLines, btFree FROM sys ORDER BY s DESC").fetchone(), (7, 1, 12))
        self.assertEqual(db.execute("SELECT spdPos, spdRaw, spdState, btnLean, btnRemote, act, sel, cur FROM io").fetchone(),
                         (3, 2048, "S", 0, 1, 1, "LEAN", "DISARM"))
        self.assertEqual(db.execute("SELECT time_ms, kind, detail FROM evt ORDER BY time_ms").fetchall(),
                         [(61000, "mode", "DISARM sel=LEAN why=seat-empty"), (61010, "btn", "lean down")])
        # The 20 Hz motor line stays out of msg; the rest are kept as text too.
        self.assertEqual(db.execute("SELECT COUNT(*) FROM msg WHERE kind = 'DBG' AND text LIKE 'DBG M %'").fetchone(), (0,))

    def test_summary_shows_vesc_health(self):
        chair_summary.PATH = self.path
        out = io.StringIO()
        sys.argv = ["chair_summary.py", "all"]
        with contextlib.redirect_stdout(out):
            chair_summary.main()
        text = out.getvalue()
        self.assertIn("VESC health", text)
        self.assertIn("id 2: status age 20 ms, FET 35.2 C", text)
        self.assertIn("motor current peak -48.0 A, input current peak -6.1 A", text)
        self.assertIn("polls asked/replied/lost 12/11/1, slowest reply 9 ms", text)
        self.assertIn("FAULT 12:00:06.000 id=2 ABS_OVER_CURRENT (code 4): motor -48.70 A", text)
        self.assertIn("WARN: VESC 2 faulted: ABS_OVER_CURRENT", text)
        self.assertIn("left 20 Hz: motor current peak -23.0 A", text)
        self.assertIn("Bluetooth: 6000 B/s sent, dropped 1 lines (120 B)", text)
        self.assertIn("WARN: Bluetooth TX buffer overflowed", text)
        self.assertIn("12:00:11.000 61000 mode DISARM sel=LEAN why=seat-empty", text)
        self.assertIn("WARN: VESC 26: no GET_VALUES reply", text)
        # Summarised lines are not repeated in the message list.
        messages = text.split("firmware messages")[-1]
        self.assertNotIn("DBG VVAL", messages)
        self.assertNotIn("DBG M ", messages)


if __name__ == "__main__":
    unittest.main()
