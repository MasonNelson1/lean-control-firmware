"""Tests for tools/chair_config.py: WebSocket framing, COBS/CRC, request and
reply encoding, the text/frame splitter and value validation. Hex vectors were
produced by debugViewer/config-protocol.js (ConfigWire) so both sides agree."""

import base64
import contextlib
import hashlib
import io
import os
import socket
import struct
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import chair_config as cc  # noqa: E402


def text(value):
    data = value.encode()
    return bytes([len(data)]) + data


def f32(value):
    return struct.pack("<f", value)


def reply(seq, command, body=b"", version=1, status=0, flags=2):
    """Reply frame without delimiters, like the JS test helper."""
    data = bytes([0xC2, version, seq, command, status, flags]) + body
    return cc.cobs_encode(data + struct.pack("<H", cc.crc16(data)))[:-1]


def parse(command, body, reader):
    return reader(cc.response_frame(reply(1, command, body))["body"])


class Wire(unittest.TestCase):
    def test_crc_matches_ccitt_false(self):
        self.assertEqual(cc.crc16(b"123456789"), 0x29B1)

    def test_request_frames_match_js(self):
        self.assertEqual(cc.request_frame(42, cc.CMD["read"], cc.id_body(0x1234)).hex(), "09c1012a043412d62700")
        self.assertEqual(cc.request_frame(0, cc.CMD["hello"]).hex(), "03c1010401c16600")
        self.assertEqual(cc.request_frame(255, cc.CMD["save"]).hex(), "07c101ff06d91500")

    def test_request_size_limit(self):
        with self.assertRaises(ValueError):
            cc.request_frame(0, cc.CMD["set"], bytes(43))

    def test_set_bodies_match_js(self):
        self.assertEqual(cc.set_body(dict(id=2, type=cc.T_F32), 0.05).hex(), "0200cdcc4c3d")
        self.assertEqual(cc.set_body(dict(id=40, type=cc.T_U16), 100).hex(), "28006400")
        self.assertEqual(cc.set_body(dict(id=90, type=cc.T_ENUM), 1).hex(), "5a0001")
        self.assertEqual(cc.set_body(dict(id=7, type=cc.T_I32), -5).hex(), "0700fbffffff")
        profile = dict(id=30, type=cc.T_PROFILE, editor=cc.EDITOR_SIGNED_POWER)
        self.assertEqual(cc.set_body(profile, [0.75, 0.5]), bytes([30, 0, 1]) + f32(0.75) + f32(0.5))

    def test_cobs_round_trip_and_block_boundaries(self):
        long = bytes(0 if i == 320 else 0x42 for i in range(385))
        self.assertEqual(cc.cobs_decode(cc.cobs_encode(long)[:-1]), long)
        no_zero = bytes(range(1, 256)) * 2
        encoded = cc.cobs_encode(no_zero)
        self.assertEqual(encoded[0], 0xFF)
        self.assertNotIn(0, encoded[:-1])
        self.assertEqual(cc.cobs_decode(encoded[:-1]), no_zero)
        for sample in (b"", b"\x00", b"\x00\x00", b"\x11\x00\x22", bytes(254 * [7]), bytes(255 * [7])):
            self.assertEqual(cc.cobs_decode(cc.cobs_encode(sample)[:-1]), sample)

    def test_cobs_rejects_malformed(self):
        self.assertIsNone(cc.cobs_decode(bytes([4, 0x42])))
        self.assertIsNone(cc.cobs_decode(bytes([0, 1])))

    def test_response_rejects_corruption(self):
        good = reply(42, cc.CMD["read"], b"\x03")
        self.assertEqual(cc.response_frame(good)["seq"], 42)
        bad = bytearray(good)
        bad[-1] ^= 1
        self.assertIsNone(cc.response_frame(bytes(bad)))

    def test_response_header(self):
        r = cc.response_frame(reply(7, cc.CMD["set"], status=5, flags=0))
        self.assertEqual((r["seq"], r["command"], r["status"], r["dirty"]), (7, cc.CMD["set"], 5, False))

    def test_read_group_and_fields_like_js(self):
        group = parse(cc.CMD["group"], bytes([2, 0, 1, 0, 1]) + text("Child"), cc.read_group)
        self.assertEqual((group["name"], group["parent"], group["advanced"]), ("Child", 1, True))

        enum = parse(cc.CMD["describe"], bytes([1, 0, 2, 0, cc.T_ENUM, 1]) + text("Mode") + text("Description")
                     + text("") + bytes([7, 2, 2]) + text("Lean") + bytes([7]) + text("Remote") + bytes([2]),
                     cc.read_field)
        self.assertEqual(enum["default"], 7)
        self.assertEqual(enum["choices"][0]["name"], "Lean")
        self.assertEqual(enum["value"], 2)
        self.assertTrue(enum["restart"])

        profile = parse(cc.CMD["describe"], bytes([30, 0, 2, 0, cc.T_PROFILE, 0]) + text("Another curve")
                        + text("Description") + text("") + bytes([cc.EDITOR_SIGNED_POWER, 2])
                        + text("Input") + f32(0.5) + f32(0.125) + f32(0.875)
                        + text("Output") + f32(0.25) + f32(0.125) + f32(0.875)
                        + bytes([cc.EDITOR_SIGNED_POWER]) + f32(0.625) + f32(0.375), cc.read_field)
        self.assertEqual(profile["parameters"][1]["name"], "Output")
        self.assertEqual(profile["value"], [0.625, 0.375])

        unknown = parse(cc.CMD["describe"], bytes([99, 0, 2, 0, 0xFE, 0]) + text("Future field")
                        + text("Description") + text("") + bytes([12, 34]), cc.read_field)
        self.assertTrue(unknown["unsupported"])

    def test_scalar_field_and_trailing_data(self):
        body = (bytes([40, 0, 3, 0, cc.T_U16, 0]) + text("Controller timeout") + text("d") + text("ms")
                + struct.pack("<HHHH", 100, 30, 500, 120))
        field = parse(cc.CMD["describe"], body, cc.read_field)
        self.assertEqual((field["default"], field["min"], field["max"], field["value"]), (100, 30, 500, 120))
        with self.assertRaises(ValueError):
            parse(cc.CMD["describe"], body + b"\x01", cc.read_field)


