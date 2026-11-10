const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const source = fs.readFileSync(path.join(__dirname, '..', 'bluetooth.js'), 'utf8');
const row = (time = 1000, mode = 'LEAN') => `${time},100,200,300,400,1000,370,0.25,-0.1,10,20,9,19,1,${mode}`;
const settle = () => new Promise(resolve => setImmediate(resolve));

function harness({secure = true, openError = null} = {}) {
  const elements = new Map();
  const messages = [], frames = [], commands = [];
  let controller, now = 0, closed = false, requestOptions;
  const port = {
    readable: new ReadableStream({start(value) { controller = value; }}),
    async open(options) { assert.equal(options.baudRate, 115200); if (openError) throw openError; },
    async close() { assert.equal(port.readable.locked, false); closed = true; },
  };
  const context = vm.createContext({
    TextDecoder, performance: {now: () => now},
    window: {isSecureContext: secure},
    navigator: {serial: {async requestPort(options) { requestOptions = options; return port; }}},
    bluetoothActive: false, displayed: [], latest: [], lastRow: null,
    connectionMode: 'bluetooth', usbConnected: false,
    ws: {readyState: 1, send: command => commands.push(command)},
    $: selector => {
      if (!elements.has(selector)) elements.set(selector, {classList: {add() {}}, disabled: false, hidden: false});
      return elements.get(selector);
    },
    graphs() {}, showMode() {}, sensors() {},
    appendConsole: line => messages.push(line),
    applyTelemetry: (rows, stale, source) => frames.push({rows: rows.slice(), stale, source}),
    setInterval() { return 1; }, clearInterval() {},
  });
  vm.runInContext(source, context);
  return {
    context, elements, frames, messages, commands,
    run: code => vm.runInContext(code, context),
    send: text => controller.enqueue(new TextEncoder().encode(text)),
    fail: () => controller.error(new Error('out of range')),
    setNow: value => { now = value; },
    get closed() { return closed; },
    get requestOptions() { return requestOptions; },
  };
}

test('parses current and legacy telemetry; rejects headers and damaged rows', () => {
  const h = harness();
  const parse = line => h.context.parseBluetoothLine(line);
  assert.equal(parse(row()).mode, 'LEAN');
  assert.equal(parse(row()).rightSmoothedRPM, 19);
  assert.equal(parse(row().split(',').slice(0, 14).join(',')).mode, 'UNKNOWN');
  assert.equal(parse('1,2,3,4,5,6,7,0,0,10,20,1').stopped, 1);
  for (const line of ['time_ms,F,B,L,R', 'Calibrated', row().replace('1000,100', 'NaN,100'), row().replace(',200,', ',,')]) {
    assert.equal(parse(line), null);
  }
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

test('dropdown stops USB retries and connects only the selected transport', async () => {
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
  assert.equal(h.closed, true);
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
