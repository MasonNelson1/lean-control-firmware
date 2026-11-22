"""Centre of pressure in plate inches from DBG R lines (config F5 B6 L5 R5 by default).
usage: cop_inches.py <tap.csv> <start> <end> [bucket_s] -> prints time, x", y", total per bucket"""
import csv,sys,statistics as st,datetime as dt
def secs(s):
    t=dt.datetime.strptime(s if '.' in s else s+'.0',"%H:%M:%S.%f"); return t.hour*3600+t.minute*60+t.second+t.microsecond/1e6
POS={'F':6.5,'B':-7.5,'L':-6.5,'R':6.5}
def cop(raw):
    f=[max(v-372,0) for v in raw]; T=sum(f)
    return ((f[3]*POS['R']+f[2]*POS['L'])/T,(f[0]*POS['F']+f[1]*POS['B'])/T,T) if T>300 else None
if __name__=='__main__':
    src,a,b=sys.argv[1:4]; bk=float(sys.argv[4]) if len(sys.argv)>4 else 2
    a,b=secs(a),secs(b); buckets={}
    for r in csv.reader(open(src)):
        if len(r)>=2 and r[1].startswith('DBG R '):
            t=secs(r[0])
            if a<=t<=b:
                c=cop([int(v) for v in r[1].split()[3:7]])
                if c: buckets.setdefault(int((t-a)//bk),[]).append((r[0][:8],)+c)
    for k in sorted(buckets):
        v=buckets[k]; print(v[0][0], '%+.2f %+.2f %5d'%(st.mean(q[1] for q in v),st.mean(q[2] for q in v),st.mean(q[3] for q in v)))
