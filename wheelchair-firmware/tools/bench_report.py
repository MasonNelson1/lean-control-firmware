"""Left-vs-right report of a wheels-off bench test (firmware src/bench_test.h).

    python3 tools/bench_report.py                    # latest run in logs/live/current.csv
    python3 tools/bench_report.py --file logs/live/X.csv
    python3 tools/bench_report.py --list             # every run in the log
    python3 tools/bench_report.py --run 2            # the second run (1-based; -1 = latest)
    python3 tools/bench_report.py --no-html          # table only

Start a run with the wheels OFF THE GROUND, nobody in the chair, in DISARM:
    python.exe tools/chair_config.py set "bench" 1

Prints one row per step from the firmware's `DBG BENCH ... sum` lines (computed
on the Teensy at 200 Hz over the last 2 s of each hold) and flags:
  ASYM   motor current differs between sides by > 25% of their mean (holds with
         both sides under 0.5 A are skipped: that's noise, not drag)
  RIPPLE RPM standard deviation > 10% of the setpoint
  MISSED the mean RPM didn't reach the setpoint (within 10% or 1.5 RPM); the
         brake step: a side never got below 3 RPM
Writes a chart (RPM and motor current, left vs right on the same axes, from the
20 Hz CSV and `DBG M` lines) to ../experiments/bench-<log>-run<N>.html.
Runs under plain WSL python (it only reads the file chair_tap.py writes).
"""

import argparse
import csv
import html
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIVE = os.path.join(HERE, "..", "logs", "live", "current.csv")
EXPERIMENTS = os.path.normpath(os.path.join(HERE, "..", "..", "experiments"))

ASYM_LIMIT = 0.25
ASYM_MIN_A = 0.5
RIPPLE_LIMIT = 0.10
MIN_ERPM_RPM = 300 / 58.18  # the VESC's minimum ERPM in wheel RPM (≈5.2)

BENCH = re.compile(r"^DBG BENCH (\d+) (.*)$")
MOTOR = re.compile(r"^DBG M (\d+) (\S+) (\S+)")


def num(text):
    try:
        v = float(text)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def kv(text):
    """key=value pairs of a bench line; the first bare word (start/sum/end/...) is 'what'."""
    out = {}
    for token in text.split():
        if "=" in token:
            k, v = token.split("=", 1)
            out[k] = v
        else:
            out.setdefault("what", token)
    return out


def read_log(path):
    """Ordered events: ('bench', ms, dict), ('rpm', ms, l, r), ('cur', ms, l, r), each with host time."""
    events = []
    with open(path, newline="", errors="replace") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return events
        cols = {name: i for i, name in enumerate(header)}
        n = len(header)
        for row in reader:
            if not row:
                continue
            host = row[0]
            if len(row) == n and "time_ms" in cols:
                ms = num(row[cols["time_ms"]])
                if ms is not None:
                    events.append(("rpm", int(ms), host, num(row[cols["leftActualRPM"]]),
                                   num(row[cols["rightActualRPM"]])))
                continue
            text = ",".join(row[1:]).strip()
            m = BENCH.match(text)
            if m:
                events.append(("bench", int(m[1]), host, kv(m[2])))
                continue
            m = MOTOR.match(text)
            if m:
                events.append(("cur", int(m[1]), host, num(m[2]), num(m[3])))
    return events


def split_runs(events):
    """Each run: the events from a `begin` (or a lone `refused`) to its done/released line."""
    runs, current = [], None
    for e in events:
        if e[0] == "bench":
            d = e[3]
            if d.get("what") == "refused" and current is None:
                runs.append({"events": [e], "refused": d.get("why", "?"), "host": e[2]})
                continue
            if d.get("what") == "begin":
                if current:
                    runs.append(current)  # never finished (log cut, reboot)
                current = {"events": [], "host": e[2]}
        if current is not None:
            current["events"].append(e)
            if e[0] == "bench" and e[3].get("what") in ("done", "released"):
                runs.append(current)
                current = None
    if current:
        runs.append(current)
    for run in runs:
        summarize_run(run)
    return runs


