"""Read and change the chair's firmware settings live, through the tap's bridge.

Run with WINDOWS Python from WSL (the bridge listens on Windows localhost):
    python.exe tools/chair_config.py list [--json]
    python.exe tools/chair_config.py get "turn reach"
    python.exe tools/chair_config.py set "braking current" -20 "max rpm" 3000
    python.exe tools/chair_config.py save        # persist RAM settings to EEPROM
    python.exe tools/chair_config.py revert      # drop unsaved RAM changes

Needs tools/chair_tap.py running (it owns the HC-06 and serves
ws://127.0.0.1:8766). Stdlib only. Speaks the same COBS/CRC configuration
protocol as debugViewer/config-protocol.js and src/config/Protocol.cpp:
`set` is the dashboard's Apply (RAM only, one field per request) and `save` is
its Save. The firmware decides what may change: Set/Save/Revert are refused
outside DISARM, and cross-field checks can reject a value. list/get only read.
"""

import argparse
import base64
import json
import math
import os
import random
import select
import socket
import struct
import sys
import time

# ---------------------------------------------------------------- wire format

VERSION = 1
REQUEST_TAG, RESPONSE_TAG = 0xC1, 0xC2
MAX_REQUEST, MAX_RESPONSE = 48, 400
CMD = dict(hello=1, group=2, describe=3, read=4, set=5, save=6, revert=7, defaults=8, preview=9)
READ_ONLY = {CMD["hello"], CMD["group"], CMD["describe"], CMD["read"], CMD["preview"]}
T_F32, T_F64, T_I8, T_U8, T_I16, T_U16, T_I32, T_U32, T_I64, T_U64, T_BOOL, T_ENUM, T_PROFILE = range(1, 14)
TYPE_NAMES = {T_F32: "f32", T_F64: "f64", T_I8: "i8", T_U8: "u8", T_I16: "i16", T_U16: "u16", T_I32: "i32",
              T_U32: "u32", T_I64: "i64", T_U64: "u64", T_BOOL: "bool", T_ENUM: "enum", T_PROFILE: "profile"}
SCALAR = {T_F32: "<f", T_F64: "<d", T_I8: "<b", T_U8: "<B", T_I16: "<h", T_U16: "<H", T_I32: "<i",
          T_U32: "<I", T_I64: "<q", T_U64: "<Q", T_BOOL: "<B", T_ENUM: "<B"}
EDITOR_SIGNED_POWER, EDITOR_PIECEWISE = 1, 2
STATUS = ["ok", "the chair uses a different configuration protocol version",
          "the chair rejected the request format", "this setting or group is unavailable",
          "the chair rejected the value or a combination of settings",
          "refused: switch the chair to DISARM and try again",
          "the chair could not save or restore the configuration",
          "this schema entry exceeds the protocol limit"]
NOT_DISARMED = 5


