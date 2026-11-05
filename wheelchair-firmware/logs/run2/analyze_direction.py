"""Quantify cross-axis commands without treating command vectors as headings."""
from pathlib import Path
import os,json
os.environ.setdefault('MPLCONFIGDIR','/tmp/lean-matplotlib')
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parent
d=pd.read_csv(ROOT/'aligned_data.csv')
t=d.time_since_serial_start_s.to_numpy();f=d.forward_lean_deg.to_numpy();r=d.right_lean_deg.to_numpy()
l=d.leftTargetRPM.to_numpy();rr=d.rightTargetRPM.to_numpy();turn=d.turn.to_numpy()
v=(l+rr)/2; yaw=(l-rr)/2
last_active=float(t[(abs(l)+abs(rr))>1][-1])
# Thresholds select posture, not inferred intent. 10 degrees forward / 5 back
# reflect the available reverse range. Sensitivity table varies the lateral gate.
active=(t<=last_active)&(d.stopped.to_numpy()==0)
forward=active&(f>=10)&(abs(r)<=2)
back=active&(f<=-5)&(abs(r)<=2)
initial_forward=forward&(t>=2)&(t<=6)
initial_back=back&(t>=6)&(t<=9)
groups={'Initial forward':initial_forward,'Initial backward':initial_back,
        'All centered forward':forward,'All centered backward':back}
rows=[]
for label,m in groups.items():
    ratio=abs(yaw[m])/np.maximum(abs(v[m]),1e-12)
    moving=abs(v[m])>=15
    rows.append(dict(group=label,duration_s=float(m.sum()*.005),
        median_forward_deg=float(np.median(f[m])),median_right_deg=float(np.median(r[m])),
        median_signed_turn=float(np.median(turn[m])),median_abs_turn=float(np.median(abs(turn[m]))),
        p95_abs_turn=float(np.percentile(abs(turn[m]),95)),
        fraction_abs_turn_gt_0_1=float(np.mean(abs(turn[m])>.1)),
        median_left_target_rpm=float(np.median(l[m])),median_right_target_rpm=float(np.median(rr[m])),
        median_wheel_imbalance_moving=float(np.median(ratio[moving])) if moving.any() else None))
metrics=pd.DataFrame(rows);metrics.to_csv(ROOT/'direction_metrics.csv',index=False)
sensitivity=[]
for gate in [1,2,3,5]:
    for label,axis in [('forward',f>=10),('backward',f<=-5)]:
        m=active&axis&(abs(r)<=gate)
        sensitivity.append(dict(posture=label,lateral_gate_deg=gate,duration_s=float(m.sum()*.005),
                                median_abs_turn=float(np.median(abs(turn[m]))),
                                fraction_abs_turn_gt_0_1=float(np.mean(abs(turn[m])>.1))))
pd.DataFrame(sensitivity).to_csv(ROOT/'direction_threshold_sensitivity.csv',index=False)
# Contiguous instances let the reader distinguish transient crossings from holds.
events=[]
bad=(forward|back)&(abs(turn)>.1)
edges=np.diff(np.r_[False,bad,False].astype(int))
for start,stop in zip(np.flatnonzero(edges==1),np.flatnonzero(edges==-1)):
    if (stop-start)*.005<.1: continue
    a=d.iloc[start:stop]
    events.append(dict(start_s=float(t[start]),end_s=float(t[stop-1]+.005),duration_s=(stop-start)*.005,
        posture='forward' if f[start]>0 else 'backward',median_forward_deg=float(a.forward_lean_deg.median()),
        median_right_deg=float(a.right_lean_deg.median()),median_turn=float(a.turn.median()),
        max_abs_turn=float(a.turn.abs().max()),median_left_rpm=float(a.leftTargetRPM.median()),
        median_right_rpm=float(a.rightTargetRPM.median())))
pd.DataFrame(events).to_csv(ROOT/'direction_events.csv',index=False)
fig,axs=plt.subplots(2,2,figsize=(15,10),layout='constrained')
a=axs[0,0];a.plot(t,f,label='Forward/back angle',color='#238b45');a.plot(t,r,label='Right/left angle',color='#9467bd')
a.axhspan(-2,2,color='#9467bd',alpha=.1,label='±2° lateral gate');a.set_xlim(1,9.5);a.set_ylabel('Chest lean (degrees)');a.set_xlabel('Time (s)');a.legend(loc='upper right');a.set_title('Initial forward and backward leans')
a=axs[0,1];a.plot(t,l,label='Left target',color='#146db0');a.plot(t,rr,label='Right target',color='#c46013');a.set_xlim(1,9.5);a.set_ylabel('Wheel target RPM');a.set_xlabel('Time (s)');a.legend();a.set_title('Backward lean produces unequal wheel targets')
for a in axs[0]:
    for mask,color in [(initial_forward,'#238b45'),(initial_back,'#be3434')]:
        ids=np.flatnonzero(mask)
        if len(ids): a.axvspan(t[ids[0]],t[ids[-1]],color=color,alpha=.12)
a=axs[1,0];center=forward|back
sc=a.scatter(f[center],turn[center],c=t[center],s=7,alpha=.45,cmap='viridis',rasterized=True)
a.axhspan(-.1,.1,color='gray',alpha=.15,label='±10% turn reference')
a.set_xlabel('Forward/back chest lean (degrees)');a.set_ylabel('Turn command (+ right; − left)');a.set_ylim(-1.05,1.05);a.set_title('Turning while chest is within ±2° laterally');a.legend(loc='upper left');fig.colorbar(sc,ax=a,label='Recording time (s)')
a=axs[1,1];vals=metrics.fraction_abs_turn_gt_0_1.to_numpy()*100
bars=a.bar(np.arange(4),vals,color=['#238b45','#be3434','#74a9cf','#fdae6b'])
a.set_xticks(np.arange(4),['Initial\nforward','Initial\nbackward','All centered\nforward','All centered\nbackward']);a.set_ylim(0,115);a.set_ylabel('Selected time with |turn| > 0.10 (%)');a.set_title('Cross-axis command frequency (10% reference threshold)')
for bar,val,dur in zip(bars,vals,metrics.duration_s): a.text(bar.get_x()+bar.get_width()/2,val+2,f'{val:.1f}%\n({dur:.2f} s selected)',ha='center',fontsize=9)
for a in axs.flat:a.grid(alpha=.2);a.axhline(0,color='black',lw=.6)
fig.suptitle('Run 2: does a straight chest lean command a turn?\nForward ≥10° or backward ≤−5°, lateral lean within ±2°; commands are not measured motion',fontsize=14)
fig.savefig(ROOT/'direction_coupling.png',dpi=170);fig.savefig(ROOT/'direction_coupling.svg')
print(metrics.to_string(index=False));print('last active',last_active);print('events',len(events))
