const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const root = path.join(__dirname, '..');
const context = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(root, 'vesc-conf-layout.js'), 'utf8'), context);
vm.runInContext(fs.readFileSync(path.join(root, 'vesc-panel.js'), 'utf8'), context);
const VescPanel = vm.runInContext('VescPanel', context);
const VescConfLayout = vm.runInContext('VescConfLayout', context);

// Real DBG VCONF lines from the chair's 2026-11-22 tap (both VESCs, fw 6.06),
// and the same bytes decoded by tools/vesc_conf.py (the Python reference).
const realConf = fs.readFileSync(path.join(__dirname, 'fixtures', 'vconf-real.log'), 'utf8').trim().split('\n');
const reference = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', 'vconf-real-decoded.json'), 'utf8'));

// Lines in the firmware's formats (src/vesc_monitor.h; tests/native/vesc_monitor_test.cpp).
const vescLine = 'DBG VESC id=2 age=20 frames=1 erpm=-8000 current=-23.0 duty=0.123 fetC=35.2 vin=52.7 motC=-270.0 iin=-4.5 ' +
  'ipk=-48.0 iinpk=-6.1 ah=0.1234/0.0000 wh=6.432/0.000 tach=-987654 pid=0.0 ages=20/21/22/23/-1 side=L';
const vvalLine = 'DBG VVAL id=2 age=80 fault=0 fetC=25.4 motC=-270.0 imot=-41.23 iin=-3.21 iD=0.12 iQ=-41.23 duty=-0.456 ' +
  'erpm=-8123 vin=52.3 ah=0.1234/0.0567 wh=6.432/2.500 tach=-123456/654321 pid=0.00 mos=30.1/30.2/30.3 vD=1.23 ' +
  'vQ=-20.50 st=0 faults=1 last=4 n=12/11/1 lat=9 side=L';
const faultLine = 'DBG VFAULT id=2 code=4 name=ABS_OVER_CURRENT ms=2000 imot=-48.70 iin=-3.21 erpm=-8123 duty=-0.456 ' +
  'vin=52.3 fetC=25.4 n=1 side=L';

test('the generated layout matches the C source it came from', () => {
  const src = fs.readFileSync(path.join(root, '..', 'tools', 'vesc', 'confgenerator_6_06.c'), 'utf8');
  for (const kind of ['mcconf', 'appconf']) {
    const body = new RegExp(`confgenerator_serialize_${kind}\\(.*?\\{([\\s\\S]*?)\\n\\}`).exec(src)[1];
    const appends = body.split('\n').filter(l => /buffer\[ind\+\+\] =|buffer_append_/.test(l)).length;
    assert.equal(VescConfLayout[kind].fields.length, appends, kind);
  }
  assert.equal(VescConfLayout.mcconf.fields[0][0], 'MCCONF_SIGNATURE');
  assert.equal(VescConfLayout.mcconf.signature, 788332866);
  assert.equal(VescConfLayout.appconf.signature, 2099347128);
  assert.equal(VescConfLayout.mcconf.bytes, 483);
  assert.equal(VescConfLayout.appconf.bytes, 278);
});

test('float32_auto decodes like VESC buffer_get_float32_auto', () => {
  assert.equal(VescPanel.float32Auto(0), 0);
  // 32.68 A (l_current_max on the left VESC) in VESC's float32_auto encoding.
  const enc = v => { const b = new DataView(new ArrayBuffer(4)); b.setFloat32(0, v); return b.getUint32(0); };
  for (const v of [1, -1, 0.5, 32.68, -34.8133, 1e-6, 123456.7]) assert.ok(Math.abs(VescPanel.float32Auto(enc(v)) - Math.fround(v)) <= Math.abs(v) * 1e-6, v);
});