def crc16(data):
    """CRC-16/CCITT-FALSE, as in Protocol.cpp."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def cobs_encode(data):
    """COBS frame including its trailing zero (PacketSerial)."""
    out, block = bytearray(), bytearray()
    for byte in data:
        if byte == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(byte)
            if len(block) == 254:
                out += b"\xff" + block
                block = bytearray()
    out += bytes([len(block) + 1]) + block
    return bytes(out) + b"\x00"


def cobs_decode(data):
    """Decodes a frame without its delimiters; None when malformed."""
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        i += 1
        end = i + code - 1
        if code == 0 or end > len(data):
            return None
        chunk = data[i:end]
        if 0 in chunk:
            return None
        out += chunk
        i = end
        if code < 255 and i < len(data):
            out.append(0)
    return bytes(out)


def request_frame(seq, command, body=b""):
    data = bytes([REQUEST_TAG, VERSION, seq & 0xFF, command]) + bytes(body)
    if len(data) + 2 > MAX_REQUEST:
        raise ValueError("configuration request exceeds the protocol limit")
    return cobs_encode(data + struct.pack("<H", crc16(data)))


def response_frame(frame):
    """Decodes and checks one reply frame (no delimiters); None if damaged."""
    data = cobs_decode(frame)
    if not data or len(data) < 8 or len(data) > MAX_RESPONSE or data[0] != RESPONSE_TAG:
        return None
    if crc16(data[:-2]) != struct.unpack("<H", data[-2:])[0]:
        return None
    return dict(version=data[1], seq=data[2], command=data[3], status=data[4],
                dirty=bool(data[5] & 2), body=Reader(data[6:-2]))


class Reader:
    def __init__(self, data):
        self.data, self.offset = bytes(data), 0

    def get(self, type_):
        fmt = SCALAR[type_]
        size = struct.calcsize(fmt)
        if self.offset + size > len(self.data):
            raise ValueError("incomplete configuration reply")
        (value,) = struct.unpack_from(fmt, self.data, self.offset)
        self.offset += size
        if type_ == T_BOOL:
            if value > 1:
                raise ValueError("invalid Boolean value in reply")
            return bool(value)
        return value

    def u8(self):
        return self.get(T_U8)

    def u16(self):
        return self.get(T_U16)

    def text(self):
        size = self.u8()
        end = self.offset + size
        if end > len(self.data):
            raise ValueError("incomplete configuration reply")
        value = self.data[self.offset:end].decode("utf-8")
        self.offset = end
        return value

    def done(self):
        if self.offset != len(self.data):
            raise ValueError("unexpected configuration data")


def id_body(value):
    return struct.pack("<H", value)


def read_group(r):
    group = dict(id=r.u16(), parent=r.u16(), advanced=bool(r.u8()), name=r.text())
    r.done()
    return group


def read_value(field, r):
    if field["type"] != T_PROFILE:
        return r.get(field["type"])
    if r.u8() != field["editor"]:
        raise ValueError("the profile type changed")
    return [r.get(T_F32) for _ in field["parameters"]]


def read_field(r):
    """Mirror of ConfigWire.readField: header, type metadata, current value."""
    field = dict(id=r.u16(), group=r.u16(), type=r.u8(), restart=bool(r.u8() & 1),
                 name=r.text(), description=r.text(), units=r.text())
    t = field["type"]
    if t == T_PROFILE:
        field["editor"] = r.u8()
        if field["editor"] not in (EDITOR_SIGNED_POWER, EDITOR_PIECEWISE):
            field["unsupported"] = True
            return field
        count = r.u8()
        if count < 2 or count > 8 or count % 2 or (field["editor"] == EDITOR_SIGNED_POWER and count != 2):
            raise ValueError("invalid profile dimensions")
        field["parameters"] = [dict(name=r.text(), default=r.get(T_F32), min=r.get(T_F32), max=r.get(T_F32))
                               for _ in range(count)]
        field["default"] = [p["default"] for p in field["parameters"]]
    elif t in SCALAR:
        field["default"] = r.get(t)
        if t == T_ENUM:
            field["choices"] = [dict(value=r.u8(), name=r.text()) for _ in range(r.u8())]
        elif t != T_BOOL:
            field["min"], field["max"] = r.get(t), r.get(t)
    else:
        field["unsupported"] = True
        return field
    field["value"] = read_value(field, r)
    r.done()
    return field


def set_body(field, value):
    head = id_body(field["id"])
    if field["type"] == T_PROFILE:
        return head + bytes([field["editor"]]) + b"".join(struct.pack("<f", v) for v in value)
    return head + struct.pack(SCALAR[field["type"]], value)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def parse_value(field, raw):
    """Typed value from command-line text, checked against the firmware's limits."""
    t, raw = field["type"], raw.strip()
    if field.get("unsupported"):
        raise ValueError("this tool doesn't know how to edit this field type")
    if t == T_BOOL:
        low = raw.lower()
        if low in ("1", "true", "on", "yes"):
            return True
        if low in ("0", "false", "off", "no"):
            return False
        raise ValueError("enter true or false")
    if t == T_ENUM:
        for choice in field["choices"]:
            if raw.lower() == choice["name"].lower() or raw == str(choice["value"]):
                return choice["value"]
        names = ", ".join(f'{c["name"]} ({c["value"]})' for c in field["choices"])
        raise ValueError(f"choose one of: {names}")
    if t == T_PROFILE:
        parts = [p for p in raw.replace(";", ",").split(",") if p.strip()]
        params = field["parameters"]
        if len(parts) != len(params):
            raise ValueError(f"enter {len(params)} comma-separated numbers: " + ", ".join(p["name"] for p in params))
        values = [_number(part, T_F32, p["min"], p["max"], p["name"]) for part, p in zip(parts, params)]
        if field["editor"] == EDITOR_PIECEWISE and any(values[i] <= values[i - 2] for i in range(2, len(values), 2)):
            raise ValueError("profile inputs must increase from left to right")
        return values
    return _number(raw, t, field["min"], field["max"])


