const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const context = vm.createContext({window: {devicePixelRatio: 1}});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'pid-preview.js'), 'utf8'), context);
const model = vm.runInContext('PidPreview', context);
const limits = [4.91744 * .45, 2.85];
const circumference = 74.75 * .0254;

// Reference RPMs from RPMSetpointPID::smoothRPM at the same sample indices,
// with a half-scale target beginning at 250 ms.
test('step preview matches the native firmware with default tuning', () => {
  const result = model.simulate([.035, .1, 0, 2, .02, 400, 5, circumference], limits);
  const reference = [[50, .01], [100, 13.051920891], [150, 30.633272171],
    [200, 33.660091400], [300, 34.836547852], [500, 34.963268280], [1000, 34.964527130]];
  for (const [index, rpm] of reference) {
    assert.ok(Math.abs(result.samples[index].rpm - rpm) < .002, 'sample ' + index);
  }
});

test('step preview matches firmware integral, derivative and jerk behavior', () => {
  const result = model.simulate([.08, .2, .5, .7, .03, 250, 10, circumference], limits);
  const reference = [[25, .025], [50, 8.774999619], [75, 28.509601593],
    [100, 36.025604248], [150, 35.184413910], [250, 35.058952332], [500, 35.041454315]];
  for (const [index, rpm] of reference) {
    assert.ok(Math.abs(result.samples[index].rpm - rpm) < .002, 'sample ' + index);
  }
});

test('step preview uses the device limits and rejects incomplete tuning', () => {
  const values = [.035, .1, 0, 2, 0, 400, 5, 2];
  const result = model.simulate(values, limits);
  const maxRPM = limits[0] * 30;
  assert.equal(result.target, maxRPM / 2);
  for (const sample of result.samples) assert.ok(Math.abs(sample.rpm) <= maxRPM);
  assert.throws(() => model.simulate([0, ...values.slice(1)], limits), /valid PID settings/);
});

test('PID plot keeps the same zero-to-fifty RPM axis for every response', () => {
  const labels = [];
  const drawing = new Proxy({
    fillText(text) { labels.push(text); }
  }, {get(target, key) { return target[key] || (() => {}); }});
  const canvas = {getBoundingClientRect: () => ({width: 720, height: 280}), getContext: () => drawing};
  model.draw(canvas, {target: 100, duration: 5, samples: [{time: 0, rpm: 0, target: 0}, {time: 5, rpm: 100, target: 100}]});
  assert.deepEqual(labels.slice(0, 6), ['0', '10', '20', '30', '40', '50']);
});
