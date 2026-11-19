"""Dashboard-only .lean archive handling; no serial or firmware dependencies."""
import importlib.util
import io
import json
from pathlib import Path
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('profile_viewer', Path(__file__).parents[1] / 'app.py')
viewer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(viewer)


class Request:
    def __init__(self, data):
        self.data = data

    async def stream(self):
        for start in range(0, len(self.data), 8192):
            yield self.data[start:start + 8192]


class ProfileTests(unittest.IsolatedAsyncioTestCase):
    profile = {'format': 'lean-config', 'version': 1, 'fields': [
        {'id': 10, 'type': 10, 'value': '18446744073709551615'}]}

    def archive(self, content, name='profile.json', compression=zipfile.ZIP_DEFLATED):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w', compression=compression) as archive:
            archive.writestr(name, content)
        return data.getvalue()

    async def test_round_trip_uses_real_compressed_zip_and_preserves_json(self):
        response = await viewer.export_profile(Request(json.dumps(self.profile).encode()))
        self.assertIn('.lean', response.headers['content-disposition'])
        with zipfile.ZipFile(io.BytesIO(response.body)) as archive:
            self.assertEqual(archive.namelist(), ['profile.json'])
            self.assertEqual(archive.getinfo('profile.json').compress_type, zipfile.ZIP_DEFLATED)
            self.assertEqual(json.loads(archive.read('profile.json')), self.profile)
        loaded = await viewer.import_profile(Request(response.body))
        self.assertEqual(json.loads(loaded.body), self.profile)

    async def test_rejects_invalid_and_oversized_archives(self):
        for data in [b'not a zip', self.archive(b'{}', '../profile.json'),
                     self.archive(b'{}', compression=zipfile.ZIP_STORED),
                     self.archive(b'x' * (viewer.PROFILE_LIMIT + 1)),
                     self.archive(b'not JSON'), self.archive(b'{}')]:
            with self.subTest(size=len(data)):
                with self.assertRaises(viewer.HTTPException) as raised:
                    await viewer.import_profile(Request(data))
                self.assertEqual(raised.exception.status_code, 400)

    async def test_rejects_multiple_entries(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('profile.json', json.dumps(self.profile))
            archive.writestr('extra.txt', 'extra')
        with self.assertRaises(viewer.HTTPException):
            await viewer.import_profile(Request(data.getvalue()))

    async def test_request_limit(self):
        with self.assertRaises(viewer.HTTPException) as raised:
            await viewer.export_profile(Request(b' ' * (viewer.PROFILE_LIMIT + 1)))
        self.assertEqual(raised.exception.status_code, 413)

    async def test_unknown_properties_and_partial_fields_are_preserved(self):
        profile = {**self.profile, 'future': 'ignored', 'fields': []}
        exported = await viewer.export_profile(Request(json.dumps(profile).encode()))
        imported = await viewer.import_profile(Request(exported.body))
        self.assertEqual(json.loads(imported.body), profile)

    async def test_invalid_format_and_version(self):
        for profile in [{}, [], {**self.profile, 'version': 99},
                        {**self.profile, 'version': True}, {**self.profile, 'fields': None}]:
            with self.assertRaises(viewer.HTTPException):
                await viewer.export_profile(Request(json.dumps(profile).encode()))


if __name__ == '__main__':
    unittest.main()
