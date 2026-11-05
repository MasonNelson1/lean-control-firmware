"""Offline RPM command shaping for run 2. This is not a motor/vehicle model.

The logged F/B/L/R are already floor-subtracted, gained and clipped. Do not
apply sensor gains again. The controller takes a normalized motor demand,
current commanded RPM, and actual elapsed seconds. No measured RPM is available.
"""
from dataclasses import dataclass, replace
from pathlib import Path
import argparse
import json
import math
import numpy as np
import pandas as pd

WHEEL_CIRCUMFERENCE_M = 74.75 * .0254

@dataclass
class Tuning:
    max_rpm: float = 4.91744 * 60 / WHEEL_CIRCUMFERENCE_M
    low_speed_tau_s: float = .035
    high_speed_tau_s: float = .1

    ki: float = 0.0  # Integral is optional; this command-state model has no load error.
    kd: float = 2
    derivative_filter_s: float = .02
    low_speed_accel_m_s2: float = 2.85
    high_speed_accel_m_s2: float = 2.85
    max_jerk_rpm_s2: float = 150.

    @property
    def low_speed_accel_rpm_s(self):
        return self.low_speed_accel_m_s2 * 60 / WHEEL_CIRCUMFERENCE_M

    @property
    def high_speed_accel_rpm_s(self):
        return self.high_speed_accel_m_s2 * 60 / WHEEL_CIRCUMFERENCE_M

    def __post_init__(self):
        for name, value in vars(self).items():
            if not math.isfinite(value) or value < 0:
                raise ValueError(f'{name} must be finite and nonnegative')
        for name in ['max_rpm','low_speed_tau_s','high_speed_tau_s',
                     'low_speed_accel_m_s2','high_speed_accel_m_s2','max_jerk_rpm_s2']:
            if getattr(self,name) <= 0: raise ValueError(f'{name} must be positive')
        if self.high_speed_accel_rpm_s > self.low_speed_accel_rpm_s:
            raise ValueError('high-speed acceleration must not exceed low-speed acceleration')

class RPMSetpointPID:
    """PID outputs RPM/s, then integrates once per sample to get next RPM.

    current_rpm is the last *commanded* RPM, not measured wheel RPM. Keep a
    separate instance per wheel. D acts on command motion, avoiding target-step
    derivative kick. Acceleration and jerk bounds apply in normal operation;
    disabled/reset bypasses smoothing and zeros state immediately.
    """
    def __init__(self, tuning=None):
        self.p = tuning or Tuning()
        self.reset()

    def reset(self):
        self.integral = 0.
        self.acceleration = 0.
        self.filtered_rate = 0.
        self.previous_rpm = None

    def update(self, current_rpm, motor_demand, dt, enabled=True, schedule_rpm=None):
        if not enabled:
            self.reset()
            return 0.
        if not all(math.isfinite(x) for x in [current_rpm, motor_demand, dt]):
            raise ValueError('inputs must be finite')
        if not 0 < dt <= .1:
            raise ValueError('dt must be in (0, 0.1] s; reset after recording gaps')
        p = self.p
        target = float(np.clip(motor_demand,-1,1)) * p.max_rpm
        if self.previous_rpm is not None:
            rate = (current_rpm-self.previous_rpm)/dt
            alpha = dt/(p.derivative_filter_s+dt)
            self.filtered_rate += alpha*(rate-self.filtered_rate)
        self.previous_rpm = current_rpm
        # Shared speed scheduling in replay prevents different tunings solely
        # because the left and right wheels happen to be at different RPMs.
        schedule = abs(current_rpm) if schedule_rpm is None else abs(schedule_rpm)
        fraction = float(np.clip(schedule/p.max_rpm,0,1))
        fraction = fraction*fraction*(3-2*fraction)
        tau = p.low_speed_tau_s + fraction*(p.high_speed_tau_s-p.low_speed_tau_s)
        accel_limit = p.low_speed_accel_rpm_s + fraction*(p.high_speed_accel_rpm_s-p.low_speed_accel_rpm_s)
        error = target-current_rpm
        requested = error/tau + p.ki*self.integral - p.kd*self.filtered_rate
        bounded = float(np.clip(requested,-accel_limit,accel_limit))
        # Limit change of acceleration; the scheduled bound is approached
        # smoothly so an abrupt schedule change cannot violate the jerk bound.
        change = p.max_jerk_rpm_s2*dt
        self.acceleration += float(np.clip(bounded-self.acceleration,-change,change))
        # Conditional integration prevents windup against acceleration/jerk bounds.
        if p.ki and (abs(requested-self.acceleration)<1e-9 or error*(requested-self.acceleration)<=0):
            self.integral += error*dt
            limit = p.low_speed_accel_rpm_s/p.ki
            self.integral = float(np.clip(self.integral,-limit,limit))
        return current_rpm + self.acceleration*dt

