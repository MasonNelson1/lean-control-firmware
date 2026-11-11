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

1. Build and upload the updated `teensy41` firmware using your usual process. It sends the existing CSV rows over the HC-06 UART at 115200 baud and 20 Hz while in LEAN, including with USB disconnected.
2. Pair the wheelchair's HC-06 using the Bluetooth settings on the computer running your browser. Disconnect the handheld controller from the HC-06 first; it uses the same Bluetooth connection.
3. Start the dashboard as usual and open **http://localhost:8000** in desktop Chrome 117 or later. If accessing a server on another computer, use HTTPS; plain LAN HTTP cannot access Web Serial.
4. Select LEAN using the wheelchair's physical button, choose **Bluetooth** in the connection dropdown, click **Connect**, and select the paired HC-06. Selecting Bluetooth pauses the USB monitor and its reconnect attempts. The browser's computer must have Bluetooth; the server's USB connection is not needed for this source.
5. The graphs and badges now use Bluetooth data. **Disconnect** closes the selected connection without switching transports. To return to wired telemetry, choose **USB / replay** and click **Connect**. Switching dropdown options disconnects the old transport and waits for Connect. Data becomes stale after two seconds without telemetry, including when the chair switches to REMOTE; the last mode shown is then only the last observed mode.

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

Run dashboard protocol and Bluetooth tests with `node --test debugViewer/tests/*.test.cjs` from the firmware root.
