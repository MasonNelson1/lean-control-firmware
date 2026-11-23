"""Read-only tap on the chair's telemetry, so Claude (or anyone) can tail it live.

Run with WINDOWS Python from WSL (WSL can't see Bluetooth or USB serial):
    python.exe tools/chair_tap.py            # auto-picks the paired HC-06's outgoing COM port
    python.exe tools/chair_tap.py --port COM7
    python.exe tools/chair_tap.py --list     # show serial ports

Reconnects by itself after an e-stop or power cycle.
Writes every telemetry line, prefixed with the laptop's time, to
logs/live/<date>-<time>.csv and mirrors it to logs/live/current.csv.
The HC-06 takes one connection at a time, so the tap also runs a local bridge:
the dashboard's "Bluetooth via Claude's tap" option connects to
ws://127.0.0.1:8766 and gets every byte from the chair, and whatever the
dashboard sends (configuration packets) is written to the chair. The tap
itself never writes anything on its own. --no-bridge turns the bridge off.
"""

import argparse
import base64
import hashlib
import socket
import struct
import threading
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


class Bridge:
    """Minimal WebSocket server (stdlib only) relaying raw bytes between the
    chair's serial link and local dashboard pages."""
    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, port, note):
        self.clients, self.lock, self.ser, self.note = [], threading.Lock(), None, note
        self.ser_lock = threading.Lock()
        self.sent_to_chair = 0
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", port))
        srv.listen(4)
        threading.Thread(target=self._accept, args=(srv,), daemon=True).start()

    def _accept(self, srv):
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    @staticmethod
    def _recv_exact(conn, n):
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                raise ConnectionError("closed")
            data += chunk
        return data

    def _client(self, conn):
        try:
            req = b""
            while b"\r\n\r\n" not in req:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                req += chunk
            key = None
            for line in req.decode("latin-1").split("\r\n"):
                if line.lower().startswith("sec-websocket-key:"):
                    key = line.split(":", 1)[1].strip()
            if not key:
                conn.close()
                return
            accept = base64.b64encode(hashlib.sha1((key + self.GUID).encode()).digest()).decode()
            conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            with self.lock:
                self.clients.append(conn)
            self.note("bridge: dashboard connected")
            while True:
                b0, b1 = self._recv_exact(conn, 2)
                op, n = b0 & 0x0F, b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._recv_exact(conn, 2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._recv_exact(conn, 8))[0]
                mask = self._recv_exact(conn, 4) if b1 & 0x80 else b"\0\0\0\0"
                payload = bytes(c ^ mask[i % 4] for i, c in enumerate(self._recv_exact(conn, n)))
                if op == 0x8:
                    break
                if op == 0x9:
                    self._send(conn, payload, 0xA)
                elif op in (0x1, 0x2) and payload:
                    with self.ser_lock:
                        if self.ser is not None:
                            self.ser.write(payload)
                            self.sent_to_chair += len(payload)
        except (OSError, ConnectionError, ValueError):
            pass
        finally:
            with self.lock:
                if conn in self.clients:
                    self.clients.remove(conn)
                    self.note("bridge: dashboard disconnected")
            try:
                conn.close()
            except OSError:
                pass

    @staticmethod
    def _send(conn, payload, op=0x2):
        n = len(payload)
        head = bytes([0x80 | op]) + (bytes([n]) if n < 126 else
                                     bytes([126]) + struct.pack(">H", n) if n < 65536 else
                                     bytes([127]) + struct.pack(">Q", n))
        conn.sendall(head + payload)

    def broadcast(self, data):
        with self.lock:
            clients = list(self.clients)
        for c in clients:
            try:
                self._send(c, data)
            except OSError:
                with self.lock:
                    if c in self.clients:
                        self.clients.remove(c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--bridge-port", type=int, default=8766)
    ap.add_argument("--no-bridge", action="store_true")
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
    with open(path, "w", buffering=1, newline="", encoding="utf-8", errors="replace") as out, \
            open(current, "w", buffering=1, newline="", encoding="utf-8", errors="replace") as cur:
        for f in (out, cur):
            f.write(HEADER + "\n")

        def note(text):
            # Tap events (link up/down) go in the log as text lines, like firmware messages.
            stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for f in (out, cur):
                f.write(f"{stamp},TAP {text}\n")
            print(f"{stamp} TAP {text}", flush=True)

        bridge = None if args.no_bridge else Bridge(args.bridge_port, note)
        if bridge:
            print(f"Bridge for the dashboard on ws://127.0.0.1:{args.bridge_port}", flush=True)
        last_report = time.monotonic()
        while True:  # reconnect forever: the chair gets e-stopped and power-cycled
            try:
                ser = serial.Serial(port, args.baud, timeout=1)
            except serial.SerialException as e:
                time.sleep(3)
                continue
            note(f"link up on {port}")
            if bridge:
                with bridge.ser_lock:
                    bridge.ser = ser
            buf, last_data = b"", time.monotonic()
            try:
                while True:
                    chunk = ser.read(ser.in_waiting or 1)
                    if chunk:
                        last_data = time.monotonic()
                        if bridge:
                            bridge.broadcast(chunk)
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
                        extra = f", bridge {len(bridge.clients)} client(s), {bridge.sent_to_chair} B sent to chair" if bridge else ""
                        print(f"{lines} lines, {skipped} skipped{extra}", flush=True)
                        last_report = time.monotonic()
            except (serial.SerialException, OSError) as e:
                note(f"link down ({e}); retrying")
                if bridge:
                    with bridge.ser_lock:
                        bridge.ser = None
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
