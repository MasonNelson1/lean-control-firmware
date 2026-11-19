# Lean debug viewer

## Install the app (desktop Chrome)

Once GitHub Pages is enabled, open
https://MasonNelson1.github.io/lean-control-firmware/ in desktop Chrome, then use
the address-bar Install icon or Chrome's **Install page as app** menu. The hosted
app uses Bluetooth directly; Python is not needed. Pair the HC-06 in your system
settings, disconnect other clients such as the handheld controller, then click
**Connect** and choose the receiver. No automatic connection is attempted.

Visit online once and allow the page to finish loading before using it offline.
The installed app can reopen without internet after its assets are cached.
Live telemetry and configuration still require the wheelchair connection.
Profiles remain local files and can be saved/loaded without internet once
settings have been discovered from the wheelchair.

Updates download during online visits and wait for all viewer windows to close.
Finish or save any edits, close every viewer tab/app window, and reopen to use
the update. Reloading or closing discards unapplied edits. The app never reloads
itself during a device session. Hardware access targets desktop Chrome; app
installation does not grant missing browser serial capabilities.

## Optional local server (USB/replay)

Use Python 3.10+ on Windows, Linux, or WSL. Wired monitoring uses a Linux or WSL
pseudo-terminal and PlatformIO (`pio` on PATH). Bluetooth also works here.

    python -m venv .venv
    .venv/bin/pip install -r requirements.txt
    .venv/bin/python app.py --replay device-monitor-261106-101548.log

On Windows, use `.venv\Scripts\python.exe` in place of `.venv/bin/python`.
On WSL, create the environment in your Linux home directory if a virtual
environment under `/mnt/c` has broken Python links.

For live use:

    .venv/bin/python app.py --port /dev/ttyACM0

Open http://localhost:8000 or http://HOST_LAN_IP:8000 from your network. Allow TCP 8000 through your host firewall. The USB monitor uses a pseudo-terminal and the `teensy41` environment. When log2file is enabled for that environment, its output goes into this directory's logs folder. Disconnect the monitor before uploading if your uploader needs exclusive access, then reconnect. Replay loops automatically.

The mode badge shows LEAN or REMOTE from the final `mode` CSV column. Older 12- and 14-column logs remain supported and show Unknown. A stale badge indicates telemetry has stopped; it does not confirm the device’s current mode.

For Bluetooth telemetry:

1. Build and upload the updated `teensy41` firmware using your usual process. It sends the existing CSV rows over the HC-06 UART at 115200 baud and 20 Hz in LEAN, REMOTE, and DISARM, including with USB disconnected.
2. Pair the wheelchair's HC-06 using the Bluetooth settings on the computer running your browser. Disconnect the handheld controller from the HC-06 first; it uses the same Bluetooth connection.
3. Start the dashboard as usual and open **http://localhost:8000** in desktop Chrome 117 or later. If accessing a server on another computer, use HTTPS; plain LAN HTTP cannot access Web Serial.
4. Choose **Bluetooth** in the connection dropdown, click **Connect**, and select the paired HC-06. Selecting Bluetooth pauses the USB monitor and its reconnect attempts. The browser's computer must have Bluetooth; the server's USB connection is not needed for this source.
5. The graphs and badges now use Bluetooth data. **Disconnect** closes the selected connection without switching transports. To return to wired telemetry, choose **USB / replay** and click **Connect**. Switching dropdown options disconnects the old transport and waits for Connect. Live sensor readings continue in every mode. They use the configured sensor floors, gains, and calibrated saturation limits; before calibration they use the default floor. Sensor sampling outside LEAN does not generate lean motor commands. Data becomes stale after two seconds without telemetry; the last mode shown is then only the last observed mode.

## Configuration over Bluetooth

After connecting, select **Configuration** or **Advanced Configuration**. The
Teensy supplies the groups, settings, current and default values, and validation
limits. The lean response editor lets you drag the knee or enter its coordinates.
Numerical settings pair a slider with an exact-value input and show the permitted
minimum and maximum. Hover over a question mark, or focus it with the keyboard,
to read the setting's description. PID groups show a simulated step response
that updates as you edit their tuning.

- **Apply changes** updates the running configuration. **Save to wheelchair**
  also commits it to EEPROM. **Revert to saved** discards edits and reloads the
  last saved configuration. Restore defaults is available for each group and
  for all settings.
- Switch the wheelchair to DISARM mode before changing settings. Accepted runtime
  changes take effect immediately. Apply, Save, Revert, and Restore defaults are
  rejected in other modes. The two VESC IDs must differ.
- Startup mode selects the drive mode at the next power-on. Save it to keep
  that selection after restarting.

