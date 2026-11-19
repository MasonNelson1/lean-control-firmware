# Lean debug viewer

Use Python 3.10+ on Windows, Linux, or WSL for Bluetooth and replay. Wired
monitoring uses a Linux or WSL pseudo-terminal and PlatformIO (`pio` on PATH).

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
- Stop the chair before changing settings. Accepted runtime changes take
  effect immediately. Changes and saves are rejected if either smoothed wheel
  command exceeds 1 RPM in magnitude. The two VESC IDs must differ.
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
Archive and expanded JSON sizes are limited to 1 MiB. The dashboard server
compresses/decompresses files in memory; profiles require no firmware changes.

Run dashboard protocol and Bluetooth tests with `node --test debugViewer/tests/*.test.cjs` from the firmware root.

Bluetooth on this Linux laptop: opening a selected Bluetooth port and disconnecting
it also asks the local Python server to run
`bluetoothctl disconnect 98:DA:60:0A:41:3C`, releasing the receiver's existing
Linux connection. Use localhost and run the server on the same computer as
Chrome. Cleanup errors appear in the console; Chrome can still attempt to connect.
Restart the server and reload the page after updating this feature.
