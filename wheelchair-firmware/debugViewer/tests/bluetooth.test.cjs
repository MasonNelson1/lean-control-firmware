const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const source = fs.readFileSync(path.join(__dirname, '..', 'bluetooth.js'), 'utf8');
const row = (time = 1000, mode = 'LEAN') => `${time},100,200,300,400,1000,370,0.25,-0.1,10,20,9,19,1,${mode}`;
const settle = () => new Promise(resolve => setImmediate(resolve));

function harness({secure = true, openError = null, backend = true, configWire = false} = {}) {
  const elements = new Map();
  const messages = [], frames = [], commands = [], plateLines = [];
  let plateResets = 0;
  let controller, now = 0, closed = false, requestOptions;
  const port = {
    readable: new ReadableStream({start(value) { controller = value; }}),
    async open(options) { assert.equal(options.baudRate, 115200); if (openError) throw openError; },
    async close() { assert.ok(!port.readable?.locked); closed = true; },
  };
  const context = vm.createContext({
    fetch: async () => ({ok: true, json: async () => ({})}),
    TextDecoder, performance: {now: () => now},
    window: {isSecureContext: secure},
    navigator: {serial: {async requestPort(options) { requestOptions = options; return port; }}},
    bluetoothActive: false, displayed: [], latest: [], lastRow: null, dynamicGains: null,
    dashboardBackend: backend, connectionMode: 'bluetooth', usbConnected: false,
    ws: {readyState: 1, send: command => commands.push(command)},
    $: selector => {
      if (!elements.has(selector)) elements.set(selector, {classList: {add() {}}, disabled: false, hidden: false,
        querySelector: () => ({remove() {}})});
      return elements.get(selector);
    },
    graphs() {}, showMode() {}, sensors() {},
    plateLine: line => plateLines.push(line), resetPlate: () => { plateResets++; },
    appendConsole: line => messages.push(line),
    applyTelemetry: (rows, stale, source) => frames.push({rows: rows.slice(), stale, source}),
    setInterval() { return 1; }, clearInterval() {}, setTimeout, clearTimeout,
  });
  if (configWire) vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'config-protocol.js'), 'utf8'), context);
  vm.runInContext(source, context);
  return {
    context, elements, frames, messages, commands, plateLines,
    get plateResets() { return plateResets; },
    run: code => vm.runInContext(code, context),
    send: value => controller.enqueue(typeof value === 'string' ? new TextEncoder().encode(value) : value),
    fail() {
      port.readable = null;
      controller.error(new Error('out of range'));
    },
    interrupt() {
      const old = controller;
      port.readable = new ReadableStream({start(value) { controller = value; }});
      old.error(new Error('serial overrun'));
    },
    setNow: value => { now = value; },
    get closed() { return closed; },
    get requestOptions() { return requestOptions; },
  };
}

test('parses current and legacy telemetry; rejects headers and damaged rows', () => {
  const h = harness();
  const parse = line => h.context.parseBluetoothLine(line);
  assert.equal(parse(row()).mode, 'LEAN');
  assert.equal(parse(row(1000, 'DISARM')).mode, 'DISARM');
  assert.equal(parse(row()).rightSmoothedRPM, 19);
  assert.equal(parse(row().split(',').slice(0, 14).join(',')).mode, 'UNKNOWN');
  assert.equal(parse('1,2,3,4,5,6,7,0,0,10,20,1').stopped, 1);
  for (const line of ['time_ms,F,B,L,R', 'Calibrated', row().replace('1000,100', 'NaN,100'), row().replace(',200,', ',,')]) {
    assert.equal(parse(line), null);
  }
});

test('parses measured wheel RPM and requested assist current', () => {
  const parse = harness().context.parseBluetoothLine;
  const sample = parse(row().replace(/LEAN$/, '12,-8,3.25,4.5,ASSIST'));
  assert.equal(sample.leftActualRPM, 12);
  assert.equal(sample.rightActualRPM, -8);
  assert.equal(sample.leftBoostCurrent, 3.25);
  assert.equal(sample.rightBoostCurrent, 4.5);
  assert.equal(sample.mode, 'ASSIST');
  const missing = parse('1,2,3,4,5,6,7,0,0,nan,nan,nan,nan,1,nan,0,0,0,DISARM');
  assert.equal(missing.leftActualRPM, undefined);
  assert.equal(missing.rightActualRPM, 0);
  assert.equal(missing.leftSmoothedRPM, undefined);
});