test('real config dumps decode field for field like tools/vesc_conf.py', () => {
  const state = VescPanel.createState();
  for (const line of realConf) assert.equal(VescPanel.handleLine(state, line, 1000), true, line);
  for (const [key, expected] of Object.entries(reference)) {
    const [id, kind] = key.split('/');
    const conf = state.vescs.get(Number(id)).conf[kind];
    assert.ok(conf.decoded, key);
    assert.equal(conf.decoded.signatureOk, true, key);
    assert.equal(conf.decoded.used, expected.used, key);
    assert.equal(conf.decoded.fields.length, expected.fields.length, key);
    expected.fields.forEach(([name, value], i) => {
      const [gotName, got] = conf.decoded.fields[i];
      assert.equal(gotName, name, `${key} field ${i}`);
      assert.ok(Math.abs(got - value) <= Math.abs(value) * 1e-9 + 1e-12, `${key} ${name}: ${got} vs ${value}`);
    });
  }
  const left = state.vescs.get(2), right = state.vescs.get(26);
  const field = (e, kind, name) => e.conf[kind].decoded.fields.find(([n]) => n === name)[1];
  assert.ok(Math.abs(field(left, 'mcconf', 'l_current_max') - 32.68) < 1e-4);
  assert.ok(Math.abs(field(right, 'mcconf', 'l_current_max') - 34.8133) < 1e-4);
  assert.equal(field(left, 'appconf', 'controller_id'), 2);
  assert.equal(field(right, 'appconf', 'controller_id'), 26);
  assert.equal(field(left, 'appconf', 'can_status_rate_1'), 50);
});

test('a garbled or partial dump is never shown as decoded', () => {
  const state = VescPanel.createState();
  const mc = realConf.filter(l => l.startsWith('DBG VCONF id=2 cmd=14 '));
  // Drop one chunk: incomplete.
  for (const line of mc.filter((_, i) => i !== 5)) VescPanel.handleLine(state, line, 0);
  assert.equal(state.vescs.get(2).conf.mcconf.decoded, null);
  assert.equal(state.vescs.get(2).conf.mcconf.got.size, 484 - 24);
  // A chunk cut short on the radio is rejected outright.
  assert.equal(VescPanel.parseConfChunk(mc[5].slice(0, -3)), null);
  assert.equal(VescPanel.handleLine(state, mc[5].slice(0, -3), 0), false);
  VescPanel.handleLine(state, mc[5], 0);
  assert.ok(state.vescs.get(2).conf.mcconf.decoded.signatureOk);
});

test('status, values and fault lines', () => {
  const s = VescPanel.parseStatus(vescLine);
  assert.equal(s.id, 2); assert.equal(s.side, 'L'); assert.equal(s.fetC, 35.2); assert.equal(s.motC, -270);
  assert.equal(s.ipk, -48); assert.equal(s.ah, 0.1234); assert.equal(s.whc, 0); assert.deepEqual([...s.ages], [20, 21, 22, 23, -1]);
  // Older firmware: the first eight fields only.
  const old = VescPanel.parseStatus('DBG VESC id=26 age=3 frames=5197 erpm=0 current=0.0 duty=0.000 fetC=22.5 vin=0.0');
  assert.equal(old.id, 26); assert.equal(old.fetC, 22.5); assert.equal(old.ages, null); assert.equal(old.side, null);
  // Garbled: tail lost and the next line run in.
  assert.equal(VescPanel.parseStatus('DBG VESC id=2 age=0 frames=0 erpm=0 current=0.0 duty=0.000 fetC=0.0 vin= 425 409'), null);
  assert.equal(VescPanel.parseStatus('DBG VESC id=2 age=1 frames=3 DBG VESC id=26 age=2'), null);

  const v = VescPanel.parseValues(vvalLine);
  assert.equal(v.fault, 0); assert.equal(v.lastFault, 4); assert.equal(v.faults, 1);
  assert.equal(v.asked, 12); assert.equal(v.lost, 1); assert.equal(v.tachAbs, 654321); assert.deepEqual([...v.mos], [30.1, 30.2, 30.3]);
  assert.equal(VescPanel.parseValues('DBG VVAL id=26 age=-1 n=0/0/0 side=R').age, -1);

  const f = VescPanel.parseFault(faultLine);
  assert.equal(f.name, 'ABS_OVER_CURRENT'); assert.equal(f.code, 4); assert.equal(f.imot, -48.7);
  assert.equal(VescPanel.parseFirmware('DBG VESCFW id=2 fw=6.06 hw=60_MK6 len=72 crc=1').hw, '60_MK6');
  assert.equal(VescPanel.handleLine(VescPanel.createState(), 'DBG R 1 2 3 4 5', 0), false);
  assert.equal(VescPanel.handleLine(VescPanel.createState(), 'DBG SENSORS raw=1/2/3/4', 0), false);
});