class Validation(unittest.TestCase):
    f = dict(id=2, type=cc.T_F32, min=cc.f32(0.01), max=1.0)
    u = dict(id=40, type=cc.T_U16, min=30, max=500)
    e = dict(id=90, type=cc.T_ENUM, choices=[dict(value=0, name="PLUS"), dict(value=1, name="CORNER")])
    p = dict(id=36, type=cc.T_PROFILE, editor=cc.EDITOR_PIECEWISE, parameters=[
        dict(name=n, min=0.0, max=2.0) for n in "abcd"])

    def test_limits_enforced(self):
        self.assertAlmostEqual(cc.parse_value(self.f, "0.05"), 0.05, places=6)
        self.assertEqual(cc.parse_value(self.f, "0.01"), cc.f32(0.01))  # printed min is accepted
        for raw in ("1.5", "0.001", "nan", "inf", "abc"):
            with self.assertRaises(ValueError):
                cc.parse_value(self.f, raw)
        self.assertEqual(cc.parse_value(self.u, "100"), 100)
        self.assertEqual(cc.parse_value(self.u, "100.0"), 100)
        for raw in ("29", "501", "100.5"):
            with self.assertRaises(ValueError):
                cc.parse_value(self.u, raw)

    def test_enum_bool_profile(self):
        self.assertEqual(cc.parse_value(self.e, "corner"), 1)
        self.assertEqual(cc.parse_value(self.e, "0"), 0)
        with self.assertRaises(ValueError):
            cc.parse_value(self.e, "2")
        self.assertIs(cc.parse_value(dict(type=cc.T_BOOL), "on"), True)
        with self.assertRaises(ValueError):
            cc.parse_value(dict(type=cc.T_BOOL), "maybe")
        self.assertEqual(cc.parse_value(self.p, "0,1,1,2"), [0.0, 1.0, 1.0, 2.0])
        for raw in ("0,1,0,2", "0,1,1", "0,1,1,3"):
            with self.assertRaises(ValueError):
                cc.parse_value(self.p, raw)

    def test_match(self):
        fields = [dict(id=72, name="Integral gain", group_path="PID / Normal"),
                  dict(id=82, name="Integral gain", group_path="PID / Transitions"),
                  dict(id=40, name="Controller timeout", group_path="Timing")]
        self.assertEqual(cc.match(fields, "timeout")["id"], 40)
        self.assertEqual(cc.match(fields, "82")["id"], 82)
        self.assertEqual(cc.match(fields, "normal / integral")["id"], 72)
        with self.assertRaises(SystemExit):
            cc.match(fields, "integral")
        with self.assertRaises(SystemExit):
            cc.match(fields, "nothing")


