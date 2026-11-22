"""Find lean sweeps automatically in a tap log and measure each one.
usage: python3 -I auto_sweeps.py <tap.csv> <start HH:MM:SS> <end HH:MM:SS> <neutral_start> <neutral_end>
Neutral = an active-neutral hold inside the window (baseline + noise).
A sweep = CoP leaves neutral by > 0.06 on one axis; start = last sample within 0.015 of
neutral before it, peak = first sample reaching 95 % of the excursion maximum.
Prints JSON: neutral, noise, and per sweep: dir, start, peak, range, 11-point curve."""
import csv, sys, json, statistics as st, datetime as dt
def secs(s):
    t=dt.datetime.strptime(s if '.' in s else s+'.0',"%H:%M:%S.%f"); return t.hour*3600+t.minute*60+t.second+t.microsecond/1e6
src,a,b,na,nb=sys.argv[1:6]; a,b,na,nb=map(secs,(a,b,na,nb))
P=[]
for r in csv.reader(open(src)):
    if len(r)>=2 and r[1].startswith('DBG R '):
        t=secs(r[0])
        if a<=t<=b:
            f,bk,l,rr=[max(int(v)-372,0) for v in r[1].split()[3:7]]; T=f+bk+l+rr
            if T>200: P.append((t,(rr-l)/T,(f-bk)/T))
# smooth (5-sample moving average = 0.25 s)
S=[]
for i in range(len(P)):
    w=P[max(0,i-2):i+3]; S.append((P[i][0],st.mean(p[1] for p in w),st.mean(p[2] for p in w)))
N=[p for p in S if na<=p[0]<=nb]; x0,y0=st.mean(p[1] for p in N),st.mean(p[2] for p in N)
sx,sy=st.pstdev([p[1] for p in N]),st.pstdev([p[2] for p in N])
out=[];i=0
while i<len(S):
    dx,dy=S[i][1]-x0,S[i][2]-y0
    if max(abs(dx),abs(dy))>0.06:
        ax='x' if abs(dx)>abs(dy) else 'y'; sg=1 if (dx if ax=='x' else dy)>0 else -1
        k=1 if ax=='x' else 2; base=x0 if ax=='x' else y0
        j=i
        while j>0 and abs(S[j][k]-base)>0.015: j-=1
        e=i
        while e<len(S) and sg*(S[e][k]-base)>0.03: e+=1
        seg=S[i:e]; mx=max(sg*(p[k]-base) for p in seg)
        pk=next(p for p in S[j:e] if sg*(p[k]-base)>=0.95*mx)
        t0,t1=S[j][0],pk[0]; curve=[]
        for q in range(11):
            tt=t0+(t1-t0)*q/10; near=[sg*(p[k]-base) for p in S[j:e] if abs(p[0]-tt)<=0.1]
            curve.append(round(st.mean(near),3) if near else None)
        d={('x',1):'right',('x',-1):'left',('y',1):'forward',('y',-1):'back'}[(ax,sg)]
        out.append({'dir':d,'start':dt.datetime.fromtimestamp(t0-0,dt.timezone.utc).strftime('%H:%M:%S.%f')[:-4],'secs':round(t1-t0,1),'range':round(mx,3),'curve':curve})
        i=e
    else: i+=1
print(json.dumps({'neutral':[round(x0,3),round(y0,3)],'noise':[round(sx,4),round(sy,4)],'sweeps':out},indent=0))
