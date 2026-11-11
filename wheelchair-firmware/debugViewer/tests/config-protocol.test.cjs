const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const script = fs.readFileSync(path.join(__dirname, '..', 'config-protocol.js'), 'utf8');
const env = vm.createContext({TextDecoder, Uint8Array, DataView, setTimeout, clearTimeout});
vm.runInContext(script, env);
const wire = vm.runInContext('ConfigWire', env);
const {commands, types, editors} = wire;

function reply(seq, command, body = [], {version = 1, status = 0, flags = 2} = {}) {
  const data = Uint8Array.from([0xc2, version, seq, command, status, flags, ...body, 0, 0]);
  new DataView(data.buffer).setUint16(data.length - 2, wire.crc16(data.subarray(0, -2)), true);
  return wire.encodeFrame(data).subarray(0, -1);
}

function text(value) {
  const bytes = new TextEncoder().encode(value);
  return [bytes.length, ...bytes];
}

function f32(value) {
  const bytes = new Uint8Array(4);
  new DataView(bytes.buffer).setFloat32(0, value, true);
  return bytes;
}

test('configuration packets use CRC-16/CCITT-FALSE and COBS across block boundaries', () => {
  assert.equal(wire.crc16(new TextEncoder().encode('123456789')), 0x29b1);
  const request = wire.requestFrame(42, commands.read, wire.idBody(0x1234));
  assert.equal(request.at(-1), 0);
  const decoded = wire.decodeFrame(request.subarray(0, -1));
  assert.deepEqual([...decoded.subarray(0, -2)], [0xc1, 1, 42, commands.read, 0x34, 0x12]);
  assert.equal(wire.decodeFrame(Uint8Array.of(4, 0x42)), null);
  const long = Uint8Array.from({length: 385}, (_, i) => i === 320 ? 0 : 0x42);
  assert.deepEqual(wire.decodeFrame(wire.encodeFrame(long).subarray(0, -1)), long);
  const corrupt = reply(42, commands.read, [3]);
  corrupt[corrupt.length - 1] ^= 1;
  assert.equal(wire.responseFrame(corrupt), null);
});

test('discovered enum, signed power and unknown field types require no parameter-specific code', () => {
  const group = ConfigWireRead(commands.group, [2, 0, 1, 0, 1, ...text('Child')], wire.readGroup);
  assert.equal(group.name, 'Child');
  assert.equal(group.parent, 1);
  assert.equal(group.advanced, true);

  const enumeration = ConfigWireRead(commands.describe, [
    1, 0, 2, 0, types.enum, 1, ...text('Mode'), ...text('Description'), ...text(''),
    7, 2, 2, ...text('Lean'), 7, ...text('Remote'), 2
  ], wire.readField);
  assert.equal(enumeration.default, 7);
  assert.equal(enumeration.choices[0].name, 'Lean');
  assert.equal(enumeration.value, 2);
  assert.equal(enumeration.restart, true);

  const profile = ConfigWireRead(commands.describe, [
    30, 0, 2, 0, types.profile, 0, ...text('Another curve'), ...text('Description'), ...text(''),
    editors.signedPower, 2,
    ...text('Input'), ...f32(0.5), ...f32(0.125), ...f32(0.875),
    ...text('Output'), ...f32(0.25), ...f32(0.125), ...f32(0.875),
    editors.signedPower, ...f32(0.625), ...f32(0.375)
  ], wire.readField);
  assert.equal(profile.parameters[1].name, 'Output');
  assert.deepEqual([...profile.value], [0.625, 0.375]);
  const set = wire.setBody(profile, [0.75, 0.5]);
  assert.deepEqual([...set], [30, 0, editors.signedPower, ...f32(0.75), ...f32(0.5)]);

  const unknown = ConfigWireRead(commands.describe, [
    99, 0, 2, 0, 0xfe, 0, ...text('Future field'), ...text('Description'), ...text(''), 12, 34
  ], wire.readField);
  assert.equal(unknown.unsupported, true);
});

function ConfigWireRead(command, body, parse) {
  return parse(wire.responseFrame(reply(1, command, body)).body);
}

test('64-bit scalar values remain exact in both directions', () => {
  const value = 18446744073709551614n;
  const data = wire.setBody({id: 7, type: types.u64}, value);
  const reader = new wire.Reader(data.subarray(2));
  assert.equal(reader.get(types.u64), value);
  reader.done();
});

test('group preview metadata binds a graph to device-defined field IDs and limits', () => {
  const descriptor = ConfigWireRead(commands.preview, [
    1, 8, 70, 0, 71, 0, 72, 0, 73, 0, 74, 0, 75, 0, 42, 0, 62, 0,
    2, ...f32(2.25), ...f32(2.875)
  ], wire.readPreview);
  assert.equal(descriptor.type, 1);
  assert.deepEqual([...descriptor.fieldIds], [70, 71, 72, 73, 74, 75, 42, 62]);
  assert.deepEqual([...descriptor.constants], [2.25, 2.875]);
});

test('Web Serial link matches request IDs and surfaces version, validation and moving errors', async () => {
  const link = new wire.Link();
  const frames = [];
  link.attach({writable: new WritableStream({write(frame) { frames.push(frame); }})});
  const request = link.request(commands.hello);
  link.receive(reply(255, commands.hello, [1, 0, 2, 0]));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(frames.length, 1);
  link.receive(reply(0, commands.hello, [1, 0, 2, 0]));
  const info = await request;
  assert.equal(info.u16(), 1);
  assert.equal(info.u16(), 2);
  assert.equal(link.dirty, true);

  const invalid = link.request(commands.set, wire.idBody(1));
  link.receive(reply(1, commands.set, [], {status: 4, flags: 0}));
  await assert.rejects(invalid, /rejected the value/);
  assert.equal(link.dirty, false);

  const moving = link.request(commands.set, wire.idBody(1));
  link.receive(reply(2, commands.set, [], {status: 5, flags: 0}));
  await assert.rejects(moving, /commanded above 1 RPM/);

  const unsupported = link.request(commands.hello);
  link.receive(reply(3, commands.hello, [], {version: 2, status: 1}));
  await assert.rejects(unsupported, /different configuration protocol version/);
  const interrupted = link.request(commands.hello);
  const closed = assert.rejects(interrupted, /connection closed/);
  await link.attach(null);
  await closed;
});