class Stream(unittest.TestCase):
    def test_receiver_splits_text_and_frames(self):
        lines, frames = [], []
        rx = cc.Receiver(lines.append, frames.append, lambda f: cc.response_frame(f) is not None)
        frame = reply(3, cc.CMD["read"], b"\x05")
        stream = b"DBG R 1 2 3 4\r\n" + b"\x00" + frame + b"\x00" + b"1,2,3\n"
        for i in range(0, len(stream), 5):  # arbitrary chunking
            rx.push(stream[i:i + 5])
        self.assertEqual(lines, ["DBG R 1 2 3 4", "1,2,3"])
        self.assertEqual(frames, [frame])

    def test_receiver_resyncs_after_garbage(self):
        frames = []
        rx = cc.Receiver(lambda _: None, frames.append, lambda f: cc.response_frame(f) is not None)
        frame = reply(9, cc.CMD["hello"], b"\x01\x00\x02\x00")
        rx.push(b"\x00\x05\x06\x07\x00" + frame + b"\x00")
        self.assertEqual(frames, [frame])


class WebSocket(unittest.TestCase):
    def test_client_frames_are_masked(self):
        mask = b"\x01\x02\x03\x04"
        self.assertEqual(cc.ws_frame(b"abc", 0x2, mask), bytes([0x82, 0x83]) + mask + bytes([0x60, 0x60, 0x60]))
        mid = cc.ws_frame(bytes(300), 0x2, mask)
        self.assertEqual(mid[:4], bytes([0x82, 0xFE, 0x01, 0x2C]))
        self.assertEqual(len(mid), 4 + 4 + 300)

    def test_decoder_round_trip_and_partial_feeds(self):
        def server(payload, op=0x2):  # as chair_tap.Bridge._send writes them
            n = len(payload)
            head = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n))
            return head + payload

        stream = server(b"hi") + server(bytes(range(200)) * 2) + server(b"", 0x9)
        dec = cc.WsDecoder()
        out = []
        for b in stream:
            out += dec.feed(bytes([b]))
        self.assertEqual(out, [(0x2, b"hi"), (0x2, bytes(range(200)) * 2), (0x9, b"")])
        masked = cc.WsDecoder().feed(cc.ws_frame(b"hello", 0x1))
        self.assertEqual(masked, [(0x1, b"hello")])