test('parses gain status without accepting incomplete, nonfinite, or nonpositive gains', () => {
  const parse = harness().context.parseDynamicGains;
  assert.equal(JSON.stringify(parse('DYNAMIC_GAINS F=1.2300 B=2.3400 L=1.0000 R=0.9800')),
    JSON.stringify({F: 1.23, B: 2.34, L: 1, R: 0.98}));
  for (const line of [
    'DYNAMIC_GAINS F=1 B=2 L=3',
    'DYNAMIC_GAINS F=nan B=2 L=3 R=4',
    'DYNAMIC_GAINS F=-1 B=2 L=3 R=4',
    'DYNAMIC_GAINS F=0 B=2 L=3 R=4',
    'DYNAMIC_GAINS F=1 B=2 L=3 R=4 garbage',
  ]) assert.equal(parse(line), null);
});

test('fragmented gain status updates separately from telemetry and clears on disconnect', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  h.send('DYNAMIC_GAINS F=1.2300 B=');
  await settle();
  assert.equal(h.context.dynamicGains, null);
  h.send('2.3400 L=1.0000 R=0.9800\n' + row() + '\n');
  await settle();
  assert.equal(h.context.dynamicGains.F, 1.23);
  assert.equal(h.context.dynamicGains.B, 2.34);
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).rows.length, 1);
  assert.ok(!h.messages.some(message => message.startsWith('DYNAMIC_GAINS')));
  await h.context.disconnectBluetooth();
  assert.equal(h.context.dynamicGains, null);
});

test('source changes discard gains and inactive Bluetooth cannot overwrite the USB cache', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  h.send('DYNAMIC_GAINS F=1 B=2 L=3 R=4\n');
  await settle();
  assert.equal(h.context.dynamicGains.R, 4);
  h.elements.get('#connection-mode').value = 'usb';
  await h.context.changeConnectionMode();
  assert.equal(h.context.dynamicGains, null);
  h.send('DYNAMIC_GAINS F=9 B=9 L=9 R=9\n');
  await settle();
  assert.equal(h.context.dynamicGains, null);
  await h.context.disconnectBluetooth();
});

test('sensor panel displays active gains beside the sensor counts', () => {
  const html = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');
  const code = html.slice(html.indexOf('    function sensors(r)'), html.indexOf('    function connect()'));
  const panels = ['F', 'B', 'L', 'R'].map(key => {
    const children = new Map();
    return {dataset: {key}, querySelector(selector) {
      if (!children.has(selector)) children.set(selector, {style: {}});
      return children.get(selector);
    }};
  });
  const context = vm.createContext({
    dynamicGains: {F: 1.23, B: 2.34, L: 1, R: 0.98},
    document: {querySelectorAll: () => panels},
    $: () => ({classList: {toggle() {}}}),
    smoothedDirection: () => null, drawDirection() {}, drawPlate() {},
  });
  vm.runInContext(code, context);
  context.sensors({F: 100, B: 200, L: 300, R: 400, total: 1000});
  assert.equal(panels[0].querySelector('strong').textContent, 100);
  assert.equal(panels[0].querySelector('.gain').textContent, '×1.23');
  assert.equal(panels[3].querySelector('.gain').textContent, '×0.98');
  context.dynamicGains = null;
  context.sensors(null);
  assert.equal(panels[0].querySelector('.gain').textContent, '—');
});

test('DBG lines feed the plate view, stay out of the console, and the plate resets with the connection', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  const resetsAtConnect = h.plateResets;
  assert.ok(resetsAtConnect >= 1);
  h.send('DBG SENSORS raw=1/2/3/4 floors=0/0/0/0 W=370 seatLoad=0 seated=0 calibrated=0 calActive=0 railed=0 layout=CORNER\n' +
         'DBG COP 1234 0.19 0.43 nan nan 2507\nDBG R 1 2 3 4 5\nCalibration complete\n');
  await settle();
  assert.deepEqual(h.plateLines.map(line => line.split(' ')[1]), ['SENSORS', 'COP', 'R']);
  assert.deepEqual(h.messages.filter(m => m.startsWith('DBG')), []);
  assert.ok(h.messages.includes('Calibration complete'));
  await h.context.disconnectBluetooth();
  assert.ok(h.plateResets > resetsAtConnect);
});

