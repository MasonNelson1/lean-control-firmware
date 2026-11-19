const assert = require('node:assert/strict');
const {test} = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {build, assets} = require('../build-pages.cjs');
test('Pages artifact is allowlisted, repeatable, and content-versioned', () => {
  const destination = fs.mkdtempSync(path.join(os.tmpdir(), 'lean-pages-build-'));
  try {
    const first = build(destination), second = build(destination);
    assert.equal(first.revision, second.revision);
    for (const asset of assets) assert.ok(fs.existsSync(path.join(destination, asset)));
    assert.equal(fs.existsSync(path.join(destination, 'app.py')), false);
    assert.equal(fs.existsSync(path.join(destination, 'node_modules')), false);
    const worker = fs.readFileSync(path.join(destination, 'service-worker.js'), 'utf8');
    assert.ok(worker.includes(first.revision));
    assert.equal(worker.includes('__ASSETS__'), false);
    const html = fs.readFileSync(path.join(destination, 'index.html'), 'utf8');
    assert.equal(/(?:src|href)="\//.test(html), false);
    assert.equal(html.includes('window.LEAN_LOCAL_BACKEND = true'), false);
    const manifest = JSON.parse(fs.readFileSync(path.join(destination, 'manifest.webmanifest'), 'utf8'));
    assert.equal(manifest.start_url, './');
    assert.equal(manifest.scope, './');
    fs.writeFileSync(path.join(destination, 'private.log'), 'not for deployment');
    assert.throws(() => build(destination), /Unexpected build output/);
  } finally { fs.rmSync(destination, {recursive: true, force: true}); }
});