def reconstruct(frame, p):
    weight=frame.userWeight.to_numpy(dtype=float)
    if np.any(weight<=0): raise ValueError('userWeight must be positive')
    speed=np.clip((frame.F-frame.B).to_numpy()/weight,-1,1)
    turn=np.clip((frame.R-frame.L).to_numpy()/weight,-1,1)
    speed[abs(speed)<.05]=0.;turn[abs(turn)<.05]=0.
    demands=np.column_stack([np.clip(speed+turn,-1,1),np.clip(speed-turn,-1,1)])
    return speed,turn,demands

def replay(time, demands, enabled, p):
    controllers=[RPMSetpointPID(p),RPMSetpointPID(p)]
    result=np.zeros_like(demands,dtype=float)
    state=np.zeros(2)
    for k in range(len(time)):
        if k==0 or time[k]-time[k-1]>.1:
            for ctrl in controllers: ctrl.reset()
            state[:]=0.
        else:
            dt=time[k]-time[k-1]
            if dt<=0: raise ValueError('timestamps must increase')
            schedule=max(abs(state))
            state=np.array([ctrl.update(state[j],demands[k,j],dt,bool(enabled[k]),schedule)
                            for j,ctrl in enumerate(controllers)])
        result[k]=state
    return result

def step_experiments(p):
    time=np.arange(0,6.005,.005)
    results={}
    for label,base,step in [('small_from_rest',0.,5.),('large_from_rest',0.,p.max_rpm),
                            ('small_at_speed',.8*p.max_rpm,5.)]:
        ctrl=RPMSetpointPID(p);state=base;out=[]
        target=base+step
        for t in time:
            demand=(base if t<.5 else target)/p.max_rpm
            state=ctrl.update(state,demand,.005)
            out.append(state)
        out=np.array(out)
        reached=np.flatnonzero((time>=.5)&(out>=base+.9*step))
        results[label]=dict(time_s=time,output_rpm=out,target_rpm=np.where(time<.5,base,target),
                           response_90_s=float(time[reached[0]]-.5) if len(reached) else None,
                           overshoot_rpm=float(max(0,out.max()-target)))
    return results

def response_text(seconds):
    return 'not reached in test' if seconds is None else f'{seconds:.3f} s'

def build_parser():
    root=Path(__file__).resolve().parent
    defaults=Tuning()
    parser=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('--input',type=Path,default=root.parent.parent/'run2Wheelchair.csv')
    parser.add_argument('--output',type=Path,default=root)
    parser.add_argument('--low-tau',type=float,default=defaults.low_speed_tau_s,help='Low-speed tracking time (s)')
    parser.add_argument('--high-tau',type=float,default=defaults.high_speed_tau_s,help='High-speed tracking time (s)')
    parser.add_argument('--ki',type=float,default=defaults.ki,help='Integral gain')
    parser.add_argument('--kd',type=float,default=defaults.kd,help='Derivative gain')
    parser.add_argument('--low-accel',type=float,default=defaults.low_speed_accel_m_s2,help='Low-speed wheel linear acceleration limit (m/s²)')
    parser.add_argument('--high-accel',type=float,default=defaults.high_speed_accel_m_s2,help='High-speed wheel linear acceleration limit (m/s²)')
    parser.add_argument('--jerk',type=float,default=defaults.max_jerk_rpm_s2,help='Jerk limit (RPM/s²)')
    return parser