def _number(raw, t, lo, hi, label=None):
    raw = raw.strip()
    if t in (T_F32, T_F64):
        try:
            value = float(raw)
        except ValueError:
            raise ValueError(f"'{raw}' is not a number") from None
        if t == T_F32:
            value = f32(value)
        if not math.isfinite(value):
            raise ValueError("enter a finite number")
    else:
        try:
            value = int(raw)
        except ValueError:
            try:
                as_float = float(raw)
            except ValueError:
                raise ValueError(f"'{raw}' is not a number") from None
            if not as_float.is_integer():
                raise ValueError("enter a whole number") from None
            value = int(as_float)
    if value < lo or value > hi:
        where = f"{label}: " if label else ""
        raise ValueError(f"{where}{fmt_scalar(value, t)} is outside the firmware's limits "
                         f"{fmt_scalar(lo, t)} .. {fmt_scalar(hi, t)}")
    return value


def fmt_scalar(value, t):
    if isinstance(value, bool):
        return "true" if value else "false"
    if t in (T_F32, T_F64) or isinstance(value, float):
        return f"{value:.7g}" if t == T_F32 else repr(value)
    return str(value)


def fmt_value(field, value):
    t = field["type"]
    if value is None:
        return ""
    if t == T_ENUM:
        for choice in field["choices"]:
            if choice["value"] == value:
                return choice["name"]
        return str(value)
    if t == T_PROFILE:
        return ",".join(fmt_scalar(v, T_F32) for v in value)
    return fmt_scalar(value, t)


# ---------------------------------------------------------------- byte stream


class Receiver:
    """Port of bluetooth.js BluetoothReceiver: splits newline text telemetry
    from zero-delimited COBS configuration frames on one byte stream."""

    def __init__(self, on_line, on_frame, valid_frame=lambda f: True):
        self.on_line, self.on_frame, self.valid_frame = on_line, on_frame, valid_frame
        self.line, self.frame = bytearray(), None

    def push(self, data):
        for byte in data:
            if byte == 0:
                if self.frame is None:
                    self.line = bytearray()
                    self.frame = bytearray()
                else:
                    candidate = bytes(self.frame)
                    if candidate and len(candidate) <= 402 and self.valid_frame(candidate):
                        self.on_frame(candidate)
                        self.frame = None
                    else:
                        # This zero may open the next packet if a boundary was lost.
                        self.frame = bytearray()
            elif self.frame is not None:
                if len(self.frame) <= 402:
                    self.frame.append(byte)
            elif byte == 10:
                if len(self.line) <= 4096:
                    self.on_line(self.line.decode("utf-8", "replace").rstrip("\r"))
                self.line = bytearray()
            elif len(self.line) <= 4096:
                self.line.append(byte)


# ---------------------------------------------------------------- WebSocket


