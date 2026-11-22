const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const context = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'plate-view.js'), 'utf8'), context);
const PlateView = vm.runInContext('PlateView', context);

const sensorsLine = layout =>
  `DBG SENSORS raw=927/756/1058/1390 floors=1038/864/657/790 W=837 seatLoad=2531 seated=1 calibrated=1 calActive=0 railed=0${layout ? ' layout=' + layout : ''}`;
const cornerLine = 'DBG CORNER neutral=-0.30/-1.63 passive=-0.15/-0.90 dead=-0.55/-0.05/-1.78/-0.75 ' +
  'full=-2.00/1.30/-3.53/1.25 reachCal=1 empty=383/396/420/404 auto=1';

test('corner positions match the firmware (lean::CornerPositions)', () => {
  const header = fs.readFileSync(path.join(__dirname, '..', '..', 'src', 'driveModes', 'lean_layout.h'), 'utf8');
  const block = /CornerPositions = \{\{([\s\S]*?)\}\};/.exec(header)[1];
  const firmware = [...block.matchAll(/\{(-?[\d.]+)f, (-?[\d.]+)f\},\s*\/\/ (\w) channel: ([\w-]+) corner/g)];
  assert.equal(firmware.length, 4);
  for (const [, x, y, channel, corner] of firmware) {
    const shown = PlateView.layouts.CORNER.channels[channel];
    assert.equal(shown.x, Number(x), channel);
    assert.equal(shown.y, Number(y), channel);
    assert.ok(shown.label.toLowerCase().startsWith(corner), `${channel} is ${corner}`);
  }
});

test('corner tiles sit at the corner they measure; plus tiles keep the plus', () => {
  const c = PlateView.layouts.CORNER.channels, p = PlateView.layouts.PLUS.channels;
  assert.deepEqual([c.L.area, c.R.area, c.F.area, c.B.area], ['1 / 1', '1 / 3', '3 / 1', '3 / 3']);
  assert.deepEqual([p.F.area, p.B.area, p.L.area, p.R.area], ['1 / 2', '3 / 2', '2 / 1', '2 / 3']);
  assert.deepEqual([p.F.label, p.B.label, p.L.label, p.R.label], ['Front', 'Back', 'Left', 'Right']);
  for (const [key, ch] of Object.entries(c)) assert.equal(Math.sign(ch.y) > 0, ['L', 'R'].includes(key));
});

test('parses the layout from DBG SENSORS; older firmware reports none', () => {
  assert.equal(PlateView.parseLayout(sensorsLine('CORNER')), 'CORNER');
  assert.equal(PlateView.parseLayout(sensorsLine('PLUS')), 'PLUS');
  assert.equal(PlateView.parseLayout(sensorsLine(null)), null);
  assert.equal(PlateView.parseLayout(sensorsLine('SQUARE')), null);
  assert.equal(PlateView.parseLayout('DBG R 1 2 3 4 5'), null);
});

test('parses DBG CORNER, with nan as unknown', () => {
  const corner = PlateView.parseCorner(cornerLine);
  assert.deepEqual({...corner.neutral}, {x: -0.3, y: -1.63});
  assert.deepEqual({...corner.passive}, {x: -0.15, y: -0.9});
  assert.deepEqual({...corner.dead}, {left: -0.55, right: -0.05, back: -1.78, front: -0.75});
  assert.deepEqual({...corner.full}, {left: -2, right: 1.3, back: -3.53, front: 1.25});
  assert.deepEqual([...corner.empty], [383, 396, 420, 404]);
  assert.equal(corner.reachCalibrated, true);
  assert.equal(corner.autoZeroed, true);
  const fresh = PlateView.parseCorner('DBG CORNER neutral=nan/nan passive=nan/nan dead=nan/nan/nan/nan ' +
    'full=nan/nan/nan/nan reachCal=0 empty=391/406/427/400 auto=0');
  assert.equal(fresh.neutral, null);
  assert.equal(fresh.passive, null);
  assert.equal(fresh.dead, null);
  assert.equal(fresh.full, null);
  assert.equal(fresh.autoZeroed, false);
  assert.equal(PlateView.parseCorner('DBG CORNERS x'), null);
});

test('parses DBG COP, with nan as unknown', () => {
  assert.deepEqual({...PlateView.parseCop('DBG COP 1284 0.19 0.43 0.49 2.06 2507')},
    {timeMs: 1284, x: 0.19, y: 0.43, dx: 0.49, dy: 2.06, total: 2507});
  const unlocated = PlateView.parseCop('DBG COP 1334 nan nan nan nan 0');
  assert.equal(unlocated.x, null);
  assert.equal(unlocated.total, 0);
  assert.equal(PlateView.parseCop('DBG COP 1 2 3'), null);
  assert.equal(PlateView.parseCop('DBG COP x 1 2 3 4 5'), null);
});

