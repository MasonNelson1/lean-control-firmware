const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '..', 'config-protocol.js'), 'utf8');
const settle = () => new Promise(resolve => setImmediate(resolve));

function harness() {
  const timers = new Map(), frames = [];
  let id = 0;
  const context = vm.createContext({TextDecoder, Uint8Array, DataView,
    setTimeout(callback, delay) { timers.set(++id, {callback, delay}); return id; },
    clearTimeout(key) { timers.delete(key); },
  });
  vm.runInContext(source, context);
  const wire = vm.runInContext('ConfigWire', context), link = new wire.Link();
  link.attach({writable: new WritableStream({write(frame) { frames.push(wire.decodeFrame(frame.subarray(0, -1))); }})});
  function reply(frame = frames.at(-1), status = 0) {
    const bytes = Uint8Array.of(0xc2, 1, frame[2], frame[3], status, 0, 42, 0, 0);
    new DataView(bytes.buffer).setUint16(bytes.length - 2, wire.crc16(bytes.subarray(0, -2)), true);
    return wire.encodeFrame(bytes).subarray(0, -1);
  }
  return {wire, link, frames, timers, reply,
    expire() {
      assert.equal(timers.size, 1);
      const [key, timer] = timers.entries().next().value;
      timers.delete(key);
      timer.callback();
      return timer;
    },
  };
}

for (const name of ['hello', 'group', 'describe', 'read', 'preview', 'profileList', 'profileRead']) {
  test(`${name} retries only its lost request and accepts the second reply`, async () => {
    const h = harness(), command = h.wire.commands[name];
    const request = h.link.request(command, Uint8Array.of(7, 0));
    await settle();
    const original = h.frames[0];
    assert.equal(h.expire().delay, 2500);
    await assert.rejects(h.link.request(command), /in progress/);
    await settle();
    assert.equal(h.frames.length, 2);
    const retry = h.frames[1];
    assert.notEqual(original[2], retry[2]);
    assert.equal(retry[3], command);
    assert.deepEqual([...retry.subarray(4, -2)], [7, 0]);
    h.link.receive(h.reply(original)); // late first attempt must be ignored
    assert.ok(h.link.pending);
    h.link.receive(h.reply(retry));
    assert.equal((await request).u8(), 42);
    assert.equal(h.link.pending, null);
    assert.equal(h.timers.size, 0);
  });
}

test('a read can succeed on its third attempt', async () => {
  const h = harness();
  const request = h.link.request(h.wire.commands.describe);
  await settle(); h.expire();
  await settle(); h.expire();
  await settle();
  assert.equal(h.frames.length, 3);
  h.link.receive(h.reply());
  assert.equal((await request).u8(), 42);
  assert.equal(h.timers.size, 0);
});

test('reads stop after exactly two retries and release the request slot', async () => {
  const h = harness();
  const request = h.link.request(h.wire.commands.describe);
  const failed = assert.rejects(request, /No configuration reply after 3 attempts/);
  await settle(); h.expire();
  await settle(); h.expire();
  await settle(); h.expire();
  await failed;
  assert.equal(h.frames.length, 3);
  assert.equal(h.link.pending, null);
  assert.equal(h.timers.size, 0);
});

for (const name of ['set', 'save', 'revert', 'defaults', 'profileBegin', 'profileChunk', 'profileCommit', 'profileMove']) {
  test(`${name} is never retried after a missing acknowledgement`, async () => {
    const h = harness();
    const request = h.link.request(h.wire.commands[name]);
    const failed = assert.rejects(request, /No configuration reply/);
    await settle();
    const timer = h.expire();
    assert.equal(timer.delay, ['save', 'profileCommit', 'profileMove'].includes(name) ? 8000 : 2500);
    await failed;
    assert.equal(h.frames.length, 1);
    assert.equal(h.timers.size, 0);
  });
}

test('device rejection of a read is surfaced immediately without retries', async () => {
  const h = harness();
  const request = h.link.request(h.wire.commands.describe);
  await settle();
  h.link.receive(h.reply(undefined, 3));
  await assert.rejects(request, /unavailable/);
  assert.equal(h.frames.length, 1);
  assert.equal(h.timers.size, 0);
});

test('corrupted replies time out and retry the same field', async () => {
  const h = harness();
  const request = h.link.request(h.wire.commands.describe, Uint8Array.of(17, 0));
  await settle();
  const corrupt = h.reply();
  corrupt[corrupt.length - 1] ^= 1;
  h.link.receive(corrupt);
  assert.ok(h.link.pending);
  h.expire(); await settle();
  h.link.receive(h.reply());
  assert.equal((await request).u8(), 42);
  assert.deepEqual([...h.frames[1].subarray(4, -2)], [17, 0]);
});

test('disconnect during retries cancels remaining attempts and stale timers', async () => {
  const h = harness();
  const request = h.link.request(h.wire.commands.hello);
  const closed = assert.rejects(request, /connection closed/);
  await settle();
  h.expire(); await settle();
  const stale = h.timers.values().next().value.callback;
  await h.link.attach(null);
  await closed;
  stale(); await settle();
  assert.equal(h.frames.length, 2);
  assert.equal(h.timers.size, 0);
});

test('discovery continues from the failed field instead of re-reading earlier fields', async () => {
  const h = harness(), {commands: c} = h.wire;
  for (const [command, field] of [[c.hello, 0], [c.group, 0], [c.describe, 0], [c.describe, 1], [c.describe, 2]]) {
    const request = h.link.request(command, Uint8Array.of(field, 0));
    await settle();
    if (command === c.describe && field === 1) { h.expire(); await settle(); }
    h.link.receive(h.reply());
    await request;
  }
  assert.deepEqual(h.frames.map(frame => [frame[3], frame[4]]),
    [[c.hello, 0], [c.group, 0], [c.describe, 0], [c.describe, 1], [c.describe, 1], [c.describe, 2]]);
});
