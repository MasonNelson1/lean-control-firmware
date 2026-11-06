"""Analyze run 2 using recorded phone timestamps; writes only under logs/run2/."""
from pathlib import Path
import os, zipfile, json
os.environ.setdefault('MPLCONFIGDIR', '/tmp/lean-matplotlib')
import numpy as np
import pandas as pd
from scipy.ndimage import gaussian_filter1d
from scipy.optimize import differential_evolution
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'run2'
OUT.mkdir(exist_ok=True)
s = pd.read_csv(ROOT / 'run2Wheelchair.csv')
st = (s.time_ms.to_numpy() - s.time_ms.iloc[0]) / 1000
streams = {}
with zipfile.ZipFile(ROOT / 'run2Phone.zip') as z:
    for name in ['Accelerometer', 'Gravity', 'Gyroscope', 'Magnetometer']:
        streams[name] = pd.read_csv(z.open(name + '.csv'))
    metadata = pd.read_csv(z.open('meta/time.csv'))
grav = streams['Gravity']
pt = grav.iloc[:, 0].to_numpy()
g = grav.iloc[:, 1:].to_numpy()
assert np.all(np.diff(st) > 0)
for frame in streams.values():
    assert np.all(np.diff(frame.iloc[:, 0]) > 0)
# Phone is approximately upright (+Y along initial gravity).
fb = -np.degrees(np.arctan2(g[:, 2], g[:, 1]))
lr = np.degrees(np.arctan2(g[:, 0], np.hypot(g[:, 1], g[:, 2])))
neutral = pt < pt[0] + 1
fb_zero, lr_zero = np.median(fb[neutral]), np.median(lr[neutral])
sigma = .05 / np.median(np.diff(pt))
fb = gaussian_filter1d(fb - fb_zero, sigma)
lr = gaussian_filter1d(lr - lr_zero, sigma)
t = st[::10]
commands = [s.speed.to_numpy()[::10], s.turn.to_numpy()[::10]]
def corrs(offset, scale=1., window=None):
    it = pt * scale + offset
    mask = (t >= it[0]) & (t <= it[-1])
    if window:
        mask &= (t >= window[0]) & (t < window[1])
    if mask.sum() < 100:
        return np.array([0., 0.])
    return np.array([np.corrcoef(np.interp(t[mask], it, a), c[mask])[0, 1]
                     for a,c in zip([fb, lr], commands)])
# Search start offset only. Preserve the measured phone time scale.
fit = differential_evolution(lambda p: -np.abs(corrs(p[0])).mean(),
                             [(-10, 10)], seed=13, tol=1e-10)
offset = float(fit.x[0])
c = corrs(offset)
signs = np.where(c >= 0, 1, -1)
fb *= signs[0]; lr *= signs[1]
# Scale fit is diagnostic only; never applied to timestamps in the output.
drift_fit = differential_evolution(lambda p: -np.abs(corrs(p[0], p[1])).mean(),
                                   [(-10, 10), (.98, 1.02)], seed=13, tol=1e-9)
it = pt + offset
aligned = s.copy()
aligned.insert(0, 'time_since_serial_start_s', st)
stats = {}
for name, frame in streams.items():
    raw_t = frame.iloc[:, 0].to_numpy()
    at = raw_t + offset
    right = np.clip(np.searchsorted(at, st), 0, len(at)-1)
    left = np.maximum(right - 1, 0)
    nearest = np.where(abs(st-at[left]) <= abs(st-at[right]), left, right)
    valid = (st >= at[0]) & (st <= at[-1])
    prefix = name.lower() + '.'
    aligned[prefix + 'sample_index'] = np.where(valid, nearest, np.nan)
    aligned[prefix + 'aligned_time_s'] = np.where(valid, at[nearest], np.nan)
    aligned[prefix + 'time_difference_s'] = np.where(valid, at[nearest]-st, np.nan)
    for column in frame.columns:
        aligned[prefix + column] = np.where(valid, frame[column].to_numpy()[nearest], np.nan)
    raw_out = frame.copy()
    raw_out.insert(0, 'sample_index', np.arange(len(frame)))
    raw_out.insert(1, 'aligned_time_s', at)
    raw_out.to_csv(OUT / (name.lower() + '_raw_aligned.csv'), index=False)
    intervals = np.diff(raw_t)
    stats[name] = dict(rows=len(frame), first_timestamp_s=float(raw_t[0]),
                      last_timestamp_s=float(raw_t[-1]), median_interval_ms=float(np.median(intervals)*1000),
                      min_interval_ms=float(intervals.min()*1000), max_interval_ms=float(intervals.max()*1000),
                      rate_hz=float(1/np.median(intervals)), unmatched_serial_rows=int((~valid).sum()))
