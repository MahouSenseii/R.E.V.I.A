"""Small offline boundaries only; no actual HTTP, subprocess or packages."""
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import state006_setup as setup


class SetupTests(unittest.TestCase):
    def test_default_is_offline(self):
        with patch.object(setup, 'execute', side_effect=AssertionError('live')):
            with patch.object(setup, 'validate_config', return_value={}):
                self.assertEqual(setup.main(['--config', 'unused']), 0)
        with tempfile.TemporaryDirectory(dir=setup.ROOT / '.verification') as directory:
            root = Path(directory)
            runtime = setup.HERE / 'STATE-006-runtime.json'
            lock = setup.HERE / 'state006-offline.lock'
            config = dict(schema='STATE-006-control-config-v1', runtime=str(runtime), runtime_sha256=setup.digest(runtime),
                          offline_lock=str(lock), offline_lock_sha256=setup.digest(lock),
                          source_sha256={n: setup.digest(setup.HERE / n) for n in setup.SOURCES},
                          output_dir=str(setup.ROOT / '.verification/state006-probe-01'), census=str(setup.ROOT / '.verification/state006-environment-01.json'),
                          manifest=str(root / 'manifest.json'), probe_receipt=None)
            path = root / 'config.json'; path.write_text(json.dumps(config), encoding='utf-8')
            self.assertEqual(setup.validate_config(path)['_config_sha256'], setup.digest(path))
            for key in ('runtime_sha256', 'offline_lock_sha256'):
                bad = dict(config); bad[key] = '0' * 64
                path.write_text(json.dumps(bad), encoding='utf-8')
                with self.assertRaises(ValueError): setup.validate_config(path)
            config['source_sha256']['state006.py'] = '0' * 64
            path.write_text(json.dumps(config), encoding='utf-8')
            with self.assertRaises(ValueError): setup.validate_config(path)

    def test_download_is_exact_once_and_rejects_length_and_hash(self):
        data = b'bounded wheel'
        wheel = dict(url='https://files.pythonhosted.org/a.whl', filename='a.whl',
                     size_bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        class Response(io.BytesIO):
            status = 200
            headers = {'Content-Length': str(len(data))}
            def geturl(self): return wheel['url']
        with tempfile.TemporaryDirectory(dir=setup.ROOT / '.verification') as directory:
            target = Path(directory) / 'a.whl'
            with patch.object(setup, 'open_https', return_value=Response(data)) as request:
                setup.fetch_wheel(wheel, target, lambda: None)
                self.assertEqual(request.call_count, 1)
            self.assertEqual(target.read_bytes(), data)
            for payload in (data[:-1], data + b'x', b'x' * len(data)):
                target.unlink()
                with patch.object(setup, 'open_https', return_value=Response(payload)):
                    with self.assertRaises(ValueError):
                        setup.fetch_wheel(wheel, target, lambda: None)
                target.write_bytes(b'for next iteration')

    def test_zip_checks_payload_traversal_and_links(self):
        with tempfile.TemporaryDirectory(dir=setup.ROOT / '.verification') as directory:
            target = Path(directory) / 'a.whl'
            for name, link in [('ok/file', False), ('../escape', False), ('C:/bad', False), ('link', True)]:
                with zipfile.ZipFile(target, 'w') as archive:
                    entry = zipfile.ZipInfo(name)
                    if link: entry.external_attr = 0o120777 << 16
                    archive.writestr(entry, b'abc')
                if name == 'ok/file': self.assertEqual(setup.wheel_payload(target), 3)
                else:
                    with self.assertRaises(ValueError): setup.wheel_payload(target)

    def test_environment_identity_detects_tamper_and_added_files(self):
        with tempfile.TemporaryDirectory(dir=setup.ROOT / '.verification') as directory:
            env = Path(directory)
            (env / 'one').write_bytes(b'original')
            census = setup.file_census(env, lambda: None)
            self.assertEqual(census, setup.file_census(env, lambda: None))
            (env / 'one').write_bytes(b'changed')
            self.assertNotEqual(census, setup.file_census(env, lambda: None))
            (env / 'two').write_bytes(b'new')
            self.assertEqual(len(setup.file_census(env, lambda: None)), 2)

    def test_clock_disk_and_freshness_fail_closed(self):
        self.assertEqual(setup.digest(setup.HERE / 'run_bounded.py'),
                         '31edb4ff8a997363d07ea8823a2efb7bdde5ed2566872a9e6adbda20b5b62bb7')
        with tempfile.TemporaryDirectory(dir=setup.ROOT / '.verification') as directory:
            path = Path(directory) / 'owned'
            path.mkdir(); (path / 'file').write_bytes(b'ab')
            with self.assertRaises(ValueError): setup.require_fresh([path])
            with self.assertRaises(ValueError): setup.check_budget(0, [path], now=lambda: 1)
            with self.assertRaises(ValueError): setup.check_budget(100, [path], cap=1, now=lambda: 1)
            with patch.object(setup.shutil, 'disk_usage', return_value=(10, 10, 0)):
                with self.assertRaises(ValueError): setup.require_space(path, 1)
            import run_bounded
            import state006_guard
            self.assertIs(state006_guard.Security, run_bounded.Security)
            self.assertIs(state006_guard.ExtendedLimits, run_bounded.ExtendedLimits)
            original_apis = run_bounded._apis
            with patch.object(state006_guard, '_apis', return_value=(object(), object())):
                result = state006_guard.run_bounded([str(Path(__import__('sys').executable).resolve()), '-c', 'never run'],
                                                Path(directory) / 'guard', memory_cap_bytes=setup.GIB2,
                                                owned_paths=[path], disk_limit_bytes=1)
            self.assertIs(run_bounded._apis, original_apis)
            self.assertEqual(run_bounded.MEMORY_CAP, 256 * 1024**2)
            self.assertEqual(result['status'], 'incomplete_disk_measurement')
            self.assertEqual(result['memory_cap_bytes'], setup.GIB2)
            self.assertIsNone(result['pid'])


if __name__ == '__main__': unittest.main()