class FakeChair:
    """A WebSocket bridge plus a two-field firmware, with telemetry noise
    between every reply. Records every command it receives."""

    def __init__(self, disarmed=True):
        self.disarmed, self.commands = disarmed, []
        self.timeout = 100  # field 40, u16 30..500
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    def describe(self, index):
        if index == 0:
            return (bytes([40, 0, 1, 0, cc.T_U16, 0]) + text("Controller timeout") + text("") + text("ms")
                    + struct.pack("<HHHH", 100, 30, 500, self.timeout))
        return (bytes([90, 0, 1, 0, cc.T_ENUM, 0]) + text("Sensor layout") + text("") + text("")
                + bytes([1, 2, 0]) + text("PLUS") + bytes([1]) + text("CORNER") + bytes([1]))

    def answer(self, data):
        seq, command, body = data[2], data[3], data[4:-2]
        self.commands.append(command)
        status, out = 0, b""
        if command == cc.CMD["hello"]:
            out = struct.pack("<HH", 1, 2)
        elif command == cc.CMD["group"]:
            out = bytes([1, 0, 0, 0, 0]) + text("Timing")
        elif command == cc.CMD["describe"]:
            out = self.describe(body[0])
        elif command in (cc.CMD["set"], cc.CMD["save"]):
            if not self.disarmed:
                status = 5
            elif command == cc.CMD["set"]:
                self.timeout = struct.unpack("<H", body[2:4])[0]
                out = body[2:4]
        packet = bytes([0xC2, 1, seq, command, status, 2]) + out
        return b"12,LEAN\n\x00" + cc.cobs_encode(packet + struct.pack("<H", cc.crc16(packet))) + b"DBG X\n"

    def serve(self):
        conn, _ = self.srv.accept()
        self.srv.close()
        with conn:
            self.session(conn)

    def session(self, conn):
        head = b""
        while b"\r\n\r\n" not in head:
            head += conn.recv(4096)
        key = [l.split(":", 1)[1].strip() for l in head.decode().split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        conn.sendall(f"HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: {accept}\r\n\r\n".encode())
        dec = cc.WsDecoder()
        while True:
            data = conn.recv(4096)
            if not data:
                return
            for op, payload in dec.feed(data):
                if op == 0x8:
                    return
                packet = cc.cobs_decode(payload[:-1])
                out = self.answer(packet)
                conn.sendall(bytes([0x82, 126]) + struct.pack(">H", len(out)) + out)


def run_cli(chair, *argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = cc.main(["--port", str(chair.port), *argv])
        except SystemExit as e:
            code = e.code
    return code, out.getvalue(), err.getvalue()


class EndToEnd(unittest.TestCase):
    def test_list_and_get_only_read(self):
        chair = FakeChair()
        code, out, _ = run_cli(chair, "list")
        self.assertEqual(code, 0)
        self.assertIn("Controller timeout", out)
        self.assertIn("CORNER", out)
        chair = FakeChair()
        code, out, _ = run_cli(chair, "get", "layout", "--json")
        self.assertEqual(code, 0)
        self.assertIn('"value": 1', out)
        self.assertTrue(set(chair.commands) <= cc.READ_ONLY)

    def test_set_reports_old_new_and_ack(self):
        chair = FakeChair()
        code, out, _ = run_cli(chair, "set", "timeout", "150")
        self.assertEqual(code, 0)
        self.assertIn("Controller timeout: 100 -> 150 ms", out)
        self.assertEqual(chair.timeout, 150)

    def test_set_out_of_range_sends_nothing(self):
        chair = FakeChair()
        code, _, _ = run_cli(chair, "set", "timeout", "20")
        self.assertNotEqual(code, 0)
        self.assertNotIn(cc.CMD["set"], chair.commands)

    def test_refusal_outside_disarm(self):
        chair = FakeChair(disarmed=False)
        code, _, err = run_cli(chair, "set", "timeout", "150")
        self.assertEqual(code, 3)
        self.assertIn("DISARM", err)
        chair = FakeChair(disarmed=False)
        code, _, err = run_cli(chair, "save")
        self.assertEqual(code, 3)
        self.assertIn("DISARM", err)


if __name__ == "__main__":
    unittest.main()
