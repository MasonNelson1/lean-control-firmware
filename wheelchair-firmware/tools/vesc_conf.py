"""Decode VESC mcconf/appconf dumps (firmware 6.06) from the tap log.

The Teensy dumps raw COMM_GET_MCCONF / COMM_GET_APPCONF replies as
  DBG VCONF id=<vesc> cmd=<14|17> len=<n> off=<o> <hex>
lines. This reassembles them and decodes every field using the field order in
tools/vesc/confgenerator_6_06.c (copied from vedderb/bldc release_6_06).

usage: python3 tools/vesc_conf.py [log.csv] [--all] [field ...]
"""
import re, struct, sys, os, math
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'vesc', 'confgenerator_6_06.c')
SIG = {'mcconf': 788332866, 'appconf': 2099347128}
CMD = {14: 'mcconf', 17: 'appconf'}  # COMM_GET_MCCONF=14, COMM_GET_APPCONF=17

def layout(kind):
    src = open(SRC).read()
    body = re.search(r'int32_t confgenerator_serialize_%s\(.*?\{(.*?)\n\}' % kind, src, re.S).group(1)
    out = []
    for line in body.splitlines():
        line = line.strip()
        m = re.match(r'buffer\[ind\+\+\] = (?:\(\w+\))?conf->([\w\.\[\]]+);', line)
        if m: out.append((m.group(1), 'u8', None)); continue
        m = re.match(r'buffer_append_(\w+)\(buffer, (?:\(\w+\))?(?:conf->)?([\w\.\[\]]+), (?:([\d\.e\-]+), )?&ind\);', line)
        if m:
            t, name, scale = m.groups()
            out.append((name, t, float(scale) if scale else None))
    return out

def f32_auto(b):
    res = struct.unpack('>I', b)[0]
    e = (res >> 23) & 0xFF; sig_i = res & 0x7FFFFF; neg = res & (1 << 31)
    if e == 0 and sig_i == 0: return 0.0
    sig = sig_i / (8388608.0 * 2.0) + 0.5
    v = math.ldexp(sig, e - 126)
    return -v if neg else v

def decode(kind, data):
    ind = 0; vals = {}
    for name, t, scale in layout(kind):
        if t == 'u8': v = data[ind]; ind += 1
        elif t == 'uint16': v = struct.unpack('>H', data[ind:ind+2])[0]; ind += 2
        elif t == 'int16': v = struct.unpack('>h', data[ind:ind+2])[0]; ind += 2
        elif t == 'uint32': v = struct.unpack('>I', data[ind:ind+4])[0]; ind += 4
        elif t == 'int32': v = struct.unpack('>i', data[ind:ind+4])[0]; ind += 4
        elif t == 'float16': v = struct.unpack('>h', data[ind:ind+2])[0] / scale; ind += 2
        elif t == 'float32': v = struct.unpack('>i', data[ind:ind+4])[0] / scale; ind += 4
        elif t == 'float32_auto': v = f32_auto(data[ind:ind+4]); ind += 4
        else: raise ValueError(t)
        vals[name] = v
    return vals, ind

def dumps(path):
    bufs = {}
    for line in open(path, errors='replace'):
        m = re.search(r'DBG VCONF id=(\d+) cmd=(\d+) len=(\d+) off=(\d+) ([0-9a-f]+)', line)
        if not m: continue
        vid, cmd, n, off, hx = int(m[1]), int(m[2]), int(m[3]), int(m[4]), m[5]
        key = (vid, cmd)
        if off == 0 or key not in bufs or bufs[key][0] != n: bufs[key] = [n, bytearray(n), set()]
        b = bufs[key]; chunk = bytes.fromhex(hx); b[1][off:off+len(chunk)] = chunk; b[2].update(range(off, off+len(chunk)))
    return bufs

WATCH = ['s_pid_min_erpm', 's_pid_kp', 's_pid_ki', 's_pid_kd', 's_pid_ramp_erpms_s', 's_pid_allow_braking', 'l_current_max', 'l_current_min',
         'l_min_erpm', 'l_max_erpm', 'l_erpm_start', 'si_motor_poles', 'si_gear_ratio', 'si_wheel_diameter', 'motor_type',
         'controller_id', 'timeout_msec', 'timeout_brake_current', 'can_status_rate_1', 'can_status_rate_2',
         'can_status_msgs_r1', 'can_status_msgs_r2', 'can_baud_rate', 'app_to_use', 'can_mode']

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    path = args[0] if args and args[0].endswith('.csv') else os.path.join(HERE, '..', 'logs', 'live', 'current.csv')
    fields = [a for a in args if not a.endswith('.csv')] or WATCH
    for (vid, cmd), (n, data, got) in sorted(dumps(path).items()):
        kind = CMD.get(cmd)
        if not kind: continue
        if len(got) < n: print(f'id={vid} {kind}: incomplete {len(got)}/{n} bytes'); continue
        sig = struct.unpack('>I', data[1:5])[0]  # data[0] is the command byte
        vals, used = decode(kind, data[1:])
        print(f'== VESC id={vid} {kind}  {n} bytes  signature {"OK" if sig == SIG[kind] else "MISMATCH %d" % sig}  decoded {used}/{n-1}')
        for k, v in vals.items():
            if '--all' in sys.argv or k in fields:
                print(f'  {k:28s} {v:.6g}' if isinstance(v, float) else f'  {k:28s} {v}')