test('reads fragmented lines, marks stale data, clears history after device reboot, and disconnects cleanly', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  assert.equal(h.context.bluetoothActive, true);
  assert.equal(h.requestOptions.filters[0].bluetoothServiceClassId, 0x1101);
  h.send(row().slice(0, 17));
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).rows.length, 0);
  h.send(row().slice(17) + '\r\n' + row(1050) + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).rows.length, 2);
  assert.equal(h.frames.at(-1).stale, false);
  h.setNow(2500);
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).stale, true);
  h.send(row(10) + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).rows.length, 1);
  assert.equal(h.frames.at(-1).rows[0].time_ms, 10);
  await h.context.disconnectBluetooth();
  assert.equal(h.closed, true);
  assert.equal(h.context.bluetoothActive, false);
  assert.equal(h.elements.get('#connection-connect').disabled, false);
  assert.equal(h.elements.get('#connection-disconnect').disabled, true);
  assert.ok(h.commands.every(command => command === 'disconnect'));
});

test('Bluetooth loss releases reader and allows reconnecting', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  h.fail();
  await settle();
  assert.equal(h.closed, true);
  assert.equal(h.context.bluetoothActive, false);
  assert.ok(h.messages.some(message => message.includes('out of range')));
  assert.equal(h.elements.get('#connection-disconnect').disabled, true);
});

test('recovers a transient read error without closing Bluetooth', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  h.interrupt();
  await settle();
  h.send(row(1000, 'ASSIST') + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).rows[0].mode, 'ASSIST');
  assert.equal(h.closed, false);
  await h.context.disconnectBluetooth();
});

test('configuration replies and CSV share the Bluetooth reader without leaking binary into telemetry', async () => {
  const h = harness();
  const replies = [];
  h.context.window.configLink = {receive: frame => replies.push([...frame])};
  await h.context.connectBluetooth();
  h.send(row() + '\n');
  h.send(Uint8Array.of(0, 2, 10));
  h.send(Uint8Array.of(9, 13, 0));
  h.send(row(1050) + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.deepEqual(replies, [[2, 10, 9, 13]]);
  assert.equal(h.frames.at(-1).rows.length, 2);
  assert.equal(h.messages.some(message => message.includes('\u0000')), false);
  await h.context.disconnectBluetooth();
});

test('insecure origins and failed opens do not start a Bluetooth connection', async () => {
  const insecure = harness({secure: false});
  await insecure.context.connectBluetooth();
  assert.equal(insecure.context.bluetoothActive, false);
  assert.ok(insecure.messages.some(message => message.includes('localhost')));
  const failed = harness({openError: new Error('device busy')});
  await failed.context.connectBluetooth();
  assert.equal(failed.context.bluetoothActive, false);
  assert.equal(failed.elements.get('#connection-connect').disabled, false);
  assert.ok(failed.messages.some(message => message.includes('device busy')));
  assert.ok(failed.messages.some(message => message.includes('finish pairing')));
});

test('dropdown preserves Bluetooth while viewing legacy USB', async () => {
  const h = harness();
  h.run("connectionMode = 'usb'; usbConnected = true");
  h.elements.get('#connection-mode').value = 'bluetooth';
  await h.context.changeConnectionMode();
  assert.equal(h.context.connectionMode, 'bluetooth');
  assert.equal(h.context.usbConnected, false);
  assert.equal(h.context.bluetoothActive, false);
  assert.equal(h.commands.at(-1), 'disconnect');
  await h.context.connectSelected();
  assert.equal(h.context.bluetoothActive, true);
  assert.equal(h.elements.get('#connection-connect').disabled, true);
  h.elements.get('#connection-mode').value = 'usb';
  await h.context.changeConnectionMode();
  assert.equal(h.closed, false);
  assert.equal(h.context.connectionMode, 'usb');
  assert.equal(h.context.usbConnected, false);
  assert.equal(h.commands.at(-1), 'disconnect');
  await h.context.connectSelected();
  assert.equal(h.commands.at(-1), 'connect');
  assert.equal(h.context.usbConnected, true);
  await h.context.disconnectSelected();
  assert.equal(h.commands.at(-1), 'disconnect');
  assert.equal(h.context.usbConnected, false);
});


test('Bluetooth connects and disconnects without forcing Linux cleanup', async () => {
  const h = harness();
  h.context.fetch = () => assert.fail('Bluetooth must not invoke OS cleanup');
  await h.context.connectBluetooth();
  assert.equal(h.context.bluetoothActive, true);
  await h.context.disconnectBluetooth();
  assert.equal(h.closed, true);
});

test('hosted Bluetooth connects without any Python cleanup or USB monitoring requests', async () => {
  const h = harness({backend: false});
  h.context.fetch = () => assert.fail('Hosted Bluetooth must not call Python');
  await h.context.connectSelected();
  assert.equal(h.context.bluetoothActive, true);
  assert.deepEqual(h.commands, []);
  await h.context.disconnectSelected();
  assert.equal(h.closed, true);
});


const currentRow = (speed = '0.25', turn = '-0.1') =>
  `1000,100,200,300,400,1000,370,${speed},${turn},10,20,9,19,0,nan,-nan,0,0,LEAN`;

test('current CSV keeps valid fields when a Lean float is nonfinite', () => {
  const parse = harness().context.parseBluetoothLine;
  for (const token of ['nan', '-nan', '+NaN', 'inf', '-Infinity']) {
    const sample = parse(currentRow(token));
    assert.equal(sample.mode, 'LEAN');
    assert.equal(sample.F, 100);
    assert.equal(sample.speed, undefined);
    assert.equal(sample.turn, -0.1);
    assert.equal(sample.leftActualRPM, undefined);
    assert.equal(sample.rightActualRPM, undefined);
  }
  assert.equal(parse(currentRow('garbage')), null);
  assert.equal(parse(currentRow().replace('1000,100', 'nan,100')), null);
});

test('Lean rows with unavailable direction update telemetry without flooding the console', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  h.send(Array.from({length: 40}, () => currentRow('-nan', 'nan') + '\n').join(''));
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).rows.length, 40);
  assert.equal(h.frames.at(-1).rows[0].F, 100);
  assert.equal(h.messages.filter(m => m.startsWith('Telemetry warning')).length, 1);
  assert.ok(h.messages.some(m => m.includes('unavailable speed, turn')));
  assert.ok(!h.messages.some(m => m.startsWith('1000,')));
  await h.context.disconnectBluetooth();
});

