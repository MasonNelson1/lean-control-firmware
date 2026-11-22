'use strict';

// Read-only VESC panel: health cards on the dashboard and every setting each
// VESC reports on the VESC settings page. It only reads firmware lines and
// never sends anything to the chair or the VESCs (tests/vesc-panel.test.cjs
// checks that it has no way to).
//
// Lines (src/vesc_monitor.h, src/main.cpp):
//   DBG VESC   1 Hz per VESC: CAN status frames 1-5, age of each type, current peaks
//   DBG VVAL   1 Hz per VESC: COMM_GET_VALUES, including the fault code
//   DBG VFAULT whenever a VESC's fault code changes
//   DBG VESCFW firmware version reply
//   DBG VCONF  hex chunks of COMM_GET_MCCONF / COMM_GET_APPCONF replies, decoded
//              with VescConfLayout (vesc-conf-layout.js, generated from VESC 6.06)
const VescPanel = (() => {
  const staleMs = 500;      // status frames older than this: the VESC has gone quiet
  const silentMs = 3000;    // no DBG VESC line for this long: no data
  const faultHoldMs = 5000; // a fault keeps the card red at least this long
  const statusNames = ['S1', 'S2', 'S3', 'S4', 'S5'];
  const commands = {14: 'mcconf', 17: 'appconf'};
  const erpmPerWheelRpm = 14.5454 * 4; // VESC_ERPM_PER_WHEEL_RPM in src/vesc_can.h

  // key=value pairs after "DBG XXX"; null when garbled on the radio (a key
  // repeats because two lines ran together, or a value is empty).
  function parseKv(line, prefix) {
    if (!line.startsWith(prefix)) return null;
    const pairs = line.slice(prefix.length).trim().split(/\s+/).filter(t => t.includes('=')).map(t => {
      const at = t.indexOf('=');
      return [t.slice(0, at), t.slice(at + 1)];
    });
    const out = Object.fromEntries(pairs);
    if (Object.keys(out).length !== pairs.length || pairs.some(([, v]) => v === '')) return null;
    return out;
  }
  const number = v => (v === undefined ? undefined : Number(v));
  const pair = v => (v === undefined ? [undefined, undefined] : v.split('/').map(Number));
  const finite = v => typeof v === 'number' && Number.isFinite(v);

  function parseStatus(line) {
    const d = parseKv(line, 'DBG VESC ');
    if (!d || !/^\d+$/.test(d.id ?? '') || !finite(number(d.age))) return null;
    const [ah, ahc] = pair(d.ah), [wh, whc] = pair(d.wh);
    const ages = d.ages ? d.ages.split('/').map(Number) : null;
    if (ages && ages.length !== 5) return null;
    return {
      id: Number(d.id), side: d.side ?? null, age: number(d.age), frames: number(d.frames), erpm: number(d.erpm),
      current: number(d.current), duty: number(d.duty), fetC: number(d.fetC), vin: number(d.vin),
      motC: number(d.motC), iin: number(d.iin), ipk: number(d.ipk), iinpk: number(d.iinpk),
      ah, ahc, wh, whc, tach: number(d.tach), ages,
    };
  }

  function parseValues(line) {
    const d = parseKv(line, 'DBG VVAL ');
    if (!d || !/^\d+$/.test(d.id ?? '')) return null;
    const [asked, replies, lost] = pair(d.n);
    const [tach, tachAbs] = pair(d.tach);
    return {
      id: Number(d.id), side: d.side ?? null, age: number(d.age), fault: number(d.fault), fetC: number(d.fetC),
      motC: number(d.motC), imot: number(d.imot), iin: number(d.iin), duty: number(d.duty), erpm: number(d.erpm),
      vin: number(d.vin), tach, tachAbs, mos: d.mos ? d.mos.split('/').map(Number) : null,
      faults: number(d.faults), lastFault: number(d.last), asked, replies, lost, lat: number(d.lat),
      status: number(d.st),
    };
  }

  function parseFault(line) {
    const d = parseKv(line, 'DBG VFAULT ');
    if (!d || !/^\d+$/.test(d.id ?? '') || !/^\d+$/.test(d.code ?? '')) return null;
    return {
      id: Number(d.id), side: d.side ?? null, code: Number(d.code), name: d.name ?? `code ${d.code}`,
      ms: number(d.ms), imot: number(d.imot), iin: number(d.iin), erpm: number(d.erpm), duty: number(d.duty),
      vin: number(d.vin), fetC: number(d.fetC), n: number(d.n),
    };
  }

  function parseFirmware(line) {
    const m = /^DBG VESCFW id=(\d+) fw=(\d+\.\d+) hw=(\S*)/.exec(line);
    return m ? {id: Number(m[1]), fw: m[2], hw: m[3]} : null;
  }

  // One 24-byte chunk of a config reply. Rejects chunks whose hex length does
  // not match (garbled on the radio; the next 30 s cycle resends it).
  function parseConfChunk(line) {
    const m = /^DBG VCONF id=(\d+) cmd=(\d+) len=(\d+) off=(\d+) ([0-9a-f]+)$/.exec(line.trim());
    if (!m) return null;
    const [id, cmd, len, off] = m.slice(1, 5).map(Number);
    const hex = m[5];
    if (off >= len || hex.length !== 2 * Math.min(24, len - off)) return null;
    const bytes = new Uint8Array(hex.length / 2);
    for (let i = 0; i < bytes.length; ++i) bytes[i] = parseInt(hex.substr(2 * i, 2), 16);
    return {id, cmd, len, off, bytes};
  }

  // buffer_get_float32_auto (VESC buffer.c).
  function float32Auto(u32) {
    const e = (u32 >>> 23) & 0xff, sig = u32 & 0x7fffff, negative = (u32 >>> 31) !== 0;
    if (e === 0 && sig === 0) return 0;
    const value = (sig / (8388608 * 2) + 0.5) * 2 ** (e - 126);
    return negative ? -value : value;
  }

  // Decode a config (bytes without the command byte) with the generated layout.
  // Returns {fields: [[name, value]], used, signatureOk} or null if too short.
  function decodeConf(kind, bytes, layout = (typeof VescConfLayout !== 'undefined' ? VescConfLayout : null)) {
    const spec = layout?.[kind];
    if (!spec) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const fields = [];
    let i = 0;
    for (const [name, type, scale] of spec.fields) {
      const size = {u8: 1, uint16: 2, int16: 2, uint32: 4, int32: 4, float16: 2, float32: 4, float32_auto: 4}[type];
      if (i + size > bytes.length) return null;
      let value;
      switch (type) {
        case 'u8': value = view.getUint8(i); break;
        case 'uint16': value = view.getUint16(i); break;
        case 'int16': value = view.getInt16(i); break;
        case 'uint32': value = view.getUint32(i); break;
        case 'int32': value = view.getInt32(i); break;
        case 'float16': value = view.getInt16(i) / scale; break;
        case 'float32': value = view.getInt32(i) / scale; break;
        case 'float32_auto': value = float32Auto(view.getUint32(i)); break;
        default: return null;
      }
      fields.push([name, value]);
      i += size;
    }
    return {fields, used: i, signatureOk: fields[0]?.[1] === spec.signature};
  }

  function createState() {
    return {vescs: new Map(), changed: 0};
  }

  function entry(state, id) {
    if (!state.vescs.has(id)) {
      state.vescs.set(id, {
        id, side: null, status: null, statusAt: null, values: null, valuesAt: null, faults: [], firmware: null,
        conf: {}, // kind -> {len, bytes, got:Set, decoded, at}
      });
    }
    return state.vescs.get(id);
  }

  // Every firmware DBG line goes through here. Returns true when the panel changed.
  function handleLine(state, line, now) {
    let v;
    if ((v = parseStatus(line))) {
      const e = entry(state, v.id);
      e.status = v; e.statusAt = now; e.side = v.side ?? e.side;
    } else if ((v = parseValues(line))) {
      const e = entry(state, v.id);
      e.values = v; e.valuesAt = now; e.side = v.side ?? e.side;
    } else if ((v = parseFault(line))) {
      const e = entry(state, v.id);
      e.side = v.side ?? e.side;
      e.faults.push({...v, at: now});
      if (e.faults.length > 20) e.faults.shift();
    } else if ((v = parseFirmware(line))) {
      entry(state, v.id).firmware = v;
    } else if ((v = parseConfChunk(line))) {
      const kind = commands[v.cmd];
      if (!kind) return false;
      const e = entry(state, v.id);
      let c = e.conf[kind];
      // A new dump (different length, or a fresh start at offset 0) replaces the bytes
      // in place; the decoded table stays until the new one is complete.
      if (!c || c.len !== v.len) c = e.conf[kind] = {len: v.len, bytes: new Uint8Array(v.len), got: new Set(), decoded: null, at: null};
      if (v.off === 0) c.got.clear();
      c.bytes.set(v.bytes, v.off);
      for (let i = v.off; i < v.off + v.bytes.length; ++i) c.got.add(i);
      if (c.got.size === c.len) {
        const decoded = decodeConf(kind, c.bytes.subarray(1));
        if (decoded) { c.decoded = decoded; c.at = now; }
      }
    } else {
      return false;
    }
    state.changed++;
    return true;
  }

  // VESCs in display order: left, right, then any others by id.
  function ordered(state) {
    const rank = e => (e.side === 'L' ? 0 : e.side === 'R' ? 1 : 2);
    return [...state.vescs.values()].sort((a, b) => rank(a) - rank(b) || a.id - b.id);
  }

  function confValue(e, kind, name) {
    return e.conf[kind]?.decoded?.fields.find(([n]) => n === name)?.[1];
  }

  // What the health card shows for one VESC at time `now` (performance.now()).
  function health(e, now) {
    const s = e.status, v = e.values;
    const lineAge = e.statusAt === null ? Infinity : now - e.statusAt;
    const statusAge = s && s.age >= 0 && s.frames !== 0 ? s.age + lineAge : null;
    const lastFault = e.faults.filter(f => f.code !== 0).at(-1) ?? null;
    // A VESC holds a fault for 500 ms, so the 1 Hz VVAL can miss it; a DBG
    // VFAULT keeps the card red for faultHoldMs so nobody misses it either.
    const valuesFresh = v && now - e.valuesAt < silentMs && v.age >= 0 && v.age < 1000;
    const faultNow = (valuesFresh && v.fault > 0) || (lastFault !== null && now - lastFault.at < faultHoldMs);
    let state;
    if (!s || lineAge > silentMs) state = 'none';
    else if (statusAge === null || statusAge > staleMs) state = 'stale';
    else if (faultNow) state = 'fault';
    else state = 'ok';
    const fetLimit = confValue(e, 'mcconf', 'l_temp_fet_start');
    const motorLimit = confValue(e, 'mcconf', 'l_temp_motor_start');
    const cutStart = confValue(e, 'mcconf', 'l_battery_cut_start');
    return {
      id: e.id, side: e.side, state, statusAge, lineAge,
      fetC: s?.fetC, motC: s && s.motC > -50 ? s.motC : null, vin: s?.vin, current: s?.current, ipk: s?.ipk,
      iin: s?.iin, iinpk: s?.iinpk, duty: s?.duty, erpm: s?.erpm,
      wheelRpm: finite(s?.erpm) ? s.erpm / erpmPerWheelRpm : null,
      ah: s?.ah, ahc: s?.ahc, wh: s?.wh, whc: s?.whc, ages: s?.ages ?? null,
      hot: finite(fetLimit) && finite(s?.fetC) && s.fetC >= fetLimit - 10,
      motorHot: finite(motorLimit) && finite(s?.motC) && s.motC > -50 && s.motC >= motorLimit - 10,
      lowVoltage: finite(cutStart) && finite(s?.vin) && s.vin > 0 && s.vin <= cutStart,
      lastFault, faultNow,
      faultsSinceBoot: v?.faults ?? null, lastFaultCode: v?.lastFault ?? null,
      poll: v ? {asked: v.asked, replies: v.replies, lost: v.lost, lat: v.lat, age: v.age} : null,
    };
  }

  const esc = text => String(text).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
  const fixed = (v, digits = 1) => (finite(v) ? v.toFixed(digits) : '—');
  function since(ms) {
    if (!finite(ms)) return '';
    const s = ms / 1000;
    return s < 60 ? `${Math.round(s)} s ago` : s < 3600 ? `${Math.round(s / 60)} min ago` : `${(s / 3600).toFixed(1)} h ago`;
  }
  const sideName = side => (side === 'L' ? 'Left' : side === 'R' ? 'Right' : 'VESC');
  const stateText = {ok: 'OK', stale: 'Status frames stale', fault: 'FAULT', none: 'No data'};
  // Hand-written fault names for the ones this chair can plausibly hit.
  const faultHelp = {
    ABS_OVER_CURRENT: 'phase current passed l_abs_current_max',
    OVER_VOLTAGE: 'input voltage above l_max_vin',
    UNDER_VOLTAGE: 'input voltage below l_min_vin',
    DRV: 'gate driver fault',
    OVER_TEMP_FET: 'MOSFETs too hot',
    OVER_TEMP_MOTOR: 'motor too hot',
  };

  function healthCard(h, now) {
    const metric = (label, value, unit, extra = '', warn = false) =>
      `<div class="vesc-metric${warn ? ' warn' : ''}"><span>${label}</span><strong>${value}${value === '—' ? '' : unit}</strong>${extra ? `<em>${extra}</em>` : ''}</div>`;
    const ages = h.ages ? h.ages.map((a, i) => `${statusNames[i]} ${a < 0 ? '—' : Math.round(a)}`).join(' · ') : 'per-type ages need newer firmware';
    let fault;
    if (h.lastFault) {
      const help = faultHelp[h.lastFault.name];
      fault = `<strong>${esc(h.lastFault.name)}</strong> ${since(now - h.lastFault.at)}${help ? ` · ${esc(help)}` : ''}` +
        ` · motor ${fixed(h.lastFault.imot, 1)} A, input ${fixed(h.lastFault.iin, 1)} A, ${fixed(h.lastFault.erpm, 0)} erpm`;
    } else if (h.lastFaultCode > 0) {
      fault = `code ${h.lastFaultCode} before this connection (${h.faultsSinceBoot} since the Teensy started)`;
    } else if (h.faultsSinceBoot === 0) {
      fault = 'none since the Teensy started';
    } else {
      fault = h.poll ? 'none seen' : 'waiting for the first GET_VALUES reply';
    }
    const poll = h.poll && finite(h.poll.asked)
      ? `values polls ${h.poll.replies}/${h.poll.asked} answered, ${h.poll.lost} lost, slowest ${fixed(h.poll.lat, 0)} ms`
      : 'values poll: no reply yet';
    return `<article class="vesc-card ${h.state}" data-id="${h.id}">
  <header><div><h3>${sideName(h.side)} · VESC ${h.id}</h3><div class="sub">status frames ${h.statusAge === null ? 'never received' : `${Math.round(h.statusAge)} ms old`}</div></div>
  <span class="vesc-state ${h.state}">${stateText[h.state]}</span></header>
  <div class="vesc-metrics">
    ${metric('MOSFET', fixed(h.fetC), ' °C', '', h.hot)}
    ${metric('Motor', fixed(h.motC), ' °C', h.motC === null ? 'no sensor reading' : '', h.motorHot)}
    ${metric('Input', fixed(h.vin), ' V', '', h.lowVoltage)}
    ${metric('Motor current', fixed(h.current), ' A', `peak ${fixed(h.ipk)} A`)}
    ${metric('Input current', fixed(h.iin), ' A', `peak ${fixed(h.iinpk)} A`)}
    ${metric('Duty', fixed(h.duty, 3), '', `${fixed(h.wheelRpm, 0)} wheel RPM`)}
  </div>
  <div class="vesc-line ${h.faultNow ? 'warn' : ''}"><span>Last fault</span>${fault}</div>
  <div class="vesc-line"><span>Frames</span>${ages}</div>
  <div class="vesc-line"><span>Energy</span>${fixed(h.ah, 3)} Ah · ${fixed(h.wh, 2)} Wh used, ${fixed(h.ahc, 3)} Ah regenerated · ${poll}</div>
</article>`;
  }

  function renderHealth(state, now) {
    const list = ordered(state);
    if (!list.length) return '<p class="sub vesc-empty">Waiting for DBG VESC lines from the chair (once a second per VESC).</p>';
    return list.map(e => healthCard(health(e, now), now)).join('\n');
  }

  function formatValue(v) {
    if (!finite(v)) return String(v);
    if (Number.isInteger(v)) return String(v);
    return String(Number(v.toPrecision(6)));
  }

  // The settings table: one column per VESC, mcconf then appconf.
  function renderDetails(state, {filter = '', onlyDifferent = false} = {}, now = 0) {
    const list = ordered(state);
    const needle = filter.trim().toLowerCase();
    const head = list.map(e => {
      const mc = e.conf.mcconf, app = e.conf.appconf;
      const part = (c, total) => (c ? (c.decoded ? `${c.len - 1} bytes${c.decoded.signatureOk ? '' : ', SIGNATURE MISMATCH'}` : `${c.got.size}/${c.len} bytes`) : `waiting (${total})`);
      const fw = e.firmware ? `fw ${esc(e.firmware.fw)} · ${esc(e.firmware.hw)}` : 'firmware not reported yet';
      return `<th>${sideName(e.side)} · VESC ${e.id}<div class="sub">${fw}<br>motor ${part(mc, 'mcconf')} · app ${part(app, 'appconf')}</div></th>`;
    }).join('');
    const rows = [];
    for (const kind of ['mcconf', 'appconf']) {
      const names = [];
      for (const e of list) for (const [n] of e.conf[kind]?.decoded?.fields ?? []) if (!names.includes(n)) names.push(n);
      if (!names.length) continue;
      const body = [];
      for (const name of names) {
        if (needle && !name.toLowerCase().includes(needle)) continue;
        const values = list.map(e => confValue(e, kind, name));
        const known = values.filter(v => v !== undefined).map(formatValue);
        const differs = new Set(known).size > 1;
        if (onlyDifferent && !differs) continue;
        body.push(`<tr${differs ? ' class="differs"' : ''}><td>${esc(name)}</td>${values.map(v => `<td>${v === undefined ? '—' : esc(formatValue(v))}</td>`).join('')}</tr>`);
      }
      rows.push(`<tr class="group"><th colspan="${list.length + 1}">${kind === 'mcconf' ? 'Motor configuration (mcconf)' : 'App configuration (appconf)'} · ${body.length} shown</th></tr>`, ...body);
    }
    if (!list.length) return '<p class="sub">No VESC has reported yet.</p>';
    const note = rows.length ? '' : '<p class="sub">The firmware streams each VESC\'s settings every 30 s (DBG VCONF); they appear here once a dump is complete.</p>';
    return `${note}<table class="vesc-settings"><thead><tr><th>Setting</th>${head}</tr></thead><tbody>${rows.join('\n')}</tbody></table>`;
  }

  return {
    staleMs, silentMs, parseKv, parseStatus, parseValues, parseFault, parseFirmware, parseConfChunk,
    float32Auto, decodeConf, createState, handleLine, ordered, health, renderHealth, renderDetails, formatValue,
  };
})();
