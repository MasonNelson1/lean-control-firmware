# Lean debug viewer

Linux Python 3.10+ and PlatformIO (`pio` on PATH) required.

    python -m venv .venv
    .venv/bin/pip install -r requirements.txt
    .venv/bin/python app.py --replay device-monitor-261106-101548.log

For live use:

    .venv/bin/python app.py --port /dev/ttyACM0

Open http://localhost:8000 or http://HOST_LAN_IP:8000 from your network. Allow TCP 8000 through your host firewall. The USB monitor uses a pseudo-terminal and the `teensy41` environment. When log2file is enabled for that environment, its output goes into this directory's logs folder. Disconnect the monitor before uploading if your uploader needs exclusive access, then reconnect. Replay loops automatically.

The mode badge shows LEAN or REMOTE from the final `mode` CSV column. Older 12- and 14-column logs remain supported and show Unknown. A stale badge indicates telemetry has stopped; it does not confirm the device’s current mode.

For Bluetooth telemetry:

1. Build and upload the updated `teensy41` firmware using your usual process. It sends the existing CSV rows over the HC-06 UART at 115200 baud and 20 Hz while in LEAN, including with USB disconnected.
2. Pair the wheelchair's HC-06 using the Bluetooth settings on the computer running your browser. Disconnect the handheld controller from the HC-06 first; it uses the same Bluetooth connection.
3. Start the dashboard as usual and open **http://localhost:8000** in Chrome 117 or later on Linux. If accessing a server on another computer, use HTTPS; plain LAN HTTP cannot access Web Serial.
4. Select LEAN using the wheelchair's physical button, choose **Bluetooth** in the connection dropdown, click **Connect**, and select the paired HC-06. Selecting Bluetooth pauses the USB monitor and its reconnect attempts. The browser's computer must have Bluetooth; the server's USB connection is not needed for this source.
5. The graphs and badges now use Bluetooth data. **Disconnect** closes the selected connection without switching transports. To return to wired telemetry, choose **USB / replay** and click **Connect**. Switching dropdown options disconnects the old transport and waits for Connect. Data becomes stale after two seconds without telemetry, including when the chair switches to REMOTE; the last mode shown is then only the last observed mode.

This connection only reads telemetry. It sends no drive packets, changes no modes, and preserves the existing COBS/PacketSerial remote command protocol. The firmware change adds buffered, nonblocking UART telemetry in LEAN; motor control and button logic are unchanged. Web Serial's Bluetooth Classic SPP support is described in the [Chrome documentation](https://developer.chrome.com/blog/serial-over-bluetooth).

Run Bluetooth parser and connection lifecycle checks with `node --test debugViewer/tests/bluetooth.test.cjs` from the repository root.
