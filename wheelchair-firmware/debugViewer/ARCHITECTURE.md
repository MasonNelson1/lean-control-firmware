# Debug viewer architecture and agent maintenance guide

Read this before modifying the dashboard. Everything here stays in the firmware
monorepo, but the hosted application has no dependency on firmware source or Python.
Do not modify firmware files, protocol layouts, identifiers, or Arduino.h for PWA
work. Any temporary Arduino.h changes made for other testing must be reverted
before finishing, as required by CLAUDE.md.

## Two runtime environments

The checked-in HTML is a static, Bluetooth-first application. `pwa.js` sets
`dashboardBackend` only when `window.LEAN_LOCAL_BACKEND === true`. The Python
server injects that flag before any dashboard JavaScript; hostname guessing and
backend probes are deliberately avoided. A static server on localhost is still
hosted mode. Do not add API calls to hosted startup.

| Behavior | Static/PWA | Python local server |
| --- | --- | --- |
| Initial source | Bluetooth, disconnected | Bluetooth, disconnected |
| Bluetooth | Browser Web Serial | Same browser Web Serial |
| USB/replay | Not offered | Python monitor/replay via `/ws` |
| Profiles | Wheelchair EEPROM over Web Serial | Same onboard storage |
| Service worker | Registered on page load | Not registered |

`index.html` owns telemetry plots and the optional server WebSocket.
`bluetooth.js` owns the serial reader, writer transport lifecycle, and the mixed
CSV/configuration stream. It delivers configuration frames to `configLink`.
`config-protocol.js` owns the protocol codec and request/reply matching.
`configuration.js` owns discovered schema, drafts, validation, forms, and actions.
Keep a single serial reader: adding another reader breaks configuration/CSV
demultiplexing. Disconnecting discards the connection-specific schema and drafts.

The firmware requires Disarm for active configuration writes and profile uploads.
The browser can edit drafts, stage defaults, and load onboard profiles in every
mode. On connection the browser queries only the profile catalog. An explicit
load fetches the selected slot's values over Web Serial.

## Legacy profile file codec

`profile-files.js` exposes asynchronous `LeanProfiles.encode(profile)` returning
ZIP bytes and `LeanProfiles.decode(bytes)` returning the parsed profile. The legacy codec uses the vendored, pinned fflate implementation.
There is no runtime CDN or npm dependency. See vendor/README.md for provenance
and how to update the library. npm dependencies are development tools only.

Files retain the `.lean` extension, one DEFLATE-compressed UTF-8 `profile.json`
entry, `format: "lean-config"`, and `version: 1`. All numbers are decimal strings;
Booleans are JSON booleans and curves are arrays of strings. Do not convert
64-bit numbers through JavaScript Number. Unknown fields are preserved by the
codec and skipped by schema staging; absent settings and unrelated drafts remain.
Recognized invalid values reject staging atomically. Existing identity checks
protect against loading incompatible settings with reused IDs.

The codec checks local/central ZIP metadata, filenames, entry count, compression,
encryption flags, decompressed length, CRC, UTF-8, and the JSON envelope. It caps
the archive and expanded content at 1 MiB and feeds compressed input in small
chunks to bound allocations even when ZIP sizes lie. Do not replace bounded
inflation with an unrestricted unzip call. Python `/profiles/export` and
`/profiles/import` remain legacy interfaces and interoperability references;
the dashboard no longer calls them.

## Assets, packaging, and offline updates

The original transparent SVGs are preserved as `assets/lean-full.svg` for the
header and `assets/lean-wheelchair.svg` for the favicon. `make-icons.cjs` renders
the wheelchair SVG into transparent 192- and 512-pixel installation icons, plus
a white-backed maskable variant with safe padding. Run it after npm ci and
installing Playwright Chromium, or set LEAN_BROWSER_CHANNEL to an installed
Chrome channel. The normal build uses the committed PNGs and needs no browser.

`build-pages.cjs` defines the complete publication/precache allowlist. Add browser
assets there when adding scripts, images, or fonts. Keep paths relative to the
application folder: GitHub Pages serves this app at a repository subpath. The
manifest uses its resolved start URL as the installation identity; do not set
`id: "./"`, which resolves at the origin and can collide with other Pages apps.
The Python server needs an exact route for every new browser asset; never expose its
whole directory with a broad static-files mount. Logs, recordings, environments,
node_modules, tests, Python files, and firmware must not enter the Pages artifact.
Build output refuses unknown files and symlinks rather than publishing them.

The build hashes asset filenames, contents, and the worker template to generate
a cache revision. `service-worker.js` exists in the build artifact, not source.
The worker precaches the complete release, serves only scoped application GETs,
and deletes older caches belonging to this app's scope when activated. It never
caches APIs, WebSockets, uploads, telemetry, or settings. Failed precaching must
not activate a partial release.

Do not add `skipWaiting()`, `clients.claim()`, automatic reloads, or cache-driven
device reconnection. An installed update waits until every old app window closes,
so an active session or pending drafts cannot be replaced. Newly installed
workers control subsequent navigations. Users should close all viewer windows
and reopen after an online visit to activate an update. Closing/reloading a tab
discards drafts and its Bluetooth connection; no draft persistence is promised.
Offline use requires a completed initial online cache. It does not synthesize
telemetry or allow configuration discovery without a connected wheelchair.

## Deployment and development

The workflow is at the repository root, `.github/workflows/debug-viewer-pages.yml`,
one level above wheelchair-firmware. It tests dashboard changes on pull requests,
builds on main/dispatch, uploads only dist, and publishes through the github-pages
environment. No firmware builds or uploads occur. Pages must be enabled with
GitHub Actions as its source. User-facing installation and commands are in README.

Run `npm ci`, `npm test`, and `npm run build` in debugViewer. Serve dist with any
static localhost server to inspect hosted behavior. Source-only serving does not
provide the generated worker. Run app.py to inspect local behavior. Use `-B` for
Python verification so tracked bytecode is not overwritten.

Run `python -B -m unittest discover -s tests` using the viewer virtual environment.
The existing pseudo-terminal connection test requires Linux; on Windows run
`test_app_profiles.py` and `test_app_telemetry.py` individually and report the
platform limitation rather than weakening tests or changing firmware headers.

Browser tests: install development dependencies, run `npx playwright install
chromium`, then `npm run test:browser`. To use installed desktop Chrome set
`LEAN_BROWSER_CHANNEL=chrome`. Set `LEAN_TEST_PYTHON` to override the test Python;
otherwise the harness looks for .venv-win on Windows, .venv on Linux, then python.
The tests launch isolated servers at 8123/8124, with repository-subpath hosting and
temporary release artifacts. The serial device and update endpoint exist only in
test code and must never be packaged. Chrome installability is checked with a
persistent profile because incognito contexts cannot install PWAs.

Coverage includes legacy/browser profile interoperability, malformed archives,
CRC and decompression limits, no hosted API calls, logo loading, configuration
edits with mocked serial frames, offline reopening, update waiting/activation,
and local replay. Test-only mocked serial proves browser wiring, not HC-06 hardware
behavior. Before shipping to riders, verify real pairing, telemetry/configuration,
DISARM rejections, disconnect/reconnect, and offline connection on desktop Chrome.
