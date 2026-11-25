#!/usr/bin/env python3
"""Cut a window out of a chair_tap live log into JSON for the braking-story site.

usage: extract_fault.py LOG START END OUT.json
Pairs each 20 Hz `DBG M` line (VESC currents, duty, battery current) with the
control CSV row of the same Teensy millisecond (command and measured RPM).
Lines garbled by interleaved Bluetooth output are skipped.
"""
import json, re, sys

log, start, end, out = sys.argv[1:5]
m_pat = re.compile(r'DBG M (\d+) ' + ' '.join([r'(-?[\d.]+)'] * 8) + r'\s')
row_pat = re.compile(r'^[\d:.]+,(\d+),' + ','.join([r'(-?[\d.]+)'] * 17) + r',(LEAN|REMOTE|DISARM)')
fault_pat = re.compile(r'^([\d:.]+),DBG VFAULT id=(\d+) code=(\d+) name=(\w+) ms=(\d+).*side=(\w)')
M, R, faults = {}, {}, []
for line in open(log, errors='ignore'):
    t = line[:12]
    if not (start <= t <= end):
        continue
    for m in m_pat.finditer(line):
        ms = int(m.group(1)); v = [float(x) for x in m.groups()[1:]]
        M[ms] = dict(curL=v[0], curR=v[1], dutyL=v[2], dutyR=v[3], iinL=v[4], iinR=v[5])
    r = row_pat.match(line)
    if r:
        g = r.groups(); ms = int(g[0]); f = [float(x) for x in g[1:18]]
        # cols: F B L R total userWeight speed turn tgtL tgtR cmdL cmdR stopped actL actR
        R[ms] = dict(cmdL=f[10], cmdR=f[11], actL=f[13], actR=f[14], lean=f[6], turn=f[7])
    fm = fault_pat.match(line)
    if fm:
        faults.append(dict(ms=int(fm.group(5)), code=fm.group(4), side=fm.group(6), clock=fm.group(1)))
pts = []
for ms in sorted(set(M) & set(R)):
    p = dict(ms=ms); p.update(M[ms]); p.update(R[ms]); pts.append(p)
t0 = pts[0]['ms']
for p in pts: p['t'] = round((p['ms'] - t0) / 1000, 3)
for f in faults: f['t'] = round((f['ms'] - t0) / 1000, 3)
json.dump(dict(source=log.split('/')[-1], start=start, end=end, points=pts, faults=faults), open(out, 'w'))
print(out, len(pts), 'points', faults)
