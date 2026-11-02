"""Own stdlib-only workers; never imports or executes uploaded/project code."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent


class BoundedControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = Path(tempfile.mkdtemp(prefix='bounded-controls-', dir=ROOT))

    def run_case(self, name, source, **limits):
        path = ROOT / 'run_bounded.py'
        self.assertTrue(path.exists(), 'bounded supervisor not implemented')
        spec = importlib.util.spec_from_file_location('run_bounded', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.run_bounded(
            [sys.executable, '-I', '-B', '-c', source],
            self.evidence / name,
            **limits,
        )

    def test_normal_exit_and_combined_output(self):
        result = self.run_case('normal', "import sys; print('finite own worker'); print('own stderr',file=sys.stderr)", wall_seconds=3)
        self.assertEqual(result['status'], 'completed', result)
        self.assertEqual(result['exit_code'], 0)
        self.assertTrue(result['limits_verified'])
        self.assertGreater(result['peak_working_set_bytes'], 0)
        self.assertLessEqual(result['peak_working_set_bytes'], 256 * 1024 * 1024)
        mask = result['affinity_mask']
        self.assertEqual(mask & (mask - 1), 0)
        output = (self.evidence / 'normal' / 'combined-output.bin').read_bytes()
        self.assertIn(b'finite own worker', output)
        self.assertIn(b'own stderr', output)

    def test_wall_timeout_terminates_owned_worker(self):
        began = time.monotonic()
        result = self.run_case('timeout', 'import time; time.sleep(10)', wall_seconds=0.5)
        self.assertEqual(result['status'], 'timeout', result)
        self.assertTrue(result['terminated_owned_job'])
        self.assertNotEqual(result['exit_code'], 259)
        self.assertLess(time.monotonic() - began, 2)

    def test_output_cap_stops_and_retains_partial_evidence(self):
        result = self.run_case('output-cap', "import os\nwhile True: os.write(1,b'x'*4096)", wall_seconds=3, total_output_limit=65536)
        self.assertEqual(result['status'], 'output_limit', result)
        self.assertTrue(result['terminated_owned_job'])
        self.assertTrue(result['output_truncated'])
        files = list((self.evidence / 'output-cap').iterdir())
        self.assertLess(sum(p.stat().st_size for p in files), 65536)
        self.assertGreater(result['captured_output_bytes'], 0)
        self.assertTrue((self.evidence / 'output-cap' / 'result.json').exists())

    def test_nonzero_exit_is_not_success(self):
        result = self.run_case('nonzero', 'raise SystemExit(9)', wall_seconds=3)
        self.assertEqual(result['status'], 'worker_failed', result)
        self.assertEqual(result['exit_code'], 9)

    def test_oversized_metadata_stops_before_creating_worker(self):
        try:
            result = self.run_case('metadata-reserve', "pass # " + '\U0001f600' * 3000, wall_seconds=3)
        except RuntimeError as exc:
            self.fail('Metadata must be refused and retained before launch: ' + str(exc))
        self.assertEqual(result['status'], 'control_failed', result)
        self.assertIsNone(result['pid'])
        self.assertTrue((self.evidence / 'metadata-reserve' / 'result.json').exists())


if __name__ == '__main__':
    result = unittest.main(exit=False)
    summary = {'passed': result.result.wasSuccessful(), 'tests_run': result.result.testsRun, 'failures': len(result.result.failures), 'errors': len(result.result.errors), 'evidence_directory': str(BoundedControls.evidence)}
    (BoundedControls.evidence / 'selftest-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary))
    raise SystemExit(0 if summary['passed'] else 1)
