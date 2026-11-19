const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const {build} = require('../../build-pages.cjs');
const destination = fs.mkdtempSync(path.join(os.tmpdir(), 'lean-browser-pages-'));
build(destination);
const prefix = '/lean-control-firmware/';
const types = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.png': 'image/png', '.svg': 'image/svg+xml', '.webmanifest': 'application/manifest+json'};
let revision = 0;
http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  if (pathname === '/__test-update' && request.method === 'POST') {
    const worker = path.join(destination, 'service-worker.js');
    fs.writeFileSync(worker, fs.readFileSync(worker, 'utf8').replace(
      'const cacheName = prefix + ', `const cacheName = prefix + "update${++revision}-" + `));
    response.writeHead(200).end(); return;
  }
  if (!pathname.startsWith(prefix)) { response.writeHead(404).end(); return; }
  const relative = pathname.slice(prefix.length) || 'index.html';
  const file = path.resolve(destination, relative);
  if (!file.startsWith(destination + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    response.writeHead(404).end(); return;
  }
  response.writeHead(200, {'Content-Type': types[path.extname(file)] || 'text/plain', 'Cache-Control': 'no-cache'});
  fs.createReadStream(file).pipe(response);
}).listen(8123, '127.0.0.1');
process.on('exit', () => fs.rmSync(destination, {recursive: true, force: true}));
