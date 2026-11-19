"""Dashboard-only .lean archive handling; no serial or firmware dependencies."""
import importlib.util
import io
import json
from pathlib import Path
import unittest
import zipfile
import subprocess
import tempfile

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

    async def test_python_and_browser_archives_are_interchangeable(self):
        helper = Path(__file__).with_name('profile-codec-helper.cjs')
        profile = {**self.profile, 'fields': self.profile['fields'] + [
            {'id': 11, 'type': 13, 'value': ['0.21000000834465027', '0.09000000357627869']},
            {'id': 12, 'type': 11, 'value': False},
            {'id': 9999, 'type': 255, 'value': 'future'}]}
        with tempfile.TemporaryDirectory() as folder:
            source, archive, decoded = (Path(folder) / name for name in ('source.json', 'profile.lean', 'decoded.json'))
            source.write_text(json.dumps(profile), encoding='utf-8')
            subprocess.run(['node', str(helper), 'encode', str(source), str(archive)], check=True, capture_output=True)
            imported = await viewer.import_profile(Request(archive.read_bytes()))
            self.assertEqual(json.loads(imported.body), profile)
            exported = await viewer.export_profile(Request(source.read_bytes()))
            archive.write_bytes(exported.body)
            subprocess.run(['node', str(helper), 'decode', str(archive), str(decoded)], check=True, capture_output=True)
            self.assertEqual(json.loads(decoded.read_text()), profile)

    async def test_browser_accepts_python_streamed_zip_with_descriptor_and_comment(self):
        class Unseekable(io.BytesIO):
            def seek(self, *args):
                raise io.UnsupportedOperation('streaming ZIP')

        data = Unseekable()
        with zipfile.ZipFile(data, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('profile.json', json.dumps(self.profile))
            archive.comment = b'Compatible profile archive'
        with tempfile.TemporaryDirectory() as folder:
            archive, decoded = Path(folder) / 'profile.lean', Path(folder) / 'profile.json'
            archive.write_bytes(data.getvalue())
            subprocess.run(['node', str(Path(__file__).with_name('profile-codec-helper.cjs')),
                            'decode', str(archive), str(decoded)], check=True, capture_output=True)
            self.assertEqual(json.loads(decoded.read_text()), self.profile)


if __name__ == '__main__':
    unittest.main()