test('rejected CSV is reported at most once per five seconds while status messages remain visible', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  const damaged = currentRow('garbage');
  h.send((damaged + '\n').repeat(100) + 'Mode: LEAN\n');
  await settle();
  assert.equal(h.messages.filter(m => m.startsWith('Telemetry warning')).length, 1);
  assert.ok(h.messages.includes('Mode: LEAN'));
  assert.ok(!h.messages.includes(damaged));
  h.setNow(4999);
  h.send(damaged + '\n');
  await settle();
  assert.equal(h.messages.filter(m => m.startsWith('Telemetry warning')).length, 1);
  h.setNow(5000);
  h.send(damaged + '\n' + currentRow() + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.messages.filter(m => m.startsWith('Telemetry warning')).length, 2);
  assert.equal(h.frames.at(-1).rows.length, 1);
  await h.context.disconnectBluetooth();
});

test('sensor panel handles missing direction fields without throwing or drawing invalid vectors', () => {
  const html = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');
  const code = html.slice(html.indexOf('    function sensors(r)'), html.indexOf('    function connect()'));
  const elements = new Map(), vectors = [];
  const context = vm.createContext({
    document: {querySelectorAll: () => []},
    $: id => {
      if (!elements.has(id)) elements.set(id, {classList: {toggle() {}}});
      return elements.get(id);
    },
    smoothedDirection: () => null,
    drawDirection: (id, value) => vectors.push([id, value]), drawPlate() {},
  });
  vm.runInContext(code, context);
  context.sensors({F: 100, B: 200, L: 300, R: 400, total: 1000, userWeight: 370, stopped: 0});
  assert.equal(elements.get('#speed').textContent, '\u2014');
  assert.equal(elements.get('#turn').textContent, '\u2014');
  assert.equal(vectors.find(([id]) => id === 'vector')[1], null);
});


const joinedLeanSample = '1782632,0,0,43,0,43,757,0.0000,-0.0356,-1.8723,1.8723,-1.9189,1.9189,0,0.0000,0.0000,0.0000,0.0000,LE1782682,0,0,46,0,46,757,0.0000,-0.0385,-2.0251,2.0251,-1.8511,1.8511,0,0.0000,0.0000,0.0000,0.0000,LEAN';

test('recovers the complete Lean row from the reported 37-column damaged stream', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  for (let index = 0; index < joinedLeanSample.length; index += 23) {
    h.send(joinedLeanSample.slice(index, index + 23));
  }
  h.send('\nMode: LEAN\n');
  await settle();
  h.context.renderBluetooth();
  const samples = h.frames.at(-1).rows;
  assert.equal(samples.length, 1);
  assert.equal(samples[0].time_ms, 1782682);
  assert.equal(samples[0].L, 46);
  assert.equal(samples[0].turn, -0.0385);
  assert.equal(samples[0].mode, 'LEAN');
  assert.ok(h.messages.some(m => m.includes('recovered 1 complete rows')));
  assert.ok(h.messages.includes('Mode: LEAN'));
  await h.context.disconnectBluetooth();
});

