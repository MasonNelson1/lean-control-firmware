"""Read-only tap on the chair's telemetry, so Claude (or anyone) can tail it live.

Run with WINDOWS Python from WSL (WSL can't see Bluetooth or USB serial):
    python.exe tools/chair_tap.py            # auto-picks the paired HC-06's outgoing COM port
    python.exe tools/chair_tap.py --port COM7
    python.exe tools/chair_tap.py --list     # show serial ports

Reconnects by itself after an e-stop or power cycle.
Writes every telemetry line, prefixed with the laptop's time, to
logs/live/<date>-<time>.csv and mirrors it to logs/live/current.csv.
It never writes to the chair. The HC-06 takes one connection at a time:
disconnect the dashboard before starting this, and stop this before
reconnecting the dashboard.
"""

import argparse
import datetime as dt
import os
import shutil
import sys
import time

import serial
from serial.tools import list_ports

# Same columns the dashboard's app.py parses; the last column is the drive mode.
HEADER = ("host_time,time_ms,F,B,L,R,total,userWeight,speed,turn,leftTargetRPM,rightTargetRPM,"
          "leftSmoothedRPM,rightSmoothedRPM,stopped,leftActualRPM,rightActualRPM,"
          "leftBoostCurrent,rightBoostCurrent,mode")
LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "live")


def bluetooth_outgoing_ports():
    # Windows makes two COM ports per paired SPP device. The outgoing one (the
    # one that connects to the chair) carries the device's address in its hwid;
    # the incoming one has all zeros.
    found = []
    for p in list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "BTHENUM" in hwid and "000000000000" not in hwid:
            found.append(p)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        for p in list_ports.comports():
            print(f"{p.device:8} {p.description}  [{p.hwid}]")
        return

    port = args.port
    if not port:
        candidates = bluetooth_outgoing_ports()
        if len(candidates) != 1:
            print("Couldn't pick one Bluetooth port; pass --port. Ports:", file=sys.stderr)
            for p in list_ports.comports():
                print(f"  {p.device:8} {p.description}  [{p.hwid}]", file=sys.stderr)
            sys.exit(2)
        port = candidates[0].device

    os.makedirs(LOG_DIR, exist_ok=True)
    path = os.path.join(LOG_DIR, dt.datetime.now().strftime("%Y-%m-%d-%H%M%S") + ".csv")
    current = os.path.join(LOG_DIR, "current.csv")
    print(f"Tapping {port} at {args.baud} -> {os.path.normpath(path)}", flush=True)

    lines = skipped = 0
    with open(path, "w", buffering=1, newline="") as out, \
            open(current, "w", buffering=1, newline="") as cur:
        for f in (out, cur):
            f.write(HEADER + "\n")

        def note(text):
            # Tap events (link up/down) go in the log as text lines, like firmware messages.
            stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for f in (out, cur):
                f.write(f"{stamp},TAP {text}\n")
            print(f"{stamp} TAP {text}", flush=True)

        last_report = time.monotonic()
        while True:  # reconnect forever: the chair gets e-stopped and power-cycled
            try:
                ser = serial.Serial(port, args.baud, timeout=1)
            except serial.SerialException as e:
                time.sleep(3)
                continue
            note(f"link up on {port}")
            buf, last_data = b"", time.monotonic()
            try:
                while True:
                    chunk = ser.read(ser.in_waiting or 1)
                    if chunk:
                        last_data = time.monotonic()
                    elif time.monotonic() - last_data > 5:
                        raise serial.SerialException("no data for 5 s")
                    buf += chunk
                    *complete, buf = buf.split(b"\n")
                    for raw in complete:
                        # Config replies are binary COBS packets (0x00-delimited); keep text only.
                        if b"\x00" in raw:
                            raw = raw.split(b"\x00")[-1]
                        text = raw.decode("ascii", "replace").strip()
                        if not text or any(ord(c) < 32 for c in text):
                            skipped += 1
                            continue
                        stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        for f in (out, cur):
                            f.write(f"{stamp},{text}\n")
                        lines += 1
                    if time.monotonic() - last_report > 10:
                        print(f"{lines} lines, {skipped} skipped", flush=True)
                        last_report = time.monotonic()
            except (serial.SerialException, OSError) as e:
                note(f"link down ({e}); retrying")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
