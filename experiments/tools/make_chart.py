import csv, json, sys, datetime as dt
src = sys.argv[1]; out = sys.argv[2]
def secs(s):
    t = dt.datetime.strptime(s, "%H:%M:%S.%f"); return t.hour*3600+t.minute*60+t.second+t.microsecond/1e6
rows = []
with open(src) as f:
    r = csv.reader(f); hdr = next(r)
    for x in r:
        if len(x) == len(hdr):
            try: rows.append((secs(x[0]), float(x[2]), float(x[3])))
            except ValueError: pass
def series(a, b):
    a, b = secs(a), secs(b); buckets = {}
    for t, F, B in rows:
        if a <= t <= b:
            k = int(t - a); buckets.setdefault(k, []).append((400+F/7, 400+B/3))
    return [[k, round(sum(v[0] for v in vs)/len(vs)), round(sum(v[1] for v in vs)/len(vs))] for k, vs in sorted(buckets.items())]
def ph(a0, spans):
    a = secs(a0); return [[round(secs(s)-a), round(secs(e)-a), n] for s, e, n in spans]
E2s, E2e = "21:24:20.000", "21:28:20.000"
E3s, E3e = "21:36:40.000", "21:39:30.000"
data = {
 "E2": {"title": "E2: bolts as before", "pts": series(E2s, E2e), "phases": ph(E2s, [
   ("21:24:25.0","21:24:50.0","sit down"),("21:24:50.0","21:25:08.0","passive"),("21:25:08.0","21:25:33.0","forward ×3"),
   ("21:25:33.0","21:25:55.0","back-pivot ×3"),("21:25:59.0","21:26:20.0","back-touch"),("21:26:44.0","21:27:15.0","back-touch reps"),
   ("21:27:22.0","21:27:53.0","active"),("21:27:56.0","21:28:15.0","passive")])},
 "E3": {"title": "E3: bolts loosened", "pts": series(E3s, E3e), "phases": ph(E3s, [
   ("21:36:45.0","21:37:33.0","sit down"),("21:37:33.0","21:37:43.0","passive"),("21:37:44.0","21:37:54.0","active"),
   ("21:37:59.0","21:38:11.0","back-pivot"),("21:38:12.0","21:38:33.0","back-touch"),("21:38:35.0","21:38:48.0","forward"),
   ("21:38:49.0","21:39:00.0","passive"),("21:39:01.0","21:39:30.0","active")])},
}
html = open('' + __import__('os').path.join(__import__('os').path.dirname(__file__), 'template.html') + '').read().replace('__DATA__', json.dumps(data))
open(out, 'w').write(html)
print(len(data["E2"]["pts"]), len(data["E3"]["pts"]))
