const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

// Minimal DOM adapter for individual controls. Layout and browser rendering
// belong to the existing real-browser checks, rather than this unit harness.
class Node {
  constructor(tag) {
    this.tagName = tag;
    this.children = [];
    this.dataset = {};
    this.attributes = {};
    this.listeners = {};
    this.style = {};
    this.classList = {add() {}, toggle() {}};
    this._value = '';
  }
  set value(value) { this._value = String(value); }
  get value() { return this._value; }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this.children = nodes; }
  get childElementCount() { return this.children.length; }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  getAttribute(name) { return this.attributes[name]; }
  addEventListener(name, callback) { (this.listeners[name] ||= []).push(callback); }
  fire(name) { for (const listener of this.listeners[name] || []) listener({}); }
  click() { this.onclick?.(); }
  getBoundingClientRect() { return {width: 440, height: 250}; }
  getContext() { return new Proxy({}, {get: () => () => {}}); }
}
function descendants(node, predicate) {
  return [node, ...node.children.flatMap(child => typeof child === 'object' ? descendants(child, predicate) : [])].filter(predicate);
}
function harness() {
  const ids = new Map();
  let confirmed = false;
  const prompts = [];
  const document = {
    createElement: tag => new Node(tag),
    getElementById: id => { if (!ids.has(id)) ids.set(id, new Node('div')); return ids.get(id); },
    querySelectorAll: () => []
  };
  const context = vm.createContext({
    document, TextDecoder, Uint8Array, DataView, setTimeout, clearTimeout,
    window: {addEventListener() {}, confirm(message) { prompts.push(message); return confirmed; }},
    graphs() {}, sensors() {}, lastRow: null
  });
  for (const file of ['config-protocol.js', 'pid-preview.js']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '..', file), 'utf8'), context);
  }
  const source = fs.readFileSync(path.join(__dirname, '..', 'configuration.js'), 'utf8');
  const hook = '  globalThis.controlsUnderTest = {numberInput, parseDraft, inputValue, renderField, restoreDefaults,' +
    ' draft(id) { return drafts.get(id); }, seed(items, entries) { groups = new Map(items); fields = new Map(entries); drafts.clear(); loaded = true; }};\n';
  assert.ok(source.includes('  window.configurationConnectionChanged(null);'));
  vm.runInContext(source.replace('  window.configurationConnectionChanged(null);', hook + '  window.configurationConnectionChanged(null);'), context);
  return {
    api: context.controlsUnderTest,
    types: vm.runInContext('ConfigWire.types', context),
    prompts,
    confirm(value) { confirmed = value; }
  };
}

for (const [name, raw, min, max, valid] of [
  ['empty', '', 0, 10, false], ['whitespace', '   ', 0, 10, false],
  ['minimum', '0', 0, 10, true], ['maximum', '10', 0, 10, true],
  ['below', '-.01', 0, 10, false], ['above', '10.01', 0, 10, false],
  ['nan', 'NaN', 0, 10, false], ['positive infinity', 'Infinity', 0, 10, false],
  ['negative infinity', '-Infinity', 0, 10, false], ['scientific', '1e-2', 0, 10, true],
  ['negative zero', '-0', 0, 10, true], ['negative valid', '-4.25', -10, 10, true]
]) test('numeric editor validation / ' + name, () => {
  const {api, types} = harness();
  const action = () => api.numberInput(raw, types.f32, min, max);
  if (valid) assert.doesNotThrow(action);
  else assert.throws(action);
});
for (const [name, type, raw, valid] of [
  ['int fractional', 'i32', '2.5', false], ['int whole', 'i32', '-2', true],
  ['u64 fractional', 'u64', '2.5', false], ['u64 exponent', 'u64', '1e3', false],
  ['u64 negative', 'u64', '-1', false], ['u64 maximum', 'u64', '18446744073709551615', true],
  ['u64 overflow', 'u64', '18446744073709551616', false]
]) test('integer input validation / ' + name, () => {
  const {api, types} = harness();
  const action = () => api.numberInput(raw, types[type], type === 'i32' ? -100 : 0n,
    type === 'i32' ? 100 : 18446744073709551615n);
  if (valid) assert.doesNotThrow(action);
  else assert.throws(action);
});

