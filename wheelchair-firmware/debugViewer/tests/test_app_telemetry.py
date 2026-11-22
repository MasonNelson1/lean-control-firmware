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
    def test_gain_status(self):
        kind, gains = viewer.parse('DYNAMIC_GAINS F=1.2300 B=2.3400 L=1.0000 R=0.9800')
        self.assertEqual(kind, 'scale')
        self.assertEqual(gains, {'kind': 'gains', 'F': 1.23, 'B': 2.34, 'L': 1, 'R': .98})

    def test_invalid_gain_status(self):
        for message in (
            'DYNAMIC_GAINS F=1 B=2 L=3',
            'DYNAMIC_GAINS F=nan B=2 L=3 R=4',
            'DYNAMIC_GAINS F=-1 B=2 L=3 R=4',
            'DYNAMIC_GAINS F=0 B=2 L=3 R=4',
            'DYNAMIC_GAINS F=1 B=2 L=3 R=4 garbage',
        ):
            self.assertEqual(viewer.parse(message)[0], 'text')

    def test_lean_ranges(self):
        kind, ranges = viewer.parse('LEAN_RANGES F=600 B=400 L=500 R=500')
        self.assertEqual(kind, 'scale')
        self.assertEqual(ranges, {'kind': 'ranges', 'F': 600, 'B': 400, 'L': 500, 'R': 500})

    def test_invalid_lean_ranges(self):
        for message in (
            'LEAN_RANGES F=600 B=400 L=500',
            'LEAN_RANGES F=600 B=400 L=500 R=0',
            'LEAN_RANGES F=-600 B=400 L=500 R=500',
            'LEAN_RANGES F=600.5 B=400 L=500 R=500',
            'LEAN_RANGES F=600 B=400 L=500 R=500 garbage',
        ):
            self.assertEqual(viewer.parse(message)[0], 'text')

    def test_health(self):
        kind, health = viewer.parse('HEALTH fault=NONE seated=1 load=330 vescL=50 vescR=50 stop=0 cal=1')
        self.assertEqual(kind, 'health')
        self.assertEqual(health, {'fault': 'NONE', 'seated': True, 'stop': False, 'cal': True,
                                  'load': 330, 'vescL': 50, 'vescR': 50})

    def test_health_ignores_unknown_keys_and_bad_known_values(self):
        self.assertEqual(viewer.parse('HEALTH future=7 fault=VESC_SILENT load=-12 vescL=0 brandNew2=x')[1],
                         {'fault': 'VESC_SILENT', 'load': -12, 'vescL': 0})
        self.assertEqual(viewer.parse('HEALTH fault=NEW_THING seated=maybe')[1], {'fault': 'NEW_THING'})

    def test_invalid_health_is_console_text(self):
        for message in ('HEALTH', 'HEALTH seated=1 load=3', 'HEALTH fault=none', 'HEALTH fault=NONE garbage',
                        'HEALTH fault=NONE 9x=1', 'HEALTHY fault=NONE', 'Health fault=NONE'):
            self.assertEqual(viewer.parse(message), ('text', message))


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
    ("disarm", row(mode="DISARM"), "data", "DISARM"),
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
        viewer.telemetry_warning_at = float("-inf")
        viewer.telemetry_warning_rows = 0
        viewer.clear_chair_status()
        viewer.health_at = float("-inf")

    async def test_gain_status_is_cached_and_emitted_without_console_or_csv_changes(self):
        events = []
        async def emit(event):
            events.append(event)
        with patch.object(viewer, 'emit', emit):
            await viewer.line_received('DYNAMIC_GAINS F=1.2300 B=2.3400 L=1.0000 R=0.9800')
        gains = {'kind': 'gains', 'F': 1.23, 'B': 2.34, 'L': 1, 'R': .98}
        self.assertEqual(viewer.lean_scale, gains)
        self.assertEqual(events, [{'leanScale': gains}])
        self.assertFalse(viewer.history)
        self.assertFalse(viewer.console)

    async def test_new_client_broadcast_includes_cached_gains(self):
        await viewer.line_received('DYNAMIC_GAINS F=1 B=2 L=3 R=4')
        events = []
        async def emit(event):
            events.append(event)
        with patch.object(viewer, 'emit', emit):
            task = asyncio.create_task(viewer.broadcast())
            await asyncio.sleep(0)
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        self.assertEqual(events[0]['leanScale'], {'kind': 'gains', 'F': 1, 'B': 2, 'L': 3, 'R': 4})
        self.assertIsNone(events[0]['health'])

    async def test_ranges_and_health_are_cached_emitted_and_kept_out_of_console(self):
        events = []
        async def emit(event):
            events.append(event)
        ok = 'HEALTH fault=NONE seated=1 load=330 vescL=50 vescR=50 stop=0 cal=1'
        with patch.object(viewer, 'emit', emit):
            await viewer.line_received('LEAN_RANGES F=600 B=400 L=500 R=500')
            await viewer.line_received(ok)
        ranges = {'kind': 'ranges', 'F': 600, 'B': 400, 'L': 500, 'R': 500}
        health = {'fault': 'NONE', 'seated': True, 'stop': False, 'cal': True, 'load': 330, 'vescL': 50, 'vescR': 50}
        self.assertEqual(events, [{'leanScale': ranges}, {'health': health}])
        self.assertFalse(viewer.console)
        self.assertFalse(viewer.history)
        with patch.object(viewer, 'emit', emit):
            events.clear()
            task = asyncio.create_task(viewer.broadcast())
            await asyncio.sleep(0)
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        self.assertEqual(events[0]['leanScale'], ranges)
        self.assertEqual(events[0]['health'], health)

    async def test_stale_health_is_not_broadcast(self):
        await viewer.line_received('HEALTH fault=NONE')
        viewer.health_at -= viewer.HEALTH_STALE_S + .5
        events = []
        async def emit(event):
            events.append(event)
        with patch.object(viewer, 'emit', emit):
            task = asyncio.create_task(viewer.broadcast())
            await asyncio.sleep(0)
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        self.assertIsNone(events[0]['health'])

    async def test_fault_change_is_logged_once(self):
        events = []
        async def emit(event):
            events.append(event)
        ok, silent = 'HEALTH fault=NONE vescL=50', 'HEALTH fault=VESC_SILENT vescL=0'
        with patch.object(viewer, 'emit', emit):
            for line in (ok, ok, silent, silent, silent, ok, ok, 'HEALTH fault=SENSOR_RAIL'):
                await viewer.line_received(line)
        self.assertEqual(list(viewer.console), ['Fault: VESC_SILENT', 'Fault cleared', 'Fault: SENSOR_RAIL'])
        self.assertEqual([e['console'] for e in events if 'console' in e], list(viewer.console))
        self.assertEqual(viewer.health, {'fault': 'SENSOR_RAIL'})

    async def test_disconnect_clears_chair_status(self):
        await viewer.line_received('LEAN_RANGES F=600 B=400 L=500 R=500')
        await viewer.line_received('HEALTH fault=VESC_SILENT')
        class Socket:
            commands = ['disconnect']
            async def accept(self): pass
            async def send_json(self, event): pass
            async def receive_text(self):
                if self.commands: return self.commands.pop()
                raise viewer.WebSocketDisconnect()
        saved = viewer.connected
        try:
            await viewer.socket(Socket())
        finally:
            viewer.connected = saved
        self.assertIsNone(viewer.lean_scale)
        self.assertIsNone(viewer.health)
        await viewer.line_received('HEALTH fault=VESC_SILENT')
        self.assertEqual(list(viewer.console).count('Fault: VESC_SILENT'), 2)

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


