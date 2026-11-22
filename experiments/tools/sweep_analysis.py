import csv, re, json, statistics as st, datetime as dt, sys
src=sys.argv[1]
def secs(s):
    t=dt.datetime.strptime(s,"%H:%M:%S.%f"); return t.hour*3600+t.minute*60+t.second+t.microsecond/1e6
rows=[]
for r in csv.reader(open(src)):
    if len(r)>=2 and r[1].startswith('DBG R '):
        p=r[1].split(); f,b,l,rr=[max(int(v)-372,0) for v in p[3:7]]; tot=f+b+l+rr
        if tot>0: rows.append((secs(r[0]),(rr-l)/tot,(f-b)/tot))
def seg(a,b,axis,sign):
    a,b=secs(a),secs(b); pts=[(t,x,y) for t,x,y in rows if a<=t<=b]
    v=[(p[1] if axis=='x' else p[2])*sign for p in pts]; t0=pts[0][0]; T=pts[-1][0]-t0
    base=st.mean(v[:4]); out=[]
    # 10 bins of progress
    for i in range(11):
        tt=t0+T*i/10; near=[vv for (p,vv) in zip(pts,v) if abs(p[0]-tt)<=0.15]
        out.append(round(st.mean(near)-base,3))
    return out
S={'forward':seg('22:40:23.40','22:40:28.45','y',1),'back':seg('22:40:41.40','22:40:49.40','y',-1),
   'right':seg('22:40:59.40','22:41:03.50','x',1),'left':seg('22:41:11.40','22:41:15.45','x',-1)}
hold=[(x,y) for t,x,y in rows if secs('22:41:21.40')<=t<=secs('22:41:26.40')]
nx,ny=st.pstdev([h[0] for h in hold]),st.pstdev([h[1] for h in hold])
res={k:round(v[-1]/(2*(ny if k in('forward','back') else nx)),1) for k,v in S.items()}
print(json.dumps({'curves':S,'noise':{'x':round(nx,4),'y':round(ny,4)},'steps':res}))
