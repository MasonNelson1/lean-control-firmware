const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

function harness() {
  const timers = [];
  const context = vm.createContext({
    TextDecoder, Uint8Array, DataView,
    setTimeout(callback) { timers.push(callback); return timers.length; },
    clearTimeout() {}
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'config-protocol.js'), 'utf8'), context);
  return {wire: vm.runInContext('ConfigWire', context), timers};
}
const {wire} = harness();
const {types, commands} = wire;

const scalarCases = [
  ['float32', types.f32, [-.25, 0, 1.5]],
  ['float64', types.f64, [-1.125, 0, Math.PI]],
  ['int8', types.i8, [-128, 0, 127]],
  ['uint8', types.u8, [0, 127, 255]],
  ['int16', types.i16, [-32768, 0, 32767]],
  ['uint16', types.u16, [0, 32768, 65535]],
  ['int32', types.i32, [-2147483648, 0, 2147483647]],
  ['uint32', types.u32, [0, 2147483648, 4294967295]],
  ['int64', types.i64, [-9223372036854775808n, 0n, 9223372036854775807n]],
  ['uint64', types.u64, [0n, 9223372036854775808n, 18446744073709551615n]],
  ['boolean', types.boolean, [false, true]],
  ['enum', types.enum, [0, 7, 255]]
];
for (const [name, type, values] of scalarCases) {
  for (const value of values) test('wire scalar ' + name + ' round trip ' + value, () => {
    const bytes = wire.setBody({id: 0x1234, type}, value);
    assert.deepEqual([...bytes.subarray(0, 2)], [0x34, 0x12]);
    const reader = new wire.Reader(bytes.subarray(2));
    assert.equal(reader.get(type), value); reader.done();
  });
  test('wire scalar ' + name + ' rejects truncated encoding', () => {
    const bytes = wire.setBody({id: 1, type}, values[0]).subarray(2);
    assert.throws(() => new wire.Reader(bytes.subarray(0, -1)).get(type));
  });
}
for (const length of [0, 1, 2, 3, 16, 253, 254, 255, 256, 385, 400]) {
  for (const pattern of ['zero', 'nonzero', 'mixed']) test('COBS ' + pattern + ' payload at ' + length + ' bytes', () => {
    const bytes = Uint8Array.from({length}, (_, index) => pattern === 'zero' ? 0 : pattern === 'nonzero' ? 123 : index % 256);
    const encoded = wire.encodeFrame(bytes);
    assert.equal(encoded.at(-1), 0);
    assert.ok([...encoded.subarray(0, -1)].every(byte => byte !== 0));
    assert.deepEqual(wire.decodeFrame(encoded.subarray(0, -1)), bytes);
  });
}
for (const code of [2, 3, 8, 127, 254, 255]) test('COBS rejects truncated block code ' + code, () => {
  assert.equal(wire.decodeFrame(Uint8Array.of(code)), null);
});
test('COBS rejects zero block marker', () => assert.equal(wire.decodeFrame(Uint8Array.of(0)), null));
test('COBS rejects a zero embedded inside an encoded data block', () => {
  assert.equal(wire.decodeFrame(Uint8Array.of(2, 0)), null);
});

function response(currentWire, seq, command, status = 0, body = []) {
  const bytes = Uint8Array.of(0xc2, 1, seq, command, status, 2, ...body, 0, 0);
  new DataView(bytes.buffer).setUint16(bytes.length - 2, currentWire.crc16(bytes.subarray(0, -2)), true);
  return currentWire.encodeFrame(bytes).subarray(0, -1);
}
for (const [name, status, message] of [
  ['version', 1, /version/], ['malformed', 2, /format/], ['unknown field', 3, /unavailable/],
  ['invalid value', 4, /rejected the value/], ['moving', 5, /1 RPM/],
  ['EEPROM', 6, /save or restore/], ['oversized schema', 7, /protocol limit/],
  ['future error', 255, /rejected the request/]
]) test('serial link surfaces ' + name + ' errors', async () => {
  const {wire: w} = harness(), link = new w.Link();
  link.attach({writable: new WritableStream({write() {}})});
  const request = link.request(commands.read, wire.idBody(1));
  await new Promise(resolve => setImmediate(resolve));
  link.receive(response(w, 0, commands.read, status));
  await assert.rejects(request, message);
  assert.equal(link.pending, null);
});
test('serial link rejects simultaneous requests', async () => {
  const {wire: w} = harness(), link = new w.Link();
  link.attach({writable: new WritableStream({write() {}})});
  const first = link.request(commands.hello);
  await assert.rejects(link.request(commands.hello), /in progress/);
  const closed = assert.rejects(first, /connection closed/);
  await link.attach(null); await closed;
});
test('serial link times out without acknowledgment', async () => {
  const {wire: w, timers} = harness(), link = new w.Link();
  link.attach({writable: new WritableStream({write() {}})});
  const request = link.request(commands.hello);
  timers[0]();
  await assert.rejects(request, /No configuration reply/);
  assert.equal(link.pending, null);
});
test('serial link releases writer after failed write', async () => {
  const {wire: w} = harness(), link = new w.Link();
  let released = false;
  link.attach({writable: {getWriter() {
    return {async write() { throw Error('port lost'); }, releaseLock() { released = true; }};
  }}});
  await assert.rejects(link.request(commands.hello), /port lost/);
  assert.equal(released, true);
});
test('serial link rejects requests after disconnect', async () => {
  const {wire: w} = harness(), link = new w.Link();
  await assert.rejects(link.request(commands.hello), /Connect/);
});
for (const count of [0, 1, 3, 5, 9, 255]) test('schema rejects invalid power-profile count ' + count, () => {
  const body = Uint8Array.of(1, 0, 1, 0, types.profile, 0, 0, 0, 0, 1, count);
  assert.throws(() => wire.readField(new wire.Reader(body)), /dimensions/);
});
for (const length of [0, 1, 2, 3, 4, 5, 6, 7]) test('response rejects short header ' + length, () => {
  assert.equal(wire.responseFrame(wire.encodeFrame(new Uint8Array(length)).subarray(0, -1)), null);
});
for (const length of [0, 1, 10, 100, 255]) test('string reader accepts ' + length + ' ASCII bytes', () => {
  const reader = new wire.Reader(Uint8Array.of(length, ...new TextEncoder().encode('x'.repeat(length))));
  assert.equal(reader.text(), 'x'.repeat(length)); reader.done();
});
test('string reader rejects incomplete UTF-8', () => assert.throws(() => new wire.Reader(Uint8Array.of(1, 0xc3)).text()));
test('string reader rejects body overflow', () => assert.throws(() => new wire.Reader(Uint8Array.of(10, 65)).text(), /Incomplete/));
test('reader rejects unexpected trailing data', () => assert.throws(() => new wire.Reader(Uint8Array.of(1)).done(), /Unexpected/));
