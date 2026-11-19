const assert = require('node:assert/strict');
const {test} = require('node:test');
const fflate = require('../vendor/fflate-0.8.3.js');
const {codec, env} = require('./profile-codec-helper.cjs');
const profile = {format: 'lean-config', version: 1, fields: [
  {id: 1, type: 10, value: '18446744073709551615'},
  {id: 2, type: 11, value: false},
  {id: 3, type: 13, value: ['0.21000000834465027', '0.09000000357627869']},
  {id: 9999, type: 200, value: 'unknown', extra: 'ignored'}
]};
const zip = content => fflate.zipSync({'profile.json': [content, {level: 6}]});
test('browser .lean codec round trips exact values and unknown settings', async () => {
  const archive = await codec.encode(profile);
  assert.equal(new DataView(archive.buffer).getUint32(0, true), 0x04034b50);
  assert.deepEqual(JSON.parse(JSON.stringify(await codec.decode(archive))), profile);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(fflate.unzipSync(archive)['profile.json'])), profile);
});
test('browser profile encoding has no network dependencies', async () => {
  env.fetch = () => assert.fail('No network access');
  assert.ok((await codec.encode(profile)).length);
});
test('profile codec rejects malformed, unsupported and oversized files', async () => {
  for (const bytes of [new Uint8Array(), Uint8Array.of(1, 2, 3), new Uint8Array(codec.limit + 1),
    zip(new TextEncoder().encode('{}')), zip(Uint8Array.of(0xff)),
    zip(new TextEncoder().encode(JSON.stringify({...profile, version: 2}))),
    fflate.zipSync({'profile.json': new Uint8Array(), 'extra': new Uint8Array()}),
    fflate.zipSync({'../profile.json': new Uint8Array()}),
    fflate.zipSync({'profile.json': [new TextEncoder().encode(JSON.stringify(profile)), {level: 0}]}),
    zip(new Uint8Array(codec.limit + 1))]) {
    await assert.rejects(codec.decode(bytes));
  }
  await assert.rejects(codec.encode({...profile, huge: 'x'.repeat(codec.limit)}));
});
test('corrupted CRC and encrypted entries are rejected', async () => {
  const original = await codec.encode(profile);
  for (const offset of [14, 6]) {
    const bytes = original.slice();
    bytes[offset] ^= 1;
    await assert.rejects(codec.decode(bytes));
  }
});
test('decompression is bounded even if both ZIP headers lie about expanded size', async () => {
  const bytes = zip(new Uint8Array(codec.limit + 10000));
  const view = new DataView(bytes.buffer);
  const central = view.getUint32(bytes.length - 22 + 16, true);
  view.setUint32(22, 10, true);
  view.setUint32(central + 24, 10, true);
  await assert.rejects(codec.decode(bytes));
});

