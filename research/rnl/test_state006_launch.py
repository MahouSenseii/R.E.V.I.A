"""Controller tests use stdlib files and a fake existing resource guard."""
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

import state006_launch as launch


class LaunchTests(unittest.TestCase):
    def test_default_never_seals_or_launches(self):
        with patch.object(launch, 'validate_config', return_value={}):
            with patch.object(launch, 'execute', side_effect=AssertionError('live')):
                self.assertEqual(launch.main(['--config', 'unused']), 0)

    def test_last_json_allows_only_known_warning_prefix(self):
        value = {'status': 'complete'}
        self.assertEqual(launch.parse_worker(b'{"status":"complete"}\n'), value)
        self.assertEqual(launch.parse_worker(b'UserWarning: Failed to initialize NumPy: absent\n{"status":"complete"}\n'), value)
        for data in (b'unknown error\n{"status":"complete"}\n', b'{"a":1}\n{"status":"complete"}', b'garbage'):
            with self.assertRaises(ValueError): launch.parse_worker(data)

    def test_probe_identity_requires_no_updates_and_exact_cpu_build(self):
        probe = dict(schema='STATE-006-native-check-v1', status='complete', model='gru', seed=11,
                     torch_version='2.15.0+cpu', cpu_only=True, parameters=4968,
                     shape=[2, 8], finite_gradients=True, optimizer_steps=0,
                     before_sha256='a'*64, after_sha256='a'*64, native_executed=True)
        launch.check_probe(probe, 'gru', '2.15.0+cpu')
        for key, val in [('optimizer_steps', 1), ('cpu_only', False), ('after_sha256', 'b'*64), ('torch_version', 'wrong')]:
            wrong = dict(probe); wrong[key] = val
            with self.assertRaises(ValueError): launch.check_probe(wrong, 'gru', '2.15.0+cpu')
        with tempfile.TemporaryDirectory(dir=launch.ROOT / '.verification') as directory:
            path = Path(directory) / '.verification/state006-probe-01/controller.json'
            path.parent.mkdir(parents=True)
            candidate = dict(schema='STATE-006-controller-v2', status='pending_confirmation', mode='probe',
                             eligible=False, operational_complete=True, error=None, elapsed_seconds=1,
                             assessment=dict(go=False), identity='same',
                             cells=[probe, dict(probe, model='rnn', parameters=4964)])
            path.write_text(json.dumps(candidate), encoding='utf-8')
            config = dict(probe_receipt=str(path), probe_confirmation=dict(controller_exit_code=0,
                          controller_wall_seconds=1, candidate_sha256=launch.digest(path)))
            with patch.object(launch, 'ROOT', Path(directory)):
                launch.validate_probe_receipt(config, dict(identity='same', torch_version='2.15.0+cpu'))
                del config['probe_confirmation']
                with self.assertRaises(ValueError):
                    launch.validate_probe_receipt(config, dict(identity='same', torch_version='2.15.0+cpu'))

    def test_schedule_stops_at_first_guard_failure(self):
        with tempfile.TemporaryDirectory(dir=launch.ROOT / '.verification') as directory:
            root = Path(directory)
            config = dict(output_dir=str(root / 'run'))
            calls = []
            def guard(command, dest, **kw):
                calls.append((command, kw)); Path(dest).mkdir()
                return dict(status='control_failed')
            with patch.object(launch, 'preflight', return_value={'identity': 'same', 'python': 'C:/python.exe', 'torch_version': '2.15.0+cpu'}):
                with patch.object(launch, 'validate_probe_receipt'):
                    report = launch.execute(config, 'study', 'a'*64, guard=guard)
            self.assertEqual(len(calls), 1)
            self.assertEqual(report['status'], 'incomplete')
            self.assertEqual(calls[0][1]['memory_cap_bytes'], 2 * 1024**3)
            self.assertLessEqual(calls[0][1]['wall_seconds'], 180)
            self.assertEqual(len(report['cells']), 1)
            self.assertIsNone(report['cells'][0]['metrics'])
        self.assertEqual(launch.STUDY, [('rnn', 11), ('gru', 11), ('gru', 23), ('rnn', 23), ('rnn', 37), ('gru', 37)])

    def test_failed_preflight_creates_no_run(self):
        with tempfile.TemporaryDirectory(dir=launch.ROOT / '.verification') as directory:
            output = Path(directory) / 'run'
            with patch.object(launch, 'preflight', side_effect=ValueError('tamper/freshness/environment')):
                with self.assertRaises(ValueError): launch.execute(dict(output_dir=str(output)), 'probe', 'a'*64)
            self.assertFalse(output.exists())
            env = Path(directory) / 'env'
            (env / 'Lib/site-packages/demo.dist-info').mkdir(parents=True)
            (env / 'Lib/site-packages/pip.dist-info').mkdir()
            (env / 'Scripts').mkdir()
            (env / 'Scripts/python.exe').write_bytes(b'fake executable never run')
            (env / 'Lib/site-packages/demo.dist-info/METADATA').write_text('Name: demo\nVersion: 1\n')
            (env / 'Lib/site-packages/pip.dist-info/METADATA').write_text('Name: pip\nVersion: 25\n')
            base = Path(directory) / 'base'; base.write_bytes(b'fake base')
            runtime = dict(wheels=[dict(name='demo', version='1')], selection=dict(torch_version='cpu'),
                           environment=dict(python_version='3.12.10', base_python_sha256=launch.digest(base), ensurepip_bundled_pip='25'))
            config = dict(_runtime=runtime, census=str(Path(directory) / 'census.json'), runtime_sha256='a'*64, source_sha256={})
            census = dict(schema='STATE-006-environment-v1', complete=True, native_imported=False,
                          runtime_sha256='a'*64, source_sha256={}, python_version='3.12.10',
                          python=str(env / 'Scripts/python.exe'), base_python=str(base), base_python_sha256=launch.digest(base),
                          packages=dict(demo='1', pip='25'), files=launch.file_census(env, lambda: None),
                          setup_elapsed_seconds=.1, setup_started_unix=time.time())
            Path(config['census']).write_text(json.dumps(census), encoding='utf-8')
            with patch.object(launch, 'runtime_path', return_value=env):
                self.assertTrue(launch.verify_environment(config)['identity'])
                (env / 'Scripts/python.exe').write_bytes(b'tampered')
                with self.assertRaises(ValueError): launch.verify_environment(config)

    def test_full_budget_admission_does_not_shrink_later_cells(self):
        with tempfile.TemporaryDirectory(dir=launch.ROOT / '.verification') as directory:
            clock, calls, checks = [0], [], [0]
            def preflight(*args):
                checks[0] += 1; clock[0] = 1050 if checks[0] == 1 else 1060
                return dict(identity='same', python='C:/never-run.exe', torch_version='2.15.0+cpu')
            def guard(*args, **kwargs):
                calls.append(args); return dict(status='control_failed')
            with patch.object(launch.time, 'monotonic', side_effect=lambda: clock[0]), patch.object(launch, 'preflight', side_effect=preflight), patch.object(launch, 'check_budget'), patch.object(launch, 'validate_probe_receipt'):
                launch.execute(dict(output_dir=str(Path(directory) / 'run')), 'study', 'a'*64, guard=guard)
            self.assertEqual(calls, [], 'must refuse aggregate remainder shorter than a full cell')
        with self.assertRaises(ValueError): launch.admit_cell(200, 190, 180, now=20)
        self.assertEqual(launch.admit_cell(300, 190, 180, now=20), 169)
        with self.assertRaises(ValueError): launch.admit_cell(300, 20, 180, now=20)
        with self.assertRaises(ValueError): launch.admit_cell(50, 45, 30, now=20)

    def test_retained_probe_counts_against_both_final_reservations(self):
        with tempfile.TemporaryDirectory(dir=launch.ROOT / '.verification') as directory:
            root = Path(directory); private = root / '.verification'; private.mkdir()
            probe = private / 'state006-probe-01'; probe.mkdir()
            (probe / 'retained').write_bytes(b'probe evidence')
            config = dict(output_dir=str(private / 'state006-study-01'), probe_receipt=str(probe / 'controller.json'))
            with patch.object(launch, 'ROOT', root):
                calls = []
                def guard(*args, **kwargs):
                    calls.append(kwargs); return dict(status='control_failed')
                with patch.object(launch, 'preflight', return_value=dict(identity='same', python='C:/never-run.exe', torch_version='2.15.0+cpu')), patch.object(launch, 'validate_probe_receipt'):
                    launch.execute(config, 'study', 'a'*64, guard=guard)
                self.assertIn(probe.resolve(), [Path(path).resolve() for path in calls[0]['owned_paths']])
                owned, evidence = launch.accounting_roots(config, 'study')
                self.assertTrue(set(evidence).issubset(owned))
                self.assertIn(probe.resolve(), evidence)
                self.assertGreaterEqual(launch.owned_bytes(evidence), 14)
                with patch.object(launch, 'GIB2', 14 + launch.FINAL_RESERVE):
                    with self.assertRaises(ValueError): launch.check_final_budget(time.monotonic()+100, owned, evidence)
                with patch.object(launch, 'EVIDENCE', 14 + launch.FINAL_RESERVE):
                    with self.assertRaises(ValueError): launch.check_final_budget(time.monotonic()+100, owned, evidence)

    def test_completion_needs_external_zero_exit_and_post_io_confirmation(self):
        with tempfile.TemporaryDirectory(dir=launch.ROOT / '.verification') as directory:
            import state006
            cells = []
            for model, seed in launch.STUDY:
                metrics = {key: 1. if model == 'gru' else .5 for key in state006.METRICS}
                metrics.update(state_reset=.1, state_exchange=.1, clean_reset=1.)
                cells.append(dict(schema='STATE-006-cell-v1', status='complete', model=model, seed=seed,
                                  metrics=metrics, training=dict(updates=256), native_executed=True, torch_version='2.15.0+cpu'))
            clock, index = [0], [0]
            original_write = launch.write_json
            def delayed_write(path, *args, **kwargs):
                if Path(path).name == 'incomplete.json': raise OSError('failure marker unavailable')
                original_write(path, *args, **kwargs)
                if Path(path).name == 'controller.json': clock[0] = 1201
            def guard(command, dest, **kwargs):
                Path(dest).mkdir(); (Path(dest) / 'combined-output.bin').write_text(json.dumps(cells[index[0]]))
                index[0] += 1
                return dict(status='completed', limits_verified=True, cleanup_exit_confirmed=True, wall_budget_compliant=True)
            run = Path(directory) / 'run'
            with patch.object(launch.time, 'monotonic', side_effect=lambda: clock[0]), patch.object(launch, 'preflight', return_value=dict(identity='same', python='C:/never-run.exe', torch_version='2.15.0+cpu')), patch.object(launch, 'check_budget'), patch.object(launch, 'validate_probe_receipt'), patch.object(launch, 'write_json', side_effect=delayed_write):
                with self.assertRaises((ValueError, OSError)):
                    launch.execute(dict(output_dir=str(run)), 'study', 'a'*64, guard=guard)
            persisted = json.loads((run / 'controller.json').read_text())
            self.assertFalse(persisted['assessment']['go'], 'late final IO must never leave a canonical GO')
            path = Path(directory) / '.verification/state006-study-01/controller.json'
            path.parent.mkdir(parents=True)
            candidate = dict(schema='STATE-006-controller-v2', status='pending_confirmation', mode='study',
                             eligible=False, identity='same', cells=cells, elapsed_seconds=1, error=None,
                             assessment=dict(status='pending_confirmation', go=False),
                             diagnostic_assessment=state006.assess(cells), operational_complete=True)
            path.write_text(json.dumps(candidate), encoding='utf-8')
            confirmation = dict(controller_exit_code=0, controller_wall_seconds=1, candidate_sha256=launch.digest(path))
            with patch.object(launch, 'ROOT', Path(directory)), patch.object(launch, 'check_final_budget'):
                accepted = launch.validate_completion(path, **confirmation)
                self.assertTrue(accepted['assessment']['go'])
                self.assertTrue(accepted['eligible'])
                for key, value in [('controller_exit_code', 1), ('controller_wall_seconds', 1201), ('candidate_sha256', '0'*64)]:
                    bad = dict(confirmation); bad[key] = value
                    with self.assertRaises(ValueError): launch.validate_completion(path, **bad)
                with self.assertRaises(TypeError): launch.validate_completion(path)
                with patch.object(launch, 'check_final_budget', side_effect=ValueError('post IO overrun')):
                    with self.assertRaises(ValueError): launch.validate_completion(path, **confirmation)
            candidate['status'] = 'incomplete'; candidate['operational_complete'] = False
            path.write_text(json.dumps(candidate), encoding='utf-8')
            confirmation['candidate_sha256'] = launch.digest(path)
            with patch.object(launch, 'ROOT', Path(directory)), patch.object(launch, 'check_final_budget'):
                with self.assertRaises(ValueError): launch.validate_completion(path, **confirmation)


if __name__ == '__main__': unittest.main()