def summarize_run(run):
    run.setdefault("refused", None)
    run["steps"], run["starts"], run["abort"], run["done"] = [], [], None, None
    for e in run["events"]:
        if e[0] != "bench":
            continue
        d = e[3]
        if d.get("what") == "sum":
            run["steps"].append(d)
        elif d.get("what") == "start":
            run["starts"].append((e[1], d))
        elif d.get("what") == "abort":
            run["abort"] = d
        elif d.get("what") == "done":
            run["done"] = d
    bench_ms = [e[1] for e in run["events"] if e[0] == "bench"]
    run["t0"], run["t1"] = (min(bench_ms), max(bench_ms)) if bench_ms else (0, 0)


def outcome(run):
    if run["refused"]:
        return f"refused ({run['refused']})"
    if run["done"]:
        return f"completed in {int(run['done'].get('ms', 0)) / 1000:.1f} s"
    if run["abort"]:
        a = run["abort"]
        return f"ABORTED why={a.get('why')} at step {a.get('step')} ({int(a.get('t', 0)) / 1000:.1f} s in)"
    return "incomplete (log ends mid-run)"


def flags_for(step):
    """The flags for one summary line, as short strings."""
    flags = []
    set_ = num(step.get("set"))
    name = step.get("step", "")
    if name == "brake":
        for side in "LR":
            if num(step.get("stop" + side)) in (None, -1):
                flags.append(f"MISSED {side} never < 3 RPM")
        return flags
    if "okL" not in step:  # stop steps carry no stats
        return flags
    iL, iR = num(step.get("iL")), num(step.get("iR"))
    if iL is not None and iR is not None and max(abs(iL), abs(iR)) >= ASYM_MIN_A:
        asym = abs(abs(iL) - abs(iR)) / ((abs(iL) + abs(iR)) / 2)
        if asym > ASYM_LIMIT:
            flags.append(f"ASYM {asym * 100:.0f}% ({'L' if abs(iL) > abs(iR) else 'R'} higher)")
    if set_:
        for side in "LR":
            rsd = num(step.get("rsd" + side))
            if rsd is not None and rsd / abs(set_) > RIPPLE_LIMIT:
                flags.append(f"RIPPLE {side} {rsd / abs(set_) * 100:.0f}%")
    for side in "LR":
        if step.get("ok" + side) == "0":
            note = " (below VESC min ERPM: expected)" if set_ is not None and 0 < abs(set_) < MIN_ERPM_RPM else ""
            flags.append(f"MISSED {side}{note}")
    return flags


def fmt(v, digits=1, width=6):
    x = num(v)
    return f"{x:{width}.{digits}f}" if x is not None else f"{'-':>{width}}"


def ripple(step, side):
    set_, rsd = num(step.get("set")), num(step.get("rsd" + side))
    return f"{rsd / abs(set_) * 100:5.1f}" if set_ and rsd is not None else f"{'-':>5}"


def table(run):
    lines = []
    head = (f"{'step':<7} {'set':>6} | {'rpm L':>6} {'rpm R':>6} | {'rip%L':>5} {'rip%R':>5} | "
            f"{'I L':>6} {'I R':>6} {'dI%':>4} | {'t90 L':>5} {'t90 R':>5} {'ovs%L':>5} {'ovs%R':>5} | flags")
    lines.append(head)
    lines.append("-" * len(head))
    for s in run["steps"]:
        name = s.get("step", "?")
        if name == "brake":
            lines.append(f"{name:<7} {fmt(s.get('amps')) + 'A':>6} | from {fmt(s.get('rpm0L'), 0, 3)}/"
                         f"{fmt(s.get('rpm0R'), 0, 3)} RPM: to < 3 RPM in L {s.get('stopL')} ms, R {s.get('stopR')} ms"
                         f"{'  ' + '; '.join(flags_for(s)) if flags_for(s) else ''}")
            continue
        if "okL" not in s:
            lines.append(f"{name:<7} {'stop':>6} | braked to still in {s.get('ms', '?')} ms "
                         f"(from {fmt(s.get('rpm0L'), 0, 3)}/{fmt(s.get('rpm0R'), 0, 3)} RPM)")
            continue
        iL, iR = num(s.get("iL")), num(s.get("iR"))
        dI = (f"{abs(abs(iL) - abs(iR)) / ((abs(iL) + abs(iR)) / 2) * 100:4.0f}"
              if iL is not None and iR is not None and abs(iL) + abs(iR) > 0 else f"{'-':>4}")
        step_cols = (f"{s.get('t90L', '-'):>5} {s.get('t90R', '-'):>5} {fmt(s.get('ovsL'), 1, 5)} {fmt(s.get('ovsR'), 1, 5)}"
                     if "t90L" in s else f"{'':>5} {'':>5} {'':>5} {'':>5}")
        lines.append(f"{name:<7} {fmt(s.get('set'))} | {fmt(s.get('rpmL'))} {fmt(s.get('rpmR'))} | "
                     f"{ripple(s, 'L')} {ripple(s, 'R')} | {fmt(s.get('iL'), 2)} {fmt(s.get('iR'), 2)} {dI} | "
                     f"{step_cols} | {'; '.join(flags_for(s))}")
    return lines