test('health: ok, stale, fault and no data', () => {
  const state = VescPanel.createState();
  VescPanel.handleLine(state, vescLine, 1000);
  VescPanel.handleLine(state, vvalLine, 1000);
  let h = VescPanel.health(state.vescs.get(2), 1100);
  assert.equal(h.state, 'ok');
  assert.equal(h.statusAge, 120);           // 20 ms old when sent + 100 ms since
  assert.equal(h.motC, null);               // -270 °C: no thermistor
  assert.ok(Math.abs(h.wheelRpm - -8000 / 58.1816) < 1e-6);
  assert.equal(h.lastFaultCode, 4);
  assert.equal(VescPanel.health(state.vescs.get(2), 1600).state, 'stale');
  assert.equal(VescPanel.health(state.vescs.get(2), 1000 + VescPanel.silentMs + 1).state, 'none');

  VescPanel.handleLine(state, faultLine, 1200);
  h = VescPanel.health(state.vescs.get(2), 1250);
  assert.equal(h.state, 'fault');
  assert.equal(h.lastFault.name, 'ABS_OVER_CURRENT');
  VescPanel.handleLine(state, faultLine.replace('code=4 name=ABS_OVER_CURRENT', 'code=0 name=NONE'), 1400);
  VescPanel.handleLine(state, vescLine, 7000);
  h = VescPanel.health(state.vescs.get(2), 7010);
  assert.equal(h.faultNow, false);          // cleared, and the hold has passed
  assert.equal(h.lastFault.name, 'ABS_OVER_CURRENT'); // still shown as the last fault
});

test('temperature and voltage warnings use the VESC\'s own limits once its settings arrive', () => {
  const state = VescPanel.createState();
  for (const line of realConf) VescPanel.handleLine(state, line, 0);
  const e = state.vescs.get(2);
  const fetStart = e.conf.mcconf.decoded.fields.find(([n]) => n === 'l_temp_fet_start')[1];
  VescPanel.handleLine(state, vescLine.replace('fetC=35.2', `fetC=${fetStart - 5}`), 0);
  assert.equal(VescPanel.health(e, 10).hot, true);
  VescPanel.handleLine(state, vescLine, 0);
  assert.equal(VescPanel.health(e, 10).hot, false);
});

test('rendering: cards in left/right order, settings table with differences, text escaped', () => {
  const state = VescPanel.createState();
  VescPanel.handleLine(state, vescLine.replace('id=2', 'id=26').replace('side=L', 'side=R'), 0);
  VescPanel.handleLine(state, vescLine, 0);
  VescPanel.handleLine(state, faultLine, 0);
  const cards = VescPanel.renderHealth(state, 50);
  assert.ok(cards.indexOf('Left · VESC 2') < cards.indexOf('Right · VESC 26'));
  assert.match(cards, /ABS_OVER_CURRENT/);
  assert.match(cards, /S5 —/);
  assert.match(VescPanel.renderHealth(VescPanel.createState(), 0), /Waiting for DBG VESC/);

  for (const line of realConf) VescPanel.handleLine(state, line, 0);
  VescPanel.handleLine(state, 'DBG VESCFW id=2 fw=6.06 hw=<b>x</b> len=72 crc=1', 0);
  const table = VescPanel.renderDetails(state);
  assert.match(table, /l_current_max/);
  assert.match(table, /<tr class="differs"><td>l_current_max<\/td><td>32\.68<\/td><td>34\.8133<\/td><\/tr>/);
  assert.ok(!table.includes('<b>x</b>') && table.includes('&lt;b&gt;x&lt;/b&gt;'));
  const only = VescPanel.renderDetails(state, {onlyDifferent: true});
  assert.ok(!only.includes('<td>pwm_mode</td>'));
  assert.match(only, /controller_id/);
  const filtered = VescPanel.renderDetails(state, {filter: 'TEMP_FET'});
  assert.match(filtered, /l_temp_fet_start/);
  assert.ok(!filtered.includes('l_current_max'));
});

test('the VESC panel is read-only: it has no way to send anything', () => {
  const source = fs.readFileSync(path.join(root, 'vesc-panel.js'), 'utf8');
  for (const forbidden of ['write', 'send', 'configLink', 'port', 'fetch', 'WebSocket', 'XMLHttpRequest', 'postMessage'])
    assert.ok(!new RegExp(`\\b${forbidden}\\b`).test(source.replace(/\/\/.*$/gm, '')), forbidden);
  const page = fs.readFileSync(path.join(root, 'index.html'), 'utf8');
  const vescPage = /<section id="vesc-page"[\s\S]*?<\/section>/.exec(page)[0];
  assert.ok(!/<button/.test(vescPage.replace(/<button type="button" id="vesc-only-different"[^>]*>/, '')), 'no action buttons on the VESC page');
});