function scalar(h, overrides = {}, groupName = 'Sensor gains') {
  const group = {id: 1, parent: 0, name: groupName, advanced: false};
  const field = {id: 10, group: 1, name: 'Front gain', type: h.types.f32,
    description: 'Scales front input.', units: '×', min: 0, max: 100, value: 1, default: 1, ...overrides};
  h.api.seed([[1, group]], [[field.id, field]]);
  const node = h.api.renderField(field);
  const input = descendants(node, node => node.tagName === 'input' && node.type === 'number')[0];
  const slider = descendants(node, node => node.type === 'range')[0];
  const buttons = descendants(node, node => node.tagName === 'button');
  return {node, input, slider, buttons, field};
}
test('gain slider stops at five while numerical input keeps firmware maximum', () => {
  const h = harness(), control = scalar(h);
  assert.equal(control.slider.max, '5');
  assert.equal(control.input.max, '100');
  control.input.value = '75'; control.input.fire('input');
  assert.equal(control.input.getAttribute('aria-invalid'), 'false');
  assert.equal(h.api.draft(10), '75');
});
for (const name of ['Left VESC ID', 'Right VESC ID', 'Wheel circumference']) {
  test(name + ' has a numerical box without a slider', () => {
    const h = harness(), control = scalar(h, {name}, 'Motor configuration');
    assert.equal(control.slider, undefined);
    assert.ok(control.input);
  });
}
for (const [value, display] of [[.035, '.04'], [.001, '0'], [1.23456, '1.23'], [99.999, '100']]) {
  test('rounding preserves exact configured value ' + value, () => {
    const h = harness(), control = scalar(h, {value});
    assert.equal(Number(control.input.value), Number(display));
    assert.equal(Number(h.api.inputValue(control.input)), value);
    assert.equal(h.api.draft(10), undefined);
  });
}
test('reset to current removes a draft without confirmation', () => {
  const h = harness(), c = scalar(h, {value: 2, default: 1});
  c.input.value = '8'; c.input.fire('input');
  c.buttons.find(button => button.title?.includes('current')).click();
  assert.equal(c.input.value, '2');
  assert.equal(h.api.draft(10), undefined);
  assert.equal(h.prompts.length, 0);
});
test('cancel default reset preserves the draft', () => {
  const h = harness(), c = scalar(h, {value: 2, default: 1});
  c.input.value = '8'; c.input.fire('input');
  c.buttons.find(button => button.title?.includes('firmware default')).click();
  assert.equal(c.input.value, '8');
  assert.equal(h.api.draft(10), '8');
  assert.equal(h.prompts.length, 1);
});
test('confirmed default reset creates an unapplied default draft', () => {
  const h = harness(), c = scalar(h, {value: 2, default: 1});
  h.confirm(true);
  c.buttons.find(button => button.title?.includes('firmware default')).click();
  assert.equal(c.input.value, '1');
  assert.equal(h.api.draft(10), '1');
});
test('group defaults require confirmation before an operation starts', async () => {
  const h = harness(); scalar(h);
  await h.api.restoreDefaults(1);
  assert.equal(h.prompts.length, 1);
  assert.match(h.prompts[0], /Sensor gains/);
});
test('units appear in the field title', () => {
  const h = harness(), c = scalar(h);
  assert.ok(descendants(c.node, node => node.tagName === 'label').some(node => node.textContent === 'Front gain (×)'));
});
test('field descriptions use an accessible question mark', () => {
  const h = harness(), c = scalar(h);
  assert.ok(c.buttons.some(button => button.textContent === '?' && button.getAttribute('aria-describedby')));
  assert.ok(descendants(c.node, node => node.getAttribute('role') === 'tooltip').length);
});
test('unknown field types render explanation without an editor', () => {
  const h = harness(), c = scalar(h, {unsupported: true});
  assert.equal(c.input, undefined);
  assert.equal(c.slider, undefined);
});

function powerField(h) {
  return {type: h.types.profile, editor: 1, parameters: [
    {min: Math.fround(.001), max: Math.fround(.999)},
    {min: Math.fround(.001), max: Math.fround(.999)}
  ]};
}
for (const [name, raw, valid] of [
  ['attenuation', ['.5', '.25'], true], ['amplification', ['.25', '.75'], true],
  ['linear', ['.5', '.5'], true], ['zero input', ['0', '.5'], false],
  ['unit input', ['1', '.5'], false], ['zero output', ['.5', '0'], false],
  ['unit output', ['.5', '1'], false], ['nan knee', ['NaN', '.5'], false]
]) test('curve draft / ' + name, () => {
  const h = harness(), action = () => h.api.parseDraft(powerField(h), raw);
  if (valid) assert.doesNotThrow(action);
  else assert.throws(action);
});
test('piecewise draft rejects coincident inputs', () => {
  const h = harness();
  const field = {type: h.types.profile, editor: 2, parameters: Array.from({length: 6}, () => ({min: 0, max: 1}))};
  assert.throws(() => h.api.parseDraft(field, ['0', '0', '.5', '.5', '.5', '1']), /increase/);
});