def ws_frame(payload, opcode=0x2, mask=None):
    """Client-to-server frame (always masked, as RFC 6455 requires)."""
    mask = os.urandom(4) if mask is None else mask
    n = len(payload)
    if n < 126:
        head = bytes([0x80 | opcode, 0x80 | n])
    elif n < 65536:
        head = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", n)
    else:
        head = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", n)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


class WsDecoder:
    """Incremental frame parser; feed() returns complete (opcode, payload) pairs."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        frames = []
        while True:
            if len(self.buf) < 2:
                return frames
            b0, b1 = self.buf[0], self.buf[1]
            n, pos = b1 & 0x7F, 2
            if n == 126:
                if len(self.buf) < 4:
                    return frames
                n, pos = struct.unpack_from(">H", self.buf, 2)[0], 4
            elif n == 127:
                if len(self.buf) < 10:
                    return frames
                n, pos = struct.unpack_from(">Q", self.buf, 2)[0], 10
            mask = None
            if b1 & 0x80:
                if len(self.buf) < pos + 4:
                    return frames
                mask, pos = bytes(self.buf[pos:pos + 4]), pos + 4
            if len(self.buf) < pos + n:
                return frames
            payload = bytes(self.buf[pos:pos + n])
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            del self.buf[:pos + n]
            frames.append((b0 & 0x0F, payload))


class BridgeError(Exception):
    pass


class ChairError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class Link:
    """One WebSocket connection to the tap's bridge, one request at a time
    (the firmware has a single request slot)."""

    def __init__(self, host="127.0.0.1", port=8766, timeout=3.0, verbose=False):
        self.verbose = verbose
        try:
            self.sock = socket.create_connection((host, port), timeout=timeout)
        except OSError as e:
            raise BridgeError(f"can't reach the tap's bridge at ws://{host}:{port} ({e}). "
                              "Is `python.exe tools/chair_tap.py` running (without --no-bridge)?") from None
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise BridgeError("bridge closed during the WebSocket handshake")
            head += chunk
        status_line, _, rest = head.partition(b"\r\n")
        if b" 101 " not in status_line + b" ":
            raise BridgeError(f"bridge refused the WebSocket upgrade: {status_line.decode('latin-1')}")
        self.ws = WsDecoder()
        self.replies = []
        self.receiver = Receiver(self._line, self.replies.append, lambda f: response_frame(f) is not None)
        self.dirty = None
        # A random start keeps our sequence numbers apart from a dashboard's.
        self.seq = random.randrange(256)
        leftover = rest.partition(b"\r\n\r\n")[2] if b"\r\n\r\n" in rest else b""
        if leftover:
            self._feed(leftover)

    def _line(self, line):
        if self.verbose and line.strip():
            print(f"  | {line}", file=sys.stderr)

    def _feed(self, data):
        for op, payload in self.ws.feed(data):
            if op in (0x1, 0x2):
                self.receiver.push(payload)
            elif op == 0x9:
                self.sock.sendall(ws_frame(payload, 0xA))
            elif op == 0x8:
                raise BridgeError("bridge closed the connection")

    def _pump(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        ready, _, _ = select.select([self.sock], [], [], remaining)
        if ready:
            data = self.sock.recv(65536)
            if not data:
                raise BridgeError("bridge closed the connection")
            self._feed(data)

    def request(self, command, body=b""):
        """Sends one request and returns the reply body Reader. Reads retry up to
        3 times; writes never retry (a lost ACK doesn't mean it wasn't applied)."""
        read_only = command in READ_ONLY
        attempts = 3 if read_only else 1
        timeout = 8.0 if command == CMD["save"] else 2.5
        for attempt in range(attempts):
            seq = self.seq
            self.seq = (self.seq + 1) & 0xFF
            self.replies.clear()
            self.sock.sendall(ws_frame(request_frame(seq, command, body)))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                self._pump(deadline)
                for frame in self.replies:
                    reply = response_frame(frame)
                    if reply and reply["seq"] == seq and reply["command"] == command:
                        self.dirty = reply["dirty"]
                        status = reply["status"] if reply["version"] == VERSION else 1
                        if status:
                            msg = STATUS[status] if status < len(STATUS) else f"rejected (status {status})"
                            raise ChairError(status, msg)
                        return reply["body"]
                self.replies.clear()
        if read_only:
            raise BridgeError("no configuration reply after 3 attempts. Is the chair on and the tap's link up?")
        raise BridgeError("no acknowledgement from the chair. The write may or may not have been applied; "
                          "run `get` to check.")

    def close(self):
        try:
            self.sock.sendall(ws_frame(b"", 0x8))
        except OSError:
            pass
        self.sock.close()


# ---------------------------------------------------------------- commands


def discover(link):
    info = link.request(CMD["hello"])
    group_count, field_count = info.u16(), info.u16()
    info.done()
    groups = {}
    for i in range(group_count):
        g = read_group(link.request(CMD["group"], id_body(i)))
        groups[g["id"]] = g
    fields = []
    for i in range(field_count):
        f = read_field(link.request(CMD["describe"], id_body(i)))
        f["group_path"] = group_path(groups, f["group"])
        fields.append(f)
    return groups, fields


def group_path(groups, gid):
    names = []
    while gid and gid in groups and len(names) < 16:
        names.append(groups[gid]["name"])
        gid = groups[gid]["parent"]
    return " / ".join(reversed(names))


def match(fields, query):
    """By exact id, exact name, then case-insensitive substring; refuses ambiguity."""
    q = query.strip()
    if q.isdigit():
        hit = [f for f in fields if f["id"] == int(q)]
        if hit:
            return hit[0]
    low = q.lower()
    exact = [f for f in fields if f["name"].lower() == low]
    if len(exact) == 1:
        return exact[0]
    hits = exact or [f for f in fields if low in f["name"].lower() or low in
                     (f["group_path"] + " / " + f["name"]).lower()]
    if not hits:
        raise SystemExit(f"No setting matches '{query}'. Try `list`.")
    if len(hits) > 1:
        lines = "\n".join(f"  {f['id']:>4}  {f['group_path']} / {f['name']}" for f in hits)
        raise SystemExit(f"'{query}' is ambiguous; use the id or a longer name:\n{lines}")
    return hits[0]


def limits(f):
    if f.get("unsupported"):
        return ""
    if f["type"] == T_ENUM:
        return " | ".join(f'{c["name"]}={c["value"]}' for c in f["choices"])
    if f["type"] == T_BOOL:
        return "true/false"
    if f["type"] == T_PROFILE:
        return "; ".join(f'{p["name"]} {p["min"]:.7g}..{p["max"]:.7g}' for p in f["parameters"])
    return f'{fmt_scalar(f["min"], f["type"])} .. {fmt_scalar(f["max"], f["type"])}'


def as_json(f):
    keys = ("id", "group", "group_path", "name", "type", "units", "value", "default", "min", "max",
            "choices", "parameters", "restart", "description", "unsupported")
    out = {k: f[k] for k in keys if k in f}
    out["type"] = TYPE_NAMES.get(f["type"], f["type"])
    return out


def show(f):
    unsupported = " (unsupported type)" if f.get("unsupported") else ""
    restart = "  [takes effect after restart]" if f.get("restart") else ""
    return (f"{f['id']:>4}  {f['group_path']} / {f['name']} = {fmt_value(f, f.get('value'))} {f['units']}".rstrip()
            + f"   (default {fmt_value(f, f.get('default'))}; {limits(f)}){unsupported}{restart}")


def cmd_list(link, args):
    _, fields = discover(link)
    if args.json:
        print(json.dumps(dict(dirty=link.dirty, fields=[as_json(f) for f in fields]), indent=1))
        return 0
    rows = [(str(f["id"]), f["group_path"], f["name"], fmt_value(f, f.get("value")),
             fmt_value(f, f.get("default")), limits(f), f["units"]) for f in fields]
    head = ("id", "group", "name", "value", "default", "limits", "units")
    widths = [min(max(len(r[i]) for r in rows + [head]), 40) for i in range(len(head))]
    for r in [head] + rows:
        print("  ".join(c.ljust(w) if i < len(head) - 1 else c for i, (c, w) in enumerate(zip(r, widths))).rstrip())
    print(f"\n{len(fields)} settings. Unsaved RAM changes: {'yes' if link.dirty else 'no'}.")
    return 0


def cmd_get(link, args):
    _, fields = discover(link)
    for query in args.names:
        f = match(fields, query)
        if args.json:
            print(json.dumps(as_json(f)))
        else:
            print(show(f))
            if f["description"]:
                print(f"      {f['description']}")
    return 0


def cmd_set(link, args):
    if len(args.pairs) % 2:
        raise SystemExit("set takes <name|id> <value> pairs.")
    _, fields = discover(link)
    updates = []
    # Validate every pair before the first write, like the dashboard's Apply.
    for query, raw in zip(args.pairs[::2], args.pairs[1::2]):
        f = match(fields, query)
        try:
            updates.append((f, parse_value(f, raw)))
        except ValueError as e:
            raise SystemExit(f"{f['name']}: {e}. Nothing was sent.")
    for i, (f, value) in enumerate(updates):
        old = f["value"]
        try:
            reply = link.request(CMD["set"], set_body(f, value))
            acked = read_value(f, reply)
            reply.done()
        except ChairError as e:
            done = f" ({i} earlier change(s) were applied and remain in RAM)" if i else " Nothing changed."
            print(f"REFUSED {f['name']}: {e}.{done}", file=sys.stderr)
            return 3 if e.status == NOT_DISARMED else 4
        f["value"] = acked
        units = f" {f['units']}" if f["units"] else ""
        print(f"{f['name']}: {fmt_value(f, old)} -> {fmt_value(f, acked)}{units}  (chair acknowledged)")
    print("Applied to RAM. Run `save` to keep it after power-off." if link.dirty else "Applied.")
    return 0


def cmd_simple(name, done_message):
    def run(link, args):
        try:
            link.request(CMD[name]).done()
        except ChairError as e:
            print(f"REFUSED {name}: {e}.", file=sys.stderr)
            return 3 if e.status == NOT_DISARMED else 4
        print(done_message)
        return 0
    return run


def main(argv=None):
    # Windows Python writes cp1252 to a pipe; units like "1/s²" need UTF-8 in WSL.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("-v", "--verbose", action="store_true", help="echo telemetry lines to stderr")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="every setting with value, default, limits")
    p.add_argument("--json", action="store_true")
    p.set_defaults(run=cmd_list)
    p = sub.add_parser("get", help="one or more settings by id or name")
    p.add_argument("names", nargs="+")
    p.add_argument("--json", action="store_true")
    p.set_defaults(run=cmd_get)
    p = sub.add_parser("set", help="apply <name|id> <value> pairs to RAM (DISARM only)")
    p.add_argument("pairs", nargs="+")
    p.set_defaults(run=cmd_set)
    p = sub.add_parser("save", help="persist the RAM settings (DISARM only)")
    p.set_defaults(run=cmd_simple("save", "Saved. Settings marked for restart take effect at next power-on."))
    p = sub.add_parser("revert", help="discard unsaved RAM changes, reload saved settings (DISARM only)")
    p.set_defaults(run=cmd_simple("revert", "Reverted to the saved settings."))
    args = ap.parse_args(argv)
    try:
        link = Link(args.host, args.port, verbose=args.verbose)
    except BridgeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    try:
        return args.run(link, args)
    except (BridgeError, ChairError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