aligned['forward_lean_deg'] = np.interp(st, it, fb, left=np.nan, right=np.nan)
aligned['right_lean_deg'] = np.interp(st, it, lr, left=np.nan, right=np.nan)
aligned.to_csv(OUT / 'aligned_data.csv', index=False)
pd.DataFrame({'phone_time_s':pt,'aligned_time_s':it,'forward_lean_deg':fb,'right_lean_deg':lr}).to_csv(OUT / 'phone_angles.csv', index=False)
# Validate serialized raw measurements against their exact source samples.
check = pd.read_csv(OUT / 'aligned_data.csv')
for name, frame in streams.items():
    prefix = name.lower() + '.'
    valid = check[prefix+'sample_index'].notna()
    ix = check.loc[valid,prefix+'sample_index'].astype(int).to_numpy()
    cols = [prefix+col for col in frame.columns]
    np.testing.assert_allclose(check.loc[valid,cols].to_numpy(),frame.iloc[ix].to_numpy(),rtol=1e-14,atol=1e-14)
    raw_check = pd.read_csv(OUT / (name.lower()+'_raw_aligned.csv'))
    np.testing.assert_allclose(raw_check[frame.columns],frame,rtol=1e-14,atol=1e-14)
assert len(check) == len(s)
fig, axes = plt.subplots(3,1,figsize=(15,10),sharex=True,layout='constrained')
for ax, angle, command, title, color in [
    (axes[0],fb,s.speed,'Forward / backward chest lean and speed command','#146db0'),
    (axes[1],lr,s.turn,'Right / left chest lean and turn command','#c46013')]:
    ax.plot(it,angle,color=color,lw=1.4,label='Chest lean (degrees)')
    ax.set_ylabel('Lean (degrees)',color=color)
    ax.axhline(0,color='black',lw=.6);ax.grid(alpha=.2);ax.set_title(title,loc='left')
    twin=ax.twinx();twin.plot(st,command,color='#555555',alpha=.6,lw=1,label='Normalized command')
    twin.set_ylabel('Command (−1 to +1)');twin.set_ylim(-1.15,1.15)
    h,l=ax.get_legend_handles_labels();h2,l2=twin.get_legend_handles_labels();ax.legend(h+h2,l+l2,loc='upper right')
axes[2].plot(st,s.speed,label='Speed (+ forward)',lw=1)
axes[2].plot(st,s.turn,label='Turn (+ right)',lw=1)
axes[2].set_ylabel('Normalized command');axes[2].set_xlabel('Time since first retained wheelchair sample (s)')
axes[2].set_title('Wheelchair commands — not measured velocity',loc='left');axes[2].grid(alpha=.2);axes[2].legend()
axes[2].set_xlim(min(0,it[0]),max(st[-1],it[-1]))
fig.suptitle(f'Run 2: timestamp-based alignment | phone offset {offset:+.3f} s\nForward and right positive; initial chest posture is zero',fontsize=14)
fig.savefig(OUT / 'lean_commands.png',dpi=170);fig.savefig(OUT / 'lean_commands.svg')
axes[2].set_xlim(0,17)
fig.savefig(OUT / 'initial_movements.png',dpi=170)
summary = dict(serial_rows=len(s),serial_duration_s=float(st[-1]),
               serial_median_interval_ms=float(np.median(np.diff(st))*1000),
               phone_offset_s=offset, alignment_equation='serial_relative_time_s = phone_time_s + phone_offset_s',
               forward_correlation=float(abs(c[0])),right_correlation=float(abs(c[1])),
               forward_sign=int(signs[0]),right_sign=int(signs[1]),
               early_correlations=np.abs(corrs(offset,window=(0,32))).tolist(),
               late_correlations=np.abs(corrs(offset,window=(32,65))).tolist(),
               diagnostic_scale_fit=dict(offset_s=float(drift_fit.x[0]),scale=float(drift_fit.x[1]),
                                         mean_abs_correlation=float(-drift_fit.fun),applied=False),
               forward_range_deg=[float(fb.min()),float(fb.max())],right_range_deg=[float(lr.min()),float(lr.max())],
               phone_streams=stats,phone_recording_events=metadata.to_dict(orient='records'))
(OUT / 'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