def main():
    root=Path(__file__).resolve().parent
    args=build_parser().parse_args()
    p=Tuning(low_speed_tau_s=args.low_tau,high_speed_tau_s=args.high_tau,ki=args.ki,kd=args.kd,
             low_speed_accel_m_s2=args.low_accel,high_speed_accel_m_s2=args.high_accel,max_jerk_rpm_s2=args.jerk)
    d=pd.read_csv(args.input)
    required={'time_ms','F','B','L','R','userWeight','stopped','speed','turn','leftTargetRPM','rightTargetRPM'}
    if not required.issubset(d.columns): raise ValueError('missing required wheelchair columns')
    if not np.isfinite(d[list(required)].to_numpy(dtype=float)).all(): raise ValueError('nonfinite log values')
    time=(d.time_ms.to_numpy()-d.time_ms.iloc[0])/1000
    if np.any(np.diff(time)<=0): raise ValueError('timestamps must increase')
    speed,turn,demands=reconstruct(d,p)
    raw=demands*p.max_rpm
    enabled=d.stopped.to_numpy()==0
    raw[~enabled]=0.
    shaped=replay(time,demands,enabled,p)
    out=d.copy();out.insert(0,'relative_time_s',time)
    out['reconstructed_speed']=speed;out['reconstructed_turn']=turn
    for j,side in enumerate(['left','right']):
        out[side+'_motor_demand']=demands[:,j]
        out[side+'_raw_target_rpm']=raw[:,j]
        out[side+'_smoothed_target_rpm']=shaped[:,j]
        out[side+'_smoothed_acceleration_rpm_s']=np.r_[0,np.diff(shaped[:,j])/np.diff(time)]
        out[side+'_smoothed_acceleration_m_s2']=out[side+'_smoothed_acceleration_rpm_s']*WHEEL_CIRCUMFERENCE_M/60
    args.output.mkdir(exist_ok=True,parents=True)
    out.to_csv(args.output/'pid_replay.csv',index=False)
    experiments=step_experiments(p)
    summary={'tuning':vars(p),'wheel_circumference_m':WHEEL_CIRCUMFERENCE_M,
             'derived_acceleration_limits_rpm_s':{'low':p.low_speed_accel_rpm_s,'high':p.high_speed_accel_rpm_s},
             'meaning':'Offline command shaping; no measured motor feedback or vehicle dynamics',
             'sensor_reconstruction_max_error':{'speed':float(max(abs(speed-d.speed))),
               'turn':float(max(abs(turn-d.turn))),
               'left_target_rpm':float(max(abs(raw[:,0]-d.leftTargetRPM))),
               'right_target_rpm':float(max(abs(raw[:,1]-d.rightTargetRPM)))},'replay':{},'synthetic_steps':{}}
    for j,side in enumerate(['left','right']):
        summary['replay'][side]={'original_max_step_rpm':float(max(abs(np.diff(raw[:,j])))),
            'smoothed_max_step_rpm':float(max(abs(np.diff(shaped[:,j])))),
            'rms_tracking_difference_rpm':float(np.sqrt(np.mean((raw[:,j]-shaped[:,j])**2))),
            'max_abs_smoothed_rpm':float(max(abs(shaped[:,j])))}
    for label,result in experiments.items():
        summary['synthetic_steps'][label]={key:result[key] for key in ['response_90_s','overshoot_rpm']}
    (args.output/'pid_summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    import os
    os.environ.setdefault('MPLCONFIGDIR','/tmp/lean-matplotlib')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(3,1,figsize=(15,10),layout='constrained')
    for j,side in enumerate(['Left','Right']):
        axes[j].plot(time,raw[:,j],color='gray',alpha=.7,lw=1,label='Sensor-derived original target')
        axes[j].plot(time,shaped[:,j],color=['#146db0','#c46013'][j],lw=1.3,label='PID-shaped target')
        axes[j].set_ylabel(side+' target RPM');axes[j].legend();axes[j].set_xlabel('Run 2 time (s)')
    change=np.max(abs(np.diff(raw,axis=0)),axis=1)
    k=int(np.argmax(change))+1
    axes[2].plot(time,raw[:,0],color='gray',alpha=.6,label='Original left')
    axes[2].plot(time,shaped[:,0],color='#146db0',label='Smoothed left')
    axes[2].plot(time,raw[:,1],color='gray',alpha=.6,ls='--',label='Original right')
    axes[2].plot(time,shaped[:,1],color='#c46013',label='Smoothed right')
    axes[2].set_xlim(max(0,time[k]-1),min(time[-1],time[k]+2))
    axes[2].set_title('Zoom around largest recorded target jump',loc='left');axes[2].set_ylabel('Target RPM');axes[2].set_xlabel('Run 2 time (s)');axes[2].legend(ncol=2)
    for ax in axes: ax.grid(alpha=.2);ax.axhline(0,color='black',lw=.5)
    settings=f'High-speed tau={p.high_speed_tau_s:g} s | low-speed tau={p.low_speed_tau_s:g} s | accel low/high={p.low_speed_accel_m_s2:g}/{p.high_speed_accel_m_s2:g} m/s²'
    fig.suptitle('Run 2: sensor-derived RPM targets with PID command shaping\n'+settings)
    fig.savefig(args.output/'pid_replay.png',dpi=160)
    tag=format(p.high_speed_tau_s,'.8g').replace('.','p')
    fig.savefig(args.output/f'pid_replay_tau_{tag}.png',dpi=160)
    fig,axes=plt.subplots(1,3,figsize=(15,4),layout='constrained')
    for ax,(label,result) in zip(axes,experiments.items()):
        ax.plot(result['time_s'],result['target_rpm'],color='gray',label='Requested')
        ax.plot(result['time_s'],result['output_rpm'],label='Shaped')
        ax.set_title(label.replace('_',' ')+"\n90% response: "+response_text(result['response_90_s']))
        ax.set_xlim(.3,4);ax.set_xlabel('Time (s)');ax.set_ylabel('Command RPM');ax.grid(alpha=.2);ax.legend()
    fig.suptitle('Synthetic command steps (not measured motor response)\n'+settings)
    fig.savefig(args.output/'pid_step_response.png',dpi=160)

    # A direct comparison exposes changes that are subtle on a full-run plot.
    fig,axes=plt.subplots(3,1,figsize=(14,10),layout='constrained')
    taus=sorted(set([.1,.25,1.,p.high_speed_tau_s]))
    for j,side in enumerate(['Left','Right']):
        axes[j].plot(time,raw[:,j],color='gray',alpha=.5,lw=1,label='Original target')
        axes[j].set_ylabel(side+' command RPM')
        axes[j].set_xlabel('Run 2 time (s)')
        axes[j].set_xlim(52,59)
    for tau in taus:
        trial=replace(p,high_speed_tau_s=tau)
        trace=shaped if tau==p.high_speed_tau_s else replay(time,demands,enabled,trial)
        label=f'tau={tau:g} s'+(' (current)' if tau==p.high_speed_tau_s else '')
        for j in range(2):axes[j].plot(time,trace[:,j],label=label,lw=1.5)
        step=step_experiments(trial)['small_at_speed']
        axes[2].plot(step['time_s'],step['output_rpm'],label=label+'; 90%: '+response_text(step['response_90_s']))
    axes[0].set_title('Recorded final forward movement: acceleration limits can hide tau changes',loc='left')
    axes[2].plot(step['time_s'],step['target_rpm'],color='gray',ls='--',label='Requested RPM')
    axes[2].set_xlim(.4,3.5);axes[2].set_ylabel('Command RPM');axes[2].set_xlabel('Synthetic test time (s)')
    axes[2].set_title('Small 5 RPM step at 80% speed: tau has a clearer effect',loc='left')
    for ax in axes:ax.grid(alpha=.2);ax.legend(fontsize=8)
    fig.suptitle('High-speed tau sensitivity — same acceleration and jerk limits for every curve\n'+settings)
    fig.savefig(args.output/'tau_sensitivity.png',dpi=160)
    print(json.dumps(summary,indent=2))

if __name__=='__main__': main()
