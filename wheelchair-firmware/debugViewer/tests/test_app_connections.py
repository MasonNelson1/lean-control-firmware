"""Run with debugViewer/.venv/bin/python -B -m unittest discover -s debugViewer/tests."""
import asyncio
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('viewer', Path(__file__).parents[1] / 'app.py')
viewer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(viewer)


class ConnectionTests(unittest.IsolatedAsyncioTestCase):
    async def test_paused_acquisition_does_not_open_usb(self):
        viewer.args = SimpleNamespace(replay=None, baud=115200, port='/dev/ttyACM0')
        viewer.connected = False
        viewer.proc = None
        with patch.object(viewer.asyncio, 'create_subprocess_exec') as spawn:
            task = asyncio.create_task(viewer.acquire())
            try:
                await asyncio.sleep(.02)
                spawn.assert_not_called()
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)

    async def test_switching_to_bluetooth_stops_monitor_without_reconnect_message(self):
        viewer.args = SimpleNamespace(replay=None, baud=115200, port='/dev/ttyACM0')
        viewer.connected = True
        viewer.proc = None
        viewer.console.clear()
        started = asyncio.Event()

        class Process:
            returncode = None
            terminated = False

            def terminate(self):
                self.terminated = True
                self.returncode = 0

            async def wait(self):
                return self.returncode

        process = Process()

        async def spawn(*args, **kwargs):
            started.set()
            return process

        with patch.object(viewer.asyncio, 'create_subprocess_exec', side_effect=spawn) as launch, \
                patch.object(viewer.os, 'read', side_effect=BlockingIOError):
            task = asyncio.create_task(viewer.acquire())
            try:
                await asyncio.wait_for(started.wait(), 1)
                viewer.connected = False
                await asyncio.sleep(.04)
                self.assertTrue(process.terminated)
                self.assertIsNone(viewer.proc)
                self.assertEqual(launch.call_count, 1)
                self.assertFalse(any('reconnecting' in message for message in viewer.console))
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)


if __name__ == '__main__':
    unittest.main()
