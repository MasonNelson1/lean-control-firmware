"""Compare lean-detection math on real sweeps (E6, config F5 B6 L5 R5).
A = current firmware with tonight's gains (F7 B3 L0.01 R0.01), calibrated at active neutral.
B = current firmware with its default gains (F2 B3 L1 R1).
C = centre of pressure, all four sensors, normalised by each direction's calibrated range.
Per direction and approach: dead zone (% of the lean before output leaves the deadband),
saturation (% of the lean where output hits full), and resolution steps
(usable signal span / 2 sigma of the same signal at rest)."""
import csv, sys, json, math, statistics as st, datetime as dt
def secs(s):
    t=dt.datetime.strptime(s if '.' in s else s+'.0',"%H:%M:%S.%f"); return t.hour*3600+t.minute*60+t.second+t.microsecond/1e6
src=sys.argv[1]; R=[]
for r in csv.reader(open(src)):
    if len(r)>=2 and r[1].startswith('DBG R '):
        R.append((secs(r[0]),[int(v) for v in r[1].split()[3:7]]))
def win(a,b): a,b=secs(a),secs(b); return [p for t,p in R if a<=t<=b]
NEUT=win('23:05:51','23:05:57'); floors=[st.mean(c) for c in zip(*NEUT)]; W=sum(floors)/4
SWEEPS={'forward':[('23:05:57.85',6.0),('23:06:11.49',3.6)],'right':[('23:06:21.53',3.8),('23:06:30.93',4.5)],
        'left':[('23:06:42.98',4.2),('23:06:49.53',6.1)],'back':[('23:06:58.57',2.2),('23:07:07.29',2.3),('23:07:17.49',2.0)]}
DB=0.05; eS=math.log(0.09)/math.log(0.21); eT=math.log(0.20)/math.log(0.35)
def firm(p,g):
    f,b,l,r=[(p[i]-floors[i])*g[i] for i in range(4)]
    return max(-1,min(1,(f-b)/W)), max(-1,min(1,(r-l)/W))
def cop(p):
    a=[max(v-372,0) for v in p]; T=sum(a) or 1; return ((a[0]-a[1])/T,(a[3]-a[2])/T)
n0=[cop(p) for p in NEUT]; cy0,cx0=st.mean(q[0] for q in n0),st.mean(q[1] for q in n0)
AX={'forward':(0,1),'back':(0,-1),'right':(1,1),'left':(1,-1)}
def series(d,fn):
    ax,sg=AX[d]; out=[]
    for start,dur in SWEEPS[d]:
        t0=secs(start); pts=[(t,p) for t,p in R if t0<=t<=t0+dur]
        out.append([(t-t0)/dur for t,_ in pts]); out.append([sg*fn(p)[ax] for _,p in pts])
    return out
res={}; curves={}
ranges={d:st.mean(max(s) for s in series(d,lambda p:(cop(p)[0]-cy0,cop(p)[1]-cx0))[1::2]) for d in AX}
for name,fn in {'A':lambda p:firm(p,[7,3,0.01,0.01]),'B':lambda p:firm(p,[2,3,1,1]),
                'C':None}.items():
    res[name]={}; curves[name]={}
    for d,(ax,sg) in AX.items():
        if name=='C':
            f=lambda p,ax=ax,d=d:((cop(p)[0]-cy0)/ranges['forward' if d in('forward','back') and sg>0 else d] if ax==0 else 0,
                                  (cop(p)[1]-cx0)/ranges[d] if ax==1 else 0)
            f2=(lambda p,ax=ax,sg=sg,d=d:tuple(((cop(p)[0]-cy0) if i==0 else (cop(p)[1]-cx0))/ranges[d] for i in range(2)))
            fn2=f2
        else: fn2=fn
        noise=st.pstdev([fn2(p)[ax] for p in NEUT]) or 1e-4
        S=series(d,fn2); dz=[];sat=[];steps=[];cv=[]
        for prog,val in zip(S[0::2],S[1::2]):
            dead=next((x for x,v in zip(prog,val) if v>DB),1.0); full=next((x for x,v in zip(prog,val) if v>=0.98),1.0)
            mx=min(max(val),1.0); usable=max(mx-DB,0)
            dz.append(dead); sat.append(full); steps.append(usable/(2*noise))
            cv.append([round(st.mean([v for x,v in zip(prog,val) if abs(x-q/10)<=0.06] or [0]),3) for q in range(11)])
        res[name][d]={'dead%':round(100*st.mean(dz)),'sat%':round(100*st.mean(sat)),'steps':round(st.mean(steps),1),'noise':round(noise,4)}
        curves[name][d]=[round(st.mean(c),3) for c in zip(*cv)]
print(json.dumps({'floors':[round(f) for f in floors],'W':round(W),'ranges':{k:round(v,3) for k,v in ranges.items()},'res':res,'curves':curves}))