test('state keeps a 2-second trail, drops it on a firmware reboot, and resets off the corner layout', () => {
  const state = PlateView.createState();
  assert.equal(state.layout, null);
  assert.equal(PlateView.layoutOf(state).name, 'Plus layout');
  assert.equal(PlateView.handleLine(state, sensorsLine('CORNER'), 0), true);
  assert.equal(PlateView.handleLine(state, sensorsLine('CORNER'), 0), false); // unchanged
  assert.equal(PlateView.layoutOf(state).name, 'Corner layout');
  for (let t = 0; t <= 3000; t += 50) PlateView.handleLine(state, `DBG COP ${t} 0.1 0.2 0 0 1500`, t);
  assert.equal(state.trail[0].timeMs, 1000);
  assert.equal(state.trail.at(-1).timeMs, 3000);
  PlateView.handleLine(state, 'DBG COP 3050 nan nan nan nan 0', 3050);
  assert.equal(state.trail.at(-1).timeMs, 3000); // unlocated samples aren't drawn
  assert.equal(state.latest.x, null);
  PlateView.handleLine(state, 'DBG COP 20 0.1 0.2 0 0 1500', 3100);
  assert.equal(state.trail.length, 1);
  assert.equal(PlateView.handleLine(state, cornerLine, 3100), true);
  assert.equal(state.corner.passive.y, -0.9);
  assert.equal(PlateView.handleLine(state, 'DBG R 1 2 3 4 5', 3100), false);
  assert.equal(PlateView.handleLine(state, 'Calibration complete', 3100), false);
  PlateView.handleLine(state, sensorsLine('PLUS'), 3200);
  assert.equal(state.corner, null);
  assert.equal(state.trail.length, 0);
});

function recorder() {
  const calls = [];
  return new Proxy({calls}, {
    get(target, key) {
      if (key in target) return target[key];
      return (...args) => { calls.push([key, ...args]); };
    },
    set(target, key, value) { calls.push(['set ' + String(key), value]); return true; },
  });
}

test('draw places the CoP dot, neutral and boxes in plate inches, front up', () => {
  const state = PlateView.createState();
  PlateView.handleLine(state, sensorsLine('CORNER'), 0);
  PlateView.handleLine(state, cornerLine, 0);
  PlateView.handleLine(state, 'DBG COP 100 1.00 2.00 1.3 3.6 2000', 100);
  const g = recorder();
  const w = 340, h = 360;
  PlateView.draw(g, w, h, state, {F: 500, B: 400, L: 300, R: 800}, 200);
  const {X, Y, scale} = PlateView.projection(w, h);
  assert.equal(scale, 20);
  assert.equal(X(0), 170);
  assert.ok(Y(1) < Y(0), 'front is up');
  const arcs = g.calls.filter(call => call[0] === 'arc');
  const dot = arcs.find(([, x, y, r]) => r === 6);
  assert.deepEqual(dot.slice(1, 3), [X(1), Y(2)]);
  const relaxed = arcs.find(([, x, y, r]) => r === 5);
  assert.deepEqual(relaxed.slice(1, 3), [X(-0.15), Y(-0.9)]);
  // Sensors: biggest dot on the most loaded channel (R, front-right).
  const sensorDots = arcs.filter(([, , , r]) => r !== 5 && r !== 6);
  assert.equal(sensorDots.length, 4);
  const biggest = sensorDots.reduce((a, b) => (b[3] > a[3] ? b : a));
  assert.deepEqual(biggest.slice(1, 3), [X(5.375), Y(2.9375)]);
  const rects = g.calls.filter(call => call[0] === 'fillRect').slice(1); // [0] is the background
  const close = (actual, expected) => expected.forEach((v, i) => assert.ok(Math.abs(actual[i] - v) < 1e-9, `${actual} vs ${expected}`));
  close(rects[0].slice(1), [X(-2), Y(1.25), 3.3 * scale, 4.78 * scale]); // full lean
  assert.equal(rects[1][1], X(-0.55)); // dead box
  assert.ok(Math.abs(rects[1][4] - 1.03 * scale) < 1e-9);
  assert.ok(g.calls.some(([k, v]) => k === 'set fillStyle' && v === '#287d9c'), 'fresh dot is blue');
});