def interpretation(run):
    """A few plain sentences: what stands out."""
    out = []
    holds = [s for s in run["steps"] if "okL" in s and "t90L" not in s and s.get("step", "").startswith(("fwd", "rev"))]
    if holds:
        ratios = [abs(num(s["iL"])) / abs(num(s["iR"])) for s in holds
                  if num(s.get("iL")) and num(s.get("iR")) and max(abs(num(s["iL"])), abs(num(s["iR"]))) >= ASYM_MIN_A]
        if ratios:
            mean = sum(ratios) / len(ratios)
            side = "left" if mean > 1 else "right"
            pct = abs(mean - 1) * 100
            verdict = "within normal spread" if pct <= ASYM_LIMIT * 100 else "check that side for binding/misalignment"
            out.append(f"Steady holds: the {side} side draws {pct:.0f}% more current on average ({verdict}).")
    flagged = [s.get("step") for s in run["steps"] if flags_for(s)]
    out.append(f"Flagged steps: {', '.join(flagged)}." if flagged else "No step flagged.")
    return out


def series(run):
    """Chart data within the run, firmware ms -> seconds from the run start."""
    t0 = run["t0"]
    rpm = [(e[1], e[3], e[4]) for e in run["events"] if e[0] == "rpm"]
    cur = [(e[1], e[3], e[4]) for e in run["events"] if e[0] == "cur"]
    # Setpoint: RPM steps carry kind hold/step/spinup; stop/brake steps have none.
    sp, last = [], None
    for ms, d in run["starts"]:
        value = num(d.get("set")) if d.get("kind") in ("hold", "step", "spinup") else None
        if last is not None:
            sp.append([round((ms - t0) / 1000, 3), last])
        sp.append([round((ms - t0) / 1000, 3), value])
        last = value
    if sp:
        sp.append([round((run["t1"] - t0) / 1000, 3), last])
    conv = lambda rows: [[round((ms - t0) / 1000, 3), l, r] for ms, l, r in rows]  # noqa: E731
    marks = [[round((ms - t0) / 1000, 3), d.get("step", "")] for ms, d in run["starts"]]
    return {"rpm": conv(rpm), "cur": conv(cur), "set": sp, "marks": marks}