class ExtendedTelemetryTest(unittest.TestCase):
    def test_assist_current_and_unavailable_rpm(self):
        kind, sample = viewer.parse('1,2,3,4,5,6,7,0,0,nan,nan,nan,nan,1,nan,-12,3.25,4.5,ASSIST')
        self.assertEqual(kind, 'data')
        self.assertEqual(sample['mode'], 'ASSIST')
        self.assertNotIn('leftActualRPM', sample)
        self.assertEqual(sample['rightActualRPM'], -12)
        self.assertEqual(sample['leftBoostCurrent'], 3.25)
        self.assertEqual(sample['rightBoostCurrent'], 4.5)


class CurrentTelemetryTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        viewer.history.clear()
        viewer.console.clear()
        viewer.clients.clear()
        viewer.telemetry_warning_at = float('-inf')
        viewer.telemetry_warning_rows = 0

    def sample(self, speed='0.25', turn='-0.1'):
        return f'1000,100,200,300,400,1000,370,{speed},{turn},10,20,9,19,0,nan,-nan,0,0,LEAN'

    def test_nonfinite_direction_preserves_valid_fields(self):
        for token in ['nan', '-nan', '+NaN', 'inf', '-Infinity']:
            with self.subTest(token=token):
                kind, sample = viewer.parse(self.sample(token))
                self.assertEqual(kind, 'data')
                self.assertEqual(sample['mode'], 'LEAN')
                self.assertEqual(sample['F'], 100)
                self.assertNotIn('speed', sample)
                self.assertNotIn('rightActualRPM', sample)
                json = viewer.json.dumps(sample, allow_nan=False)
                self.assertNotIn('NaN', json)
        self.assertEqual(viewer.parse(self.sample('garbage'))[0], 'text')

    async def test_unavailable_direction_updates_history_and_warns_once(self):
        with patch.object(viewer.time, 'monotonic', return_value=10):
            for _ in range(40):
                await viewer.line_received(self.sample('-nan', 'nan'))
        self.assertEqual(len(viewer.history), 40)
        self.assertEqual(len(viewer.console), 1)
        self.assertIn('unavailable speed, turn', viewer.console[0])

    async def test_rejected_csv_is_rate_limited_and_recovers(self):
        damaged = self.sample('garbage')
        with patch.object(viewer.time, 'monotonic', return_value=10):
            for _ in range(100):
                await viewer.line_received(damaged)
            await viewer.line_received('Mode: LEAN')
        self.assertEqual(len(viewer.console), 2)
        self.assertIn('rejected CSV (19 columns)', viewer.console[0])
        self.assertEqual(viewer.console[1], 'Mode: LEAN')
        with patch.object(viewer.time, 'monotonic', return_value=15):
            await viewer.line_received(damaged)
            await viewer.line_received(self.sample())
        self.assertEqual(len(viewer.console), 3)
        self.assertEqual(len(viewer.history), 1)


class DamagedTelemetryTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        viewer.history.clear()
        viewer.console.clear()
        viewer.clients.clear()
        viewer.telemetry_warning_at = float('-inf')
        viewer.telemetry_warning_rows = 0

    async def test_reported_concatenated_lean_row(self):
        sample = '1782632,0,0,43,0,43,757,0.0000,-0.0356,-1.8723,1.8723,-1.9189,1.9189,0,0.0000,0.0000,0.0000,0.0000,LE1782682,0,0,46,0,46,757,0.0000,-0.0385,-2.0251,2.0251,-1.8511,1.8511,0,0.0000,0.0000,0.0000,0.0000,LEAN'
        await viewer.line_received(sample)
        self.assertEqual(len(viewer.history), 1)
        self.assertEqual(viewer.history[0]['time_ms'], 1782682)
        self.assertEqual(viewer.history[0]['L'], 46)
        self.assertEqual(viewer.history[0]['turn'], -0.0385)
        self.assertIn('recovered 1 complete rows', viewer.console[0])

    async def test_missing_newlines(self):
        make_row = lambda t: f'{t},0,0,46,0,46,757,0,-0.0385,-2,2,-1,1,0,0,0,0,0,LEAN'
        await viewer.line_received(''.join(make_row(t) for t in (1000, 1050, 1100)))
        self.assertEqual([r['time_ms'] for r in viewer.history], [1000, 1050, 1100])

    def test_truncated_rows_stay_rejected(self):
        self.assertEqual(viewer.recover_telemetry_rows('1787682,0,0,49,0,49,757,0.0000,-0.0414,-2.1795,2.1795'), [])