test('draw copes with no calibration, a stale CoP and no telemetry', () => {
  const state = PlateView.createState();
  PlateView.handleLine(state, sensorsLine('CORNER'), 0);
  PlateView.handleLine(state, 'DBG COP 100 1 2 nan nan 2000', 0);
  const g = recorder();
  PlateView.draw(g, 300, 300, state, null, 5000);
  assert.ok(g.calls.some(([k, v]) => k === 'fillText' && /Calibrate in LEAN/.test(v)));
  assert.ok(g.calls.some(([k, v]) => k === 'set fillStyle' && v === '#a7b4c1'), 'stale dot is grey');
  assert.ok(!g.calls.some(([k]) => k === 'strokeRect'), 'no boxes before calibration');
  PlateView.draw(recorder(), 0, 0, PlateView.createState(), null, 0); // zero-size canvas
});

test('dashboard tiles relabel and move with the reported layout; the plate shows only on CORNER', () => {
  const html = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');
  const code = html.slice(html.indexOf('    // Channel tiles follow'), html.indexOf('    function sensors(r)'));
  const tiles = ['F', 'B', 'L', 'R'].map(key => {
    const label = {textContent: ''};
    return {dataset: {key}, style: {}, querySelector: () => label, label};
  });
  const elements = {'#layout-name': {textContent: ''}, '#plate-block': {hidden: false},
    '#cop-xy': {textContent: ''}, '#cop-offset': {textContent: ''}};
  const page = vm.createContext({
    PlateView, document: {querySelectorAll: () => tiles}, $: selector => elements[selector],
    performance: {now: () => 0}, lastRow: null, drawn: 0,
    surface: () => [new Proxy({}, {get: () => () => {}, set: () => true}), 300, 300],
  });
  vm.runInContext(code + '; var plate = PlateView.createState();', page);
  vm.runInContext('applyLayout()', page);
  assert.deepEqual(tiles.map(t => t.label.textContent), ['Front', 'Back', 'Left', 'Right']); // older firmware: plus
  assert.equal(elements['#plate-block'].hidden, true);
  assert.match(elements['#layout-name'].textContent, /layout not reported/);
  vm.runInContext(`plateLine('DBG SENSORS raw=1/2/3/4 floors=0/0/0/0 W=1 seatLoad=0 seated=0 calibrated=0 calActive=0 railed=0 layout=CORNER')`, page);
  assert.deepEqual(tiles.map(t => t.label.textContent), ['Back-left (F)', 'Back-right (B)', 'Front-left (L)', 'Front-right (R)']);
  assert.deepEqual(tiles.map(t => t.style.gridArea), ['3 / 1', '3 / 3', '1 / 1', '1 / 3']);
  assert.equal(elements['#plate-block'].hidden, false);
  assert.match(elements['#layout-name'].textContent, /Corner layout/);
  vm.runInContext(`plateLine('DBG COP 10 0.19 0.43 0.49 2.06 2507')`, page);
  assert.equal(elements['#cop-xy'].textContent, '0.19 / 0.43');
  assert.equal(elements['#cop-offset'].textContent, '0.49 / 2.06');
  vm.runInContext('resetPlate()', page);
  assert.equal(elements['#plate-block'].hidden, true);
  assert.equal(tiles[0].label.textContent, 'Front');
});

test('USB/replay: plate lines reach the plate view and the 20 Hz gain broadcast does not wipe it', () => {
  const html = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');
  const code = html.slice(html.indexOf('    function connect()'), html.indexOf("    $('#variable-scaling')"));
  let socket;
  const calls = [];
  const page = vm.createContext({
    location: {protocol: 'http:', host: 'localhost'},
    WebSocket: class { constructor() { socket = this; } },
    connectionMode: 'usb', usbConnected: true, lastRow: null,
    setDynamicGains: gains => calls.push(['gains', gains]),
    plateLine: line => calls.push(['plate', line]),
    resetConnectionStatus: () => calls.push(['reset']),
    appendConsole() {}, applyTelemetry() {}, showMode() {}, setTimeout() {},
    $: () => ({classList: {add() {}}}),
  });
  vm.runInContext(code + '; connect();', page);
  socket.onmessage({data: JSON.stringify({history: [], stale: false, dynamicGains: null})});
  socket.onmessage({data: JSON.stringify({plate: 'DBG COP 1 0 0 0 0 100'})});
  assert.deepEqual(calls, [['gains', null], ['plate', 'DBG COP 1 0 0 0 0 100']]);
  socket.onclose();
  assert.deepEqual(calls.at(-1), ['reset']);
});
