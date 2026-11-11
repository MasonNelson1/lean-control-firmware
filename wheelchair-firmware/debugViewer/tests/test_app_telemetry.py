"""Run with the viewer environment's Python and unittest discovery."""
import asyncio
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("tested_telemetry_app", Path(__file__).parents[1] / "app.py")
viewer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(viewer)


def row(time=1000, mode="LEAN"):
    return f"{time},100,200,300,400,1000,370,0.25,-0.1,10,20,9,19,0,{mode}"


class ParseTests(unittest.TestCase):
    pass


def install_parse_case(name, line, kind, mode=None):
    def check(self):
        actual_kind, parsed = viewer.parse(line)
        self.assertEqual(actual_kind, kind)
        if mode is not None:
            self.assertEqual(parsed["mode"], mode)
    setattr(ParseTests, "test_" + name, check)


for name, line, kind, mode in [
    ("lean", row(), "data", "LEAN"),
    ("remote", row(mode="REMOTE"), "data", "REMOTE"),
    ("unknown_mode", row(mode="other"), "data", "UNKNOWN"),
    ("lowercase_mode", row(mode="lean"), "data", "LEAN"),
    ("legacy_fourteen", ",".join(row().split(",")[:14]), "data", "UNKNOWN"),
    ("legacy_twelve", "1,2,3,4,5,6,7,0,0,10,20,1", "data", "UNKNOWN"),
    ("header", "time_ms,F,B,L,R", "header", None),
    ("empty", "", "text", None),
    ("calibration", "** Calibrated **", "text", None),
    ("trailing_extra", row() + ",extra", "text", None),
    ("missing_column", ",".join(row().split(",")[:10]), "text", None),
    ("whitespace", "  " + row() + "  ", "data", "LEAN"),
]:
    install_parse_case(name, line, kind, mode)

for invalid in ["NaN", "Infinity", "-Infinity", "", "garbage"]:
    for index in [0, 7, 11]:
        parts = row().split(",")
        parts[index] = invalid
        install_parse_case("invalid_" + (invalid or "empty").replace("-", "minus") + "_" + str(index),
                           ",".join(parts), "text")


class HistoryTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        viewer.history.clear()
        viewer.console.clear()
        viewer.clients.clear()

    async def test_reboot_clears_previous_history(self):
        await viewer.line_received(row(1000))
        await viewer.line_received(row(10))
        self.assertEqual([value["time_ms"] for value in viewer.history], [10])

    async def test_same_timestamp_preserves_samples(self):
        await viewer.line_received(row(1000))
        await viewer.line_received(row(1000))
        self.assertEqual(len(viewer.history), 2)

    async def test_sixty_second_boundary_is_retained(self):
        await viewer.line_received(row(1))
        await viewer.line_received(row(60001))
        self.assertEqual(len(viewer.history), 2)

    async def test_samples_older_than_window_are_removed(self):
        await viewer.line_received(row(1))
        await viewer.line_received(row(60002))
        self.assertEqual(len(viewer.history), 1)

    async def test_header_stays_out_of_console_and_history(self):
        await viewer.line_received("time_ms,F,B,L,R")
        self.assertFalse(viewer.history)
        self.assertFalse(viewer.console)

    async def test_messages_are_emitted_as_console_events(self):
        events = []
        async def emit(event):
            events.append(event)
        with patch.object(viewer, "emit", emit):
            await viewer.line_received("Calibration complete")
        self.assertEqual(events, [{"console": "Calibration complete"}])
        self.assertEqual(list(viewer.console), ["Calibration complete"])

    async def test_stale_clock_updates_on_telemetry(self):
        with patch.object(viewer.time, "monotonic", return_value=123):
            await viewer.line_received(row())
        self.assertEqual(viewer.last_change[0], 123)

    async def test_console_is_bounded(self):
        async def emit(event):
            pass
        with patch.object(viewer, "emit", emit):
            for index in range(405):
                await viewer.line_received(str(index))
        self.assertEqual(len(viewer.console), 400)
        self.assertEqual(viewer.console[0], "5")

    async def test_dead_websocket_is_removed(self):
        class DeadClient:
            async def send_json(self, event):
                raise RuntimeError("disconnected")
        client = DeadClient()
        viewer.clients.add(client)
        await viewer.emit({"test": True})
        self.assertNotIn(client, viewer.clients)


class AssetTests(unittest.IsolatedAsyncioTestCase):
    async def test_configuration_asset(self):
        self.assertTrue(Path((await viewer.configuration_script()).path).is_file())

    async def test_protocol_asset(self):
        self.assertTrue(Path((await viewer.config_protocol_script()).path).is_file())

    async def test_pid_preview_asset(self):
        self.assertTrue(Path((await viewer.pid_preview_script()).path).is_file())

    async def test_main_page_has_configuration_navigation(self):
        self.assertIn(b"config-page", (await viewer.index()).body)


if __name__ == "__main__":
    unittest.main()
