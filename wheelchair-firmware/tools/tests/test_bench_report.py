"""bench_report.py against logs written by the real firmware routine.

bench_emit.cpp drives src/bench_test.h with simulated motors and prints a
tap-format CSV, so these tests break if the DBG BENCH format and the report
drift apart. Needs g++ (skipped without it).

    python3 -m unittest discover -s tools/tests
"""
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))
import bench_report  # noqa: E402


@unittest.skipUnless(shutil.which("g++"), "needs g++ to build the firmware emitter")
class BenchReportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.exe = os.path.join(cls.tmp, "bench_emit")
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(ROOT, "src"),
                        os.path.join(HERE, "bench_emit.cpp"), "-o", cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def log(self, *args, name="run.csv"):
        path = os.path.join(self.tmp, name)
        with open(path, "w") as f:
            subprocess.run([self.exe, *map(str, args)], stdout=f, check=True)
        return path

    def report(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = bench_report.main(list(argv))
        return code, out.getvalue()

    def test_completed_run_table_and_chart(self):
        path = self.log()
        chart = os.path.join(self.tmp, "chart.html")
        code, text = self.report("--file", path, "--html", chart)
        self.assertEqual(code, 0, text)
        self.assertIn("completed in", text)
        for step in ("creep3", "fwd90", "rev90", "up1", "down2", "brake"):
            self.assertIn(step, text)
        self.assertNotIn("ASYM", text)
        with open(chart) as f:
            page = f.read()
        self.assertIn("<title>Bench Test Report</title>", page)
        self.assertIn('"rpm":[[', page)
        self.assertIn('"set":[[', page)

    def test_binding_side_is_flagged(self):
        path = self.log(2.5)  # right side drags 2.5x
        code, text = self.report("--file", path, "--no-html")
        self.assertEqual(code, 0)
        self.assertIn("ASYM", text)
        self.assertIn("R higher", text)
        self.assertIn("right side draws", text)

    def test_aborted_run(self):
        path = self.log(1.0, 12000)  # a button 12 s in
        code, text = self.report("--file", path, "--no-html")
        self.assertEqual(code, 3)
        self.assertIn("ABORTED why=button", text)

    def test_runs_are_listed_and_selected(self):
        first = self.log(name="a.csv")
        second = self.log(1.0, 5000, name="b.csv")
        both = os.path.join(self.tmp, "both.csv")
        with open(both, "w") as out, open(first) as a, open(second) as b:
            out.write(a.read())
            next(b)  # one header
            out.write(b.read())
        code, text = self.report("--file", both, "--list")
        self.assertEqual(code, 0)
        self.assertIn("  1  ", text)
        self.assertIn("completed", text.splitlines()[0])
        self.assertIn("ABORTED", text.splitlines()[1])
        code, text = self.report("--file", both, "--run", "1", "--no-html")
        self.assertEqual(code, 0)

    def test_flags_for_summary_lines(self):
        hold = bench_report.kv("step=fwd30 sum set=30.0 n=400 iL=2.00 isdL=0.1 rpmL=30.0 rsdL=4.0 okL=1 "
                               "iR=3.00 isdR=0.1 rpmR=20.0 rsdR=0.2 okR=0")
        flags = " ".join(bench_report.flags_for(hold))
        self.assertIn("ASYM 40%", flags)
        self.assertIn("RIPPLE L 13%", flags)
        self.assertIn("MISSED R", flags)
        creep = bench_report.kv("step=creep3 sum set=3.0 n=400 iL=0.1 isdL=0 rpmL=0 rsdL=0 okL=0 "
                                "iR=0.1 isdR=0 rpmR=0 rsdR=0 okR=0")
        self.assertIn("expected", " ".join(bench_report.flags_for(creep)))
        self.assertNotIn("ASYM", " ".join(bench_report.flags_for(creep)))  # both under 0.5 A
        brake = bench_report.kv("step=brake sum amps=15.0 rpm0L=90 rpm0R=90 stopL=140 stopR=-1")
        self.assertEqual(bench_report.flags_for(brake), ["MISSED R never < 3 RPM"])

    def test_no_bench_in_log(self):
        path = os.path.join(self.tmp, "empty.csv")
        with open(path, "w") as f:
            f.write("host_time,time_ms,leftActualRPM,rightActualRPM\n")
        code, text = self.report("--file", path)
        self.assertEqual(code, 1)
        self.assertIn('chair_config.py set "bench" 1', text)


if __name__ == "__main__":
    unittest.main()