HTML = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bench Test Report</title>
<style>
:root{--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--grid:#e6e5e1;--box:#f0efec;--l:#2a78d6;--r:#eb6834;--set:#8a8984}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--grid:#3a3a37;--box:#252523;--l:#3987e5;--r:#d95926;--set:#8a8984}}
:root[data-theme="dark"]{--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--grid:#3a3a37;--box:#252523;--l:#3987e5;--r:#d95926;--set:#8a8984}
body{margin:0;padding:16px;background:var(--surface);color:var(--ink);font:14px/1.45 system-ui,sans-serif;max-width:1100px}
h1{font-size:18px;margin:0 0 4px}h2{font-size:15px;margin:18px 0 4px}p{margin:0 0 8px;color:var(--ink2)}
.take{background:var(--box);border-radius:8px;padding:12px 16px;margin:10px 0}.take li{margin:3px 0}
.legend{display:flex;gap:16px;flex-wrap:wrap;color:var(--ink2);margin:6px 0}.key{display:inline-flex;align-items:center;gap:6px}
.sw{width:16px;height:2px;display:inline-block}
.chart{position:relative}svg{width:100%;height:auto;display:block}
.tip{position:absolute;pointer-events:none;background:var(--surface);border:1px solid var(--grid);border-radius:6px;padding:4px 8px;font-size:12px;display:none;white-space:nowrap}
pre{background:var(--box);padding:10px;border-radius:8px;overflow-x:auto;font-size:12px}
</style></head><body>
<h1>Bench test, wheels off: left vs right</h1>
<p>__SUBTITLE__</p>
<div class="take"><ul>__TAKE__</ul></div>
<div class="legend"><span class="key"><span class="sw" style="background:var(--l)"></span>Left</span>
<span class="key"><span class="sw" style="background:var(--r)"></span>Right</span>
<span class="key"><span class="sw" style="border-top:2px dashed var(--set);height:0"></span>Setpoint</span></div>
<h2>Wheel speed (RPM)</h2><div class="chart" id="rpm"></div>
<h2>Motor current (A)</h2><div class="chart" id="cur"></div>
<h2>Per-step table (firmware statistics, last 2 s of each hold)</h2>
<pre>__TABLE__</pre>
<script>
const D=__DATA__;
const W=1000,H=300,M={l:48,r:12,t:10,b:26};
function chart(id,rows,withSet){
  const el=document.getElementById(id);
  const xs=rows.map(r=>r[0]).concat(D.set.map(s=>s[0]));
  const ys=rows.flatMap(r=>[r[1],r[2]]).concat(withSet?D.set.map(s=>s[1]):[]).filter(v=>v!==null);
  if(!rows.length){el.textContent='No samples in the log for this run.';return;}
  const x0=Math.min(...xs),x1=Math.max(...xs);let y0=Math.min(0,...ys),y1=Math.max(0,...ys);const pad=(y1-y0)*0.06||1;y0-=pad;y1+=pad;
  const X=v=>M.l+(v-x0)/(x1-x0||1)*(W-M.l-M.r),Y=v=>H-M.b-(v-y0)/(y1-y0)*(H-M.t-M.b);
  const ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg');svg.setAttribute('viewBox',`0 0 ${W} ${H}`);
  const add=(tag,attrs,text)=>{const e=document.createElementNS(ns,tag);for(const k in attrs)e.setAttribute(k,attrs[k]);if(text!==undefined)e.textContent=text;svg.appendChild(e);return e;};
  const step=(y1-y0)>200?50:(y1-y0)>60?20:(y1-y0)>20?5:(y1-y0)>8?2:1;
  for(let v=Math.ceil(y0/step)*step;v<=y1;v+=step){add('line',{x1:M.l,x2:W-M.r,y1:Y(v),y2:Y(v),stroke:'var(--grid)','stroke-width':v===0?1.5:1});add('text',{x:M.l-6,y:Y(v)+4,'text-anchor':'end','font-size':11,fill:'var(--ink2)'},v);}
  for(let t=Math.ceil(x0/10)*10;t<=x1;t+=10)add('text',{x:X(t),y:H-8,'text-anchor':'middle','font-size':11,fill:'var(--ink2)'},t+' s');
  for(const [t,name] of D.marks){add('line',{x1:X(t),x2:X(t),y1:M.t,y2:H-M.b,stroke:'var(--grid)','stroke-dasharray':'2 3'});}
  const path=(pts)=>{let d='',pen=false;for(const [x,y] of pts){if(y===null){pen=false;continue;}d+=(pen?'L':'M')+X(x).toFixed(1)+' '+Y(y).toFixed(1);pen=true;}return d;};
  if(withSet)add('path',{d:path(D.set),fill:'none',stroke:'var(--set)','stroke-width':1.5,'stroke-dasharray':'5 4'});
  add('path',{d:path(rows.map(r=>[r[0],r[1]])),fill:'none',stroke:'var(--l)','stroke-width':2,'stroke-linejoin':'round'});
  add('path',{d:path(rows.map(r=>[r[0],r[2]])),fill:'none',stroke:'var(--r)','stroke-width':2,'stroke-linejoin':'round'});
  const cross=add('line',{y1:M.t,y2:H-M.b,stroke:'var(--ink2)','stroke-width':1,visibility:'hidden'});
  const tip=document.createElement('div');tip.className='tip';el.append(svg,tip);
  svg.addEventListener('pointermove',ev=>{const b=svg.getBoundingClientRect();const t=x0+((ev.clientX-b.left)/b.width*W-M.l)/(W-M.l-M.r)*(x1-x0);
    let best=rows[0];for(const r of rows)if(Math.abs(r[0]-t)<Math.abs(best[0]-t))best=r;
    let stepName='';for(const [mt,n] of D.marks)if(mt<=best[0])stepName=n;
    let sp=null;for(const s of D.set)if(s[0]<=best[0])sp=s[1];
    cross.setAttribute('x1',X(best[0]));cross.setAttribute('x2',X(best[0]));cross.setAttribute('visibility','visible');
    const f=v=>v===null?'-':v.toFixed(1);
    tip.innerHTML=`<b>${best[0].toFixed(2)} s</b> ${stepName}<br>Left ${f(best[1])} &middot; Right ${f(best[2])}`+(withSet&&sp!==null?`<br>Setpoint ${f(sp)}`:'');
    tip.style.display='block';const px=(X(best[0])/W)*b.width;tip.style.left=Math.min(px+12,b.width-170)+'px';tip.style.top='8px';});
  svg.addEventListener('pointerleave',()=>{tip.style.display='none';cross.setAttribute('visibility','hidden');});
}
chart('rpm',D.rpm,true);chart('cur',D.cur,false);
</script></body></html>
"""


def write_html(run, path, label, table_lines):
    take = [outcome(run)] + interpretation(run)
    page = (HTML.replace("__SUBTITLE__", html.escape(label))
            .replace("__TAKE__", "".join(f"<li>{html.escape(t)}</li>" for t in take))
            .replace("__TABLE__", html.escape("\n".join(table_lines)))
            .replace("__DATA__", json.dumps(series(run), separators=(",", ":"))))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(page)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=os.environ.get("CHAIR_TAP", LIVE))
    ap.add_argument("--run", type=int, default=-1, help="1-based run number; -1 = latest (default)")
    ap.add_argument("--list", action="store_true", help="list the runs in the log")
    ap.add_argument("--html", help="chart path (default ../experiments/bench-<log>-run<N>.html)")
    ap.add_argument("--no-html", action="store_true")
    args = ap.parse_args(argv)

    runs = split_runs(read_log(args.file))
    if not runs:
        print(f"No bench test in {args.file}. Start one (wheels off, DISARM, seat empty):\n"
              f'    python.exe tools/chair_config.py set "bench" 1')
        return 1
    if args.list:
        for i, run in enumerate(runs, 1):
            print(f"{i:>3}  {run['host']}  {outcome(run)}")
        return 0
    index = args.run - 1 if args.run > 0 else len(runs) + args.run
    if not 0 <= index < len(runs):
        print(f"No run {args.run}; the log has {len(runs)} (see --list).")
        return 1
    run = runs[index]
    label = f"{os.path.basename(args.file)}, run {index + 1} of {len(runs)}, started {run['host']}: {outcome(run)}"
    print(label)
    if run["refused"]:
        print("  " + " ".join(f"{k}={v}" for k, v in run["events"][0][3].items() if k != "what"))
        return 2
    lines = table(run)
    for line in interpretation(run):
        print("  " + line)
    print()
    print("\n".join(lines))
    if not args.no_html:
        stem = os.path.splitext(os.path.basename(args.file))[0]
        path = args.html or os.path.join(EXPERIMENTS, f"bench-{stem}-run{index + 1}.html")
        write_html(run, path, label, lines)
        print(f"\nChart: {os.path.abspath(path)}")
    return 0 if run["done"] else 3


if __name__ == "__main__":
    sys.exit(main())
