"""Plot logged motor RPM targets against the aligned chest angles."""
from pathlib import Path
import os
os.environ.setdefault('MPLCONFIGDIR', '/tmp/lean-matplotlib')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd

ROOT = Path(__file__).resolve().parent
d = pd.read_csv(ROOT / 'aligned_data.csv')
t = d.time_since_serial_start_s
fig, axes = plt.subplots(3, 1, figsize=(15, 9), sharex=True, layout='constrained')
for col, label, color in [('leftTargetRPM', 'Left motor target', '#146db0'),
                          ('rightTargetRPM', 'Right motor target', '#c46013')]:
    axes[0].plot(t, d[col], label=label, color=color, lw=1.2)
axes[0].set_ylabel('Target RPM'); axes[0].legend(loc='upper right')
axes[1].plot(t, d.forward_lean_deg, color='#238b45', lw=1.3)
axes[1].set_ylabel('Forward / backward (°)')
axes[1].set_title('Forward positive; backward negative', loc='left', fontsize=10)
axes[2].plot(t, d.right_lean_deg, color='#9467bd', lw=1.3)
axes[2].set_ylabel('Right / left (°)')
axes[2].set_title('Right positive; left negative', loc='left', fontsize=10)
axes[2].set_xlabel('Time since first retained wheelchair sample (s)')
for ax in axes:
    ax.axhline(0, color='black', lw=.6); ax.grid(alpha=.2)
axes[2].set_xlim(t.iloc[0], t.iloc[-1])
fig.suptitle('Run 2: left and right motor RPM targets alongside chest lean\nLogged targets, not measured motor RPM; angles relative to initial posture', fontsize=14)
fig.savefig(ROOT / 'motor_rpms_vs_angles.png', dpi=170)
fig.savefig(ROOT / 'motor_rpms_vs_angles.svg')

fig, axes = plt.subplots(2, 2, figsize=(12, 9), layout='constrained', sharey=True)
for row, (rpm, motor, color) in enumerate([('leftTargetRPM','Left','#146db0'),('rightTargetRPM','Right','#c46013')]):
    for col, (angle, label) in enumerate([('forward_lean_deg','Forward / backward angle (°; + forward)'),('right_lean_deg','Right / left angle (°; + right)')]):
        ax=axes[row,col]
        ax.scatter(d[angle], d[rpm], s=4, alpha=.12, color=color, rasterized=True)
        ax.set_xlabel(label); ax.set_ylabel(f'{motor} motor target RPM')
        ax.axhline(0,color='black',lw=.6);ax.axvline(0,color='black',lw=.6);ax.grid(alpha=.2)
fig.suptitle('Run 2: motor RPM targets versus chest angles\nEach point is one aligned wheelchair sample; both lean axes affect each motor',fontsize=14)
fig.savefig(ROOT / 'motor_rpms_angle_scatter.png',dpi=170)
print(d[['leftTargetRPM','rightTargetRPM']].agg(['min','max']).to_string())
