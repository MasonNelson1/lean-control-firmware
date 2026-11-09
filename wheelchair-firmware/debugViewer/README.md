# Lean debug viewer

Linux Python 3.10+ and PlatformIO (`pio` on PATH) required.

    python -m venv .venv
    .venv/bin/pip install -r requirements.txt
    .venv/bin/python app.py --replay device-monitor-261106-101548.log

For live use:

    .venv/bin/python app.py --port /dev/ttyACM0

Open http://localhost:8000 or http://HOST_LAN_IP:8000 from your network. Allow TCP 8000 through your host firewall. The monitor uses a pseudo-terminal and the existing `simple` environment. Its log2file output goes into this directory's logs folder. Disconnect the monitor before uploading if your uploader needs exclusive access, then reconnect. Replay loops automatically. Existing firmware is never modified.

The mode badge shows LEAN or REMOTE from the final `mode` CSV column. Older 12- and 14-column logs remain supported and show Unknown. A stale badge indicates telemetry has stopped; it does not confirm the device’s current mode.