Replay is available without the chair. Configuration requires a Bluetooth
connection to firmware with schema discovery enabled. See the
[firmware configuration guide](../docs/CONFIGURATION.md) for declaration examples,
EEPROM compatibility, and the protocol. Desktop Chrome uses
[Web Serial over Bluetooth Classic](https://developer.chrome.com/blog/serial-over-bluetooth).

### Configuration profiles

With settings loaded from the wheelchair, **Save profile** downloads a `.lean`
file containing all supported Configuration and Advanced settings, including
valid pending edits. **Load profile** stages recognized values as browser edits.
Review the amber highlights before using **Apply changes** or
**Save to wheelchair**. Saving or loading a profile sends no configuration writes
to the wheelchair. A highlight clears when the edit is applied or reset to the
current wheelchair value.

The `.lean` format is a standard ZIP with one DEFLATE-compressed UTF-8
`profile.json` entry (rename a copy to `.zip` to inspect it). Its JSON structure is:

```json
{
  "format": "lean-config",
  "version": 1,
  "fields": [
    {"id": 10, "name": "Front gain", "path": ["Sensor gains"], "type": 1, "value": "1.25"}
  ]
}
```

IDs and numeric type codes come from the existing device schema. All numeric
values use decimal strings to preserve precision, including 64-bit integers;
Boolean values use JSON booleans. Curves use arrays of numeric strings plus an
`editor` code and a `parameters` array of parameter names. Extra JSON properties
are ignored. Unknown fields, unsupported types/editors, and incompatible identity
metadata are skipped and counted. Missing fields keep their current values and
any existing browser edits. Older entries may omit identity metadata other than
`id` and `type`. Recognized fields still undergo the dashboard's normal range and
curve validation; an invalid recognized value leaves all existing edits intact.
Archive and expanded JSON sizes are limited to 1 MiB. The browser compresses and
decompresses files in memory with a bundled ZIP library. Profiles from older
Python-server versions remain compatible; the existing Python profile endpoints
are retained for older clients. Profiles require no firmware changes.

## Development and GitHub Pages deployment

Keep this folder in the existing monorepo. From debugViewer:

```sh
npm ci
npm test
npm run build
python -m http.server 8002 --bind 127.0.0.1 --directory dist
```

Open http://localhost:8002 to preview the static PWA. Unlike app.py, this starts
in Bluetooth mode and has no USB/replay option. The build uses only Node.js and
committed browser assets. The header uses the transparent full SVG, and the
favicon uses the wheelchair SVG. After changing it, regenerate installation
icons with `npx playwright install chromium` and `node make-icons.cjs` (Windows
uses installed Chrome; LEAN_BROWSER_CHANNEL can override the browser channel).

For browser verification, install the Python requirements in your viewer virtual
environment, run `npx playwright install chromium`, then `npm run test:browser`.
The harness checks the repository subpath, installation, offline loading, updates,
mocked Bluetooth configuration/profile operations, and local replay. To test
installed Chrome on Windows, use `$env:LEAN_BROWSER_CHANNEL='chrome'` first.

The repository-root `.github/workflows/debug-viewer-pages.yml` tests changes and
deploys the browser-only artifact from `wheelchair-firmware/debugViewer/dist`.
To enable publishing:

1. Commit the dashboard changes and workflow to the existing repository.
2. In GitHub **Settings → Pages → Build and deployment**, choose **GitHub Actions**.
3. Push to main or manually run **Debug viewer Pages** in the Actions tab.
4. Open the URL reported by the deploy job and install from desktop Chrome.

Pull requests run verification without deploying. Nothing in this workflow
builds or uploads wheelchair firmware. Logs, recordings, virtual environments,
and Python code are excluded by an explicit asset allowlist.

If you see an old version, close all viewer windows and reopen online. To clear
an installation manually, uninstall the app and clear this site's stored data
in Chrome (this also removes offline assets and site permissions). If Bluetooth
is unavailable, check that you are using desktop Chrome on localhost or HTTPS,
the receiver is paired, and no other app owns its serial connection.

Future agents and maintainers: read [ARCHITECTURE.md](ARCHITECTURE.md) before
changing runtime behavior, deployment, profiles, or offline caching.

Run dashboard protocol and Bluetooth tests with `node --test debugViewer/tests/*.test.cjs` from the firmware root.

Bluetooth on this Linux laptop: opening a selected Bluetooth port and disconnecting
it also asks the local Python server to run
`bluetoothctl disconnect 98:DA:60:0A:41:3C`, releasing the receiver's existing
Linux connection. Use localhost and run the server on the same computer as
Chrome. Cleanup errors appear in the console; Chrome can still attempt to connect.
Restart the server and reload the page after updating this feature.
