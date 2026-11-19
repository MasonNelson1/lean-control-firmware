const {defineConfig} = require('@playwright/test');
const path = require('node:path');
const fs = require('node:fs');
const windowsPython = path.join(__dirname, '.venv-win', 'Scripts', 'python.exe');
const linuxPython = path.join(__dirname, '.venv', 'bin', 'python');
const python = process.env.LEAN_TEST_PYTHON || (process.platform === 'win32' && fs.existsSync(windowsPython)
  ? windowsPython : process.platform !== 'win32' && fs.existsSync(linuxPython) ? linuxPython : 'python');
module.exports = defineConfig({
  testDir: './tests/browser',
  workers: 1,
  timeout: 30000,
  use: {baseURL: 'http://127.0.0.1:8123/lean-control-firmware/',
    launchOptions: process.env.LEAN_BROWSER_CHANNEL ? {channel: process.env.LEAN_BROWSER_CHANNEL} : {}},
  webServer: [
    {command: 'node tests/browser/serve.cjs', url: 'http://127.0.0.1:8123/lean-control-firmware/', reuseExistingServer: false},
    {command: `"${python}" -B app.py --host 127.0.0.1 --http-port 8124 --replay tests/browser/replay.log`,
      url: 'http://127.0.0.1:8124', reuseExistingServer: false}
  ]
});
