'use strict';

// Seat sensor layout and the live centre-of-pressure plate drawing.
//
// The firmware's `Sensor layout` setting decides what the four channels F/B/L/R
// are. DBG SENSORS carries `layout=PLUS|CORNER` once a second; DBG CORNER
// (1 Hz) carries the calibrated neutral, the relaxed posture and the boxes where
// output is zero and where it is full; DBG COP (20 Hz) carries the centre of
// pressure. Firmware without these lines leaves the dashboard on PLUS.
const PlateView = (() => {
  // Plate inches from the old plus centre, x to the rider's right, y forward.
  // CORNER is a copy of lean::CornerPositions in src/driveModes/lean_layout.h;
  // tests/plate-view.test.cjs checks the two agree. PLUS is config F5 B6 L5 R5
  // (holes 2.5" out then 1" apart). `area` places each tile in the 3×3 grid.
  const layouts = Object.freeze({
    PLUS: Object.freeze({
      name: 'Plus layout',
      channels: {
        F: {label: 'Front', x: 0, y: 6.5, area: '1 / 2'},
        B: {label: 'Back', x: 0, y: -7.5, area: '3 / 2'},
        L: {label: 'Left', x: -6.5, y: 0, area: '2 / 1'},
        R: {label: 'Right', x: 6.5, y: 0, area: '2 / 3'},
      },
    }),
    CORNER: Object.freeze({
      name: 'Corner layout',
      channels: {
        F: {label: 'Back-left (F)', x: -5.75, y: -4.1875, area: '3 / 1'},
        B: {label: 'Back-right (B)', x: 5.3125, y: -4.25, area: '3 / 3'},
        L: {label: 'Front-left (L)', x: -5.75, y: 3.0, area: '1 / 1'},
        R: {label: 'Front-right (R)', x: 5.375, y: 2.9375, area: '1 / 3'},
      },
    }),
  });

  // Top plate outline measured 2026-11-22 (experiments/E9-E10-rectangle.html).
  const plateOutline = [[-7.75, 8.375], [7.5, 8.375], [7.5, -3.625], [4.81, -8.25], [-5.06, -8.25], [-7.75, -3.625]];

  const trailMs = 2000;   // CoP history drawn behind the dot
  const staleMs = 1500;   // no DBG COP for this long: the dot greys out

  // 'nan' and anything unparsable become null (unknown), never zero.
  function number(text) {
    if (text === undefined || /^[+-]?nan$/i.test(text)) return null;
    const value = Number(text);
    return Number.isFinite(value) ? value : null;
  }
  function numbers(text, count) {
    const parts = (text ?? '').split('/');
    if (parts.length !== count) return null;
    const values = parts.map(number);
    return values.every(value => value !== null) ? values : null;
  }

  function parseLayout(line) {
    const match = /^DBG SENSORS .* layout=(PLUS|CORNER)\b/.exec(line.trim());
    return match ? match[1] : null;
  }

  function parseCorner(line) {
    const text = line.trim();
    if (!text.startsWith('DBG CORNER ')) return null;
    const fields = Object.fromEntries(text.slice(11).split(' ').map(part => part.split('=')));
    const point = values => values && {x: values[0], y: values[1]};
    const box = values => values && {left: values[0], right: values[1], back: values[2], front: values[3]};
    return {
      neutral: point(numbers(fields.neutral, 2)),
      passive: point(numbers(fields.passive, 2)),
      dead: box(numbers(fields.dead, 4)),
      full: box(numbers(fields.full, 4)),
      reachCalibrated: fields.reachCal === '1',
      empty: numbers(fields.empty, 4),
      autoZeroed: fields.auto === '1',
    };
  }

  function parseCop(line) {
    const match = /^DBG COP (\d+) (\S+) (\S+) (\S+) (\S+) (\S+)$/.exec(line.trim());
    if (!match) return null;
    const [timeMs, x, y, dx, dy, total] = match.slice(1).map(number);
    if (timeMs === null) return null;
    return {timeMs, x, y, dx, dy, total};
  }

  function createState() {
    return {layout: null, corner: null, trail: [], copAt: -Infinity};
  }

  // Feeds one firmware line. Returns true when the plate status changed.
  function handleLine(state, line, now) {
    const layout = parseLayout(line);
    if (layout) {
      const changed = layout !== state.layout;
      state.layout = layout;
      if (changed && layout !== 'CORNER') { state.corner = null; state.trail = []; }
      return changed;
    }
    const corner = parseCorner(line);
    if (corner) {
      state.corner = corner;
      return true;
    }
    const cop = parseCop(line);
    if (cop) {
      // A firmware reboot restarts its clock: drop the old trail.
      if (state.trail.length && cop.timeMs < state.trail.at(-1).timeMs) state.trail = [];
      if (cop.x !== null && cop.y !== null) state.trail.push(cop);
      while (state.trail.length && cop.timeMs - state.trail[0].timeMs > trailMs) state.trail.shift();
      state.latest = cop;
      state.copAt = now;
      return true;
    }
    return false;
  }

  function layoutOf(state) {
    return layouts[state?.layout] ?? layouts.PLUS;
  }

  // Plate inches → canvas pixels, fitting the plate with the front at the top.
  function projection(w, h) {
    const spanX = 17, spanY = 18;
    const scale = Math.max(0, Math.min(w / spanX, h / spanY));
    return {scale, X: x => w / 2 + x * scale, Y: y => h / 2 - y * scale};
  }

  // Draws the corner layout's plate: outline, sensors sized by their share of
  // the load, the full-lean box, the no-output box, neutral, relaxed posture
  // and the CoP with a short trail. `row` is the latest telemetry row (F/B/L/R).
  function draw(g, w, h, state, row, now) {
    const {scale, X, Y} = projection(w, h);
    const layout = layoutOf(state);
    g.fillStyle = '#fff';
    g.fillRect(0, 0, w, h);

    g.beginPath();
    plateOutline.forEach(([x, y], i) => i ? g.lineTo(X(x), Y(y)) : g.moveTo(X(x), Y(y)));
    g.closePath();
    g.fillStyle = '#f7fafc';
    g.fill();
    g.strokeStyle = '#c9d4de';
    g.lineWidth = 1.5;
    g.stroke();

    const corner = state?.corner;
    const rect = (box, fill, stroke, dash) => {
      g.fillStyle = fill;
      g.fillRect(X(box.left), Y(box.front), (box.right - box.left) * scale, (box.front - box.back) * scale);
      g.setLineDash(dash);
      g.strokeStyle = stroke;
      g.lineWidth = 1;
      g.strokeRect(X(box.left), Y(box.front), (box.right - box.left) * scale, (box.front - box.back) * scale);
      g.setLineDash([]);
    };
    if (corner?.full) rect(corner.full, 'rgba(40,125,156,0.07)', '#8fbccd', []);
    if (corner?.dead) rect(corner.dead, 'rgba(98,112,128,0.35)', '#5d6b7a', [4, 3]);

    // Sensors: dashed outline joining the corners, dots sized by load share.
    const keys = ['F', 'B', 'L', 'R'];
    const loads = keys.map(key => Math.max(0, Number(row?.[key]) || 0));
    const total = loads.reduce((a, b) => a + b, 0);
    if (state?.layout === 'CORNER') {
      const order = ['L', 'R', 'B', 'F'].map(key => layout.channels[key]);
      g.beginPath();
      order.forEach((c, i) => i ? g.lineTo(X(c.x), Y(c.y)) : g.moveTo(X(c.x), Y(c.y)));
      g.closePath();
      g.setLineDash([3, 4]);
      g.strokeStyle = '#b6c3cf';
      g.stroke();
      g.setLineDash([]);
    }
    g.font = '10px system-ui';
    keys.forEach((key, i) => {
      const c = layout.channels[key];
      const share = total > 0 ? loads[i] / total : 0;
      g.fillStyle = 'rgba(75,143,184,0.85)';
      g.beginPath();
      g.arc(X(c.x), Y(c.y), 4 + 12 * share, 0, Math.PI * 2);
      g.fill();
      g.fillStyle = '#43566a';
      g.textAlign = c.x < 0 ? 'right' : 'left';
      g.fillText(key, X(c.x) + (c.x < 0 ? -18 : 18), Y(c.y) + 4);
    });

    // Neutral (+) and relaxed posture (○).
    g.strokeStyle = '#43566a';
    g.lineWidth = 1.5;
    if (corner?.neutral) {
      const x = X(corner.neutral.x), y = Y(corner.neutral.y);
      g.beginPath();
      g.moveTo(x - 6, y); g.lineTo(x + 6, y);
      g.moveTo(x, y - 6); g.lineTo(x, y + 6);
      g.stroke();
    }
    if (corner?.passive) {
      g.beginPath();
      g.arc(X(corner.passive.x), Y(corner.passive.y), 5, 0, Math.PI * 2);
      g.stroke();
    }

    // Centre of pressure: fading trail, then the current dot (grey when stale).
    const trail = state?.trail ?? [];
    const fresh = now - (state?.copAt ?? -Infinity) <= staleMs;
    if (trail.length > 1) {
      g.strokeStyle = fresh ? 'rgba(40,125,156,0.35)' : 'rgba(132,146,163,0.35)';
      g.lineWidth = 2;
      g.beginPath();
      trail.forEach((p, i) => i ? g.lineTo(X(p.x), Y(p.y)) : g.moveTo(X(p.x), Y(p.y)));
      g.stroke();
    }
    const dot = trail.at(-1);
    if (dot && state?.latest?.x !== null) {
      g.fillStyle = fresh ? '#287d9c' : '#a7b4c1';
      g.beginPath();
      g.arc(X(dot.x), Y(dot.y), 6, 0, Math.PI * 2);
      g.fill();
    }

    g.fillStyle = '#8492a3';
    g.textAlign = 'center';
    g.fillText('FRONT ▲', w / 2, 12);
    if (!corner?.neutral) g.fillText('Calibrate in LEAN to show the neutral zone', w / 2, h - 6);
  }

  return {layouts, plateOutline, parseLayout, parseCorner, parseCop, createState, handleLine, layoutOf, projection, draw};
})();
