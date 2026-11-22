'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

// Keep this allowlist explicit: never publish environments, logs, or recordings.
const assets = [
  'index.html', 'pwa.js', 'manifest.webmanifest', 'icons/192.png', 'icons/512.png', 'icons/512-maskable.png',
  'assets/lean-full.svg', 'assets/lean-wheelchair.svg',
  'vendor/fflate-0.8.3.js', 'vendor/fflate-LICENSE.txt', 'profile-files.js',
  'config-protocol.js', 'pid-preview.js', 'plate-view.js', 'vesc-conf-layout.js', 'vesc-panel.js',
  'configuration.js', 'bluetooth.js'
];
function build(output = path.join(__dirname, 'dist')) {
  const destination = path.resolve(output);
  // Refuse to overwrite source files, even when a custom output is supplied.
  if (assets.some(asset => path.join(destination, asset) === path.join(__dirname, asset)))
    throw new Error('Build output must differ from the source directory.');
  const allowed = new Set([...assets, 'service-worker.js', '.nojekyll']);
  function checkOutput(folder, prefix = '') {
    if (!fs.existsSync(folder)) return;
    for (const entry of fs.readdirSync(folder, {withFileTypes: true})) {
      const relative = prefix + entry.name;
      if (entry.isSymbolicLink()) throw new Error('Build output cannot contain symbolic links.');
      if (entry.isDirectory()) checkOutput(path.join(folder, entry.name), relative + '/');
      else if (!allowed.has(relative)) throw new Error(`Unexpected build output file: ${relative}`);
    }
  }
  checkOutput(destination);
  const hash = crypto.createHash('sha256');
  const template = fs.readFileSync(path.join(__dirname, 'service-worker.template.js'), 'utf8');
  hash.update(template);
  for (const asset of assets) {
    const bytes = fs.readFileSync(path.join(__dirname, asset));
    hash.update(asset).update(bytes);
    const target = path.join(destination, asset);
    fs.mkdirSync(path.dirname(target), {recursive: true});
    fs.writeFileSync(target, bytes);
  }
  const revision = hash.digest('hex').slice(0, 20);
  fs.writeFileSync(path.join(destination, 'service-worker.js'), template
    .replace('__REVISION__', JSON.stringify(revision)).replace('__ASSETS__', JSON.stringify(assets)));
  fs.writeFileSync(path.join(destination, '.nojekyll'), '');
  return {destination, revision, assets};
}
if (require.main === module) {
  const result = build(process.argv[2]);
  console.log(`Packaged ${result.assets.length} browser assets (${result.revision}) in ${result.destination}`);
}
module.exports = {build, assets};
