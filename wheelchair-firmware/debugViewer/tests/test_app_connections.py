"""Run with debugViewer/.venv/bin/python -B -m unittest discover -s debugViewer/tests."""
import asyncio
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch, AsyncMock

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
                command = launch.call_args.args
                self.assertEqual(command[command.index('-e') + 1], 'teensy41')
                self.assertFalse(any('reconnecting' in message for message in viewer.console))
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)


class BluetoothCleanupTests(unittest.IsolatedAsyncioTestCase):
    def request(self, host='127.0.0.1', headers=None):
        return SimpleNamespace(client=SimpleNamespace(host=host),
                               headers=headers if headers is not None else {'x-bluetooth-cleanup': '1'})

    async def test_fixed_receiver_command(self):
        process = SimpleNamespace(returncode=0, communicate=AsyncMock(return_value=(b'Successful disconnected', b'')))
        with patch.object(viewer.sys, 'platform', 'linux'), patch.object(
                viewer.asyncio, 'create_subprocess_exec', new=AsyncMock(return_value=process)) as spawn:
            await viewer.release_bluetooth(self.request())
            self.assertEqual(spawn.call_args.args, ('bluetoothctl', 'disconnect', '98:DA:60:0A:41:3C'))

    async def test_remote_or_unmarked_requests_are_rejected(self):
        with patch.object(viewer.asyncio, 'create_subprocess_exec', new=AsyncMock()) as spawn:
            for request in (self.request(host='192.168.1.2'), self.request(headers={})):
                with self.assertRaises(viewer.HTTPException) as error:
                    await viewer.release_bluetooth(request)
                self.assertEqual(error.exception.status_code, 403)
            spawn.assert_not_called()

    async def test_command_failure_is_reported(self):
        process = SimpleNamespace(returncode=1, communicate=AsyncMock(return_value=(b'', b'No default controller')))
        with patch.object(viewer.sys, 'platform', 'linux'), patch.object(
                viewer.asyncio, 'create_subprocess_exec', new=AsyncMock(return_value=process)):
            with self.assertRaises(viewer.HTTPException) as error:
                await viewer.release_bluetooth(self.request())
            self.assertEqual(error.exception.status_code, 503)


if __name__ == '__main__':
    unittest.main()