test('recovers multiple complete rows when newline bytes are missing', async () => {
  const h = harness();
  await h.context.connectBluetooth();
  const a = currentRow().replace(/^1000,/, '2000,');
  const b = currentRow().replace(/^1000,/, '2050,');
  const c = currentRow().replace(/^1000,/, '2100,');
  h.send(a + b + c + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.deepEqual(Array.from(h.frames.at(-1).rows, r => r.time_ms), [2000, 2050, 2100]);
  assert.ok(h.messages.some(m => m.includes('recovered 3 complete rows')));
  await h.context.disconnectBluetooth();
});

test('recovery does not invent fields or accept partial rows', () => {
  const recover = harness().context.recoverTelemetryRows;
  assert.equal(recover(currentRow().slice(0, -2)).length, 0);
  assert.equal(recover(currentRow('garbage')).length, 0);
  assert.equal(recover(currentRow().replace('1000,100', '999999999999,100')).length, 0);
  assert.equal(recover('1787682,0,0,49,0,49,757,0.0000,-0.0414,-2.1795,2.1795').length, 0);
});


function configResponse(wire, seq) {
  const data = Uint8Array.of(0xc2, 1, seq, wire.commands.hello, 0, 0, 1, 0, 2, 0, 0, 0);
  new DataView(data.buffer).setUint16(data.length - 2, wire.crc16(data.subarray(0, -2)), true);
  return wire.encodeFrame(data); // includes trailing zero
}

for (const damage of ['missing opening zero', 'missing closing zero', 'corrupt payload', 'oversized payload']) {
  test(`binary receiver resynchronizes after ${damage} without reconnecting`, async () => {
    const h = harness({configWire: true}), wire = h.run('ConfigWire');
    const received = [];
    h.context.window.configLink = {receive: frame => received.push(wire.responseFrame(frame))};
    await h.context.connectBluetooth();
    const first = configResponse(wire, 0), next = configResponse(wire, 1);
    if (damage === 'missing opening zero') h.send(first);
    if (damage === 'missing closing zero') h.send(Uint8Array.of(0, ...first.subarray(0, -1)));
    if (damage === 'corrupt payload') {
      first[first.length - 2] ^= 1;
      h.send(Uint8Array.of(0, ...first));
    }
    if (damage === 'oversized payload') h.send(Uint8Array.of(0, ...new Uint8Array(500).fill(65), 0));
    h.send(currentRow() + '\n'); // may be discarded until binary alignment is recovered
    h.send(Uint8Array.of(0, ...next));
    h.send(currentRow().replace(/^1000,/, '1050,') + '\n');
    await settle();
    h.context.renderBluetooth();
    assert.equal(received.length, 1);
    assert.equal(received[0].seq, 1);
    assert.equal(h.frames.at(-1).rows.at(-1).time_ms, 1050);
    assert.equal(h.closed, false);
    await h.context.disconnectBluetooth();
  });
}

test('stale telemetry does not close the serial connection or cancel an active configuration read', async () => {
  const h = harness({configWire: true}), wire = h.run('ConfigWire');
  const link = new wire.Link();
  link.attach({writable: new WritableStream({write() {}})});
  h.context.window.configLink = link;
  const modes = [];
  h.context.window.configurationModeChanged = mode => modes.push(mode);
  await h.context.connectBluetooth();
  h.send(currentRow() + '\n');
  await settle();
  h.context.renderBluetooth();
  const pending = link.request(wire.commands.hello);
  await settle();
  h.setNow(2100);
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).stale, true);
  assert.equal(modes.at(-1), null);
  assert.ok(link.pending);
  assert.equal(h.closed, false);
  h.send(Uint8Array.of(0, ...configResponse(wire, 0)));
  await settle();
  const reply = await pending;
  assert.equal(reply.u16(), 1);
  assert.equal(reply.u16(), 2);
  h.send(currentRow().replace(/^1000,/, '2200,') + '\n');
  await settle();
  h.context.renderBluetooth();
  assert.equal(h.frames.at(-1).stale, false);
  assert.equal(modes.at(-1), 'LEAN');
  await h.context.disconnectBluetooth();
});
