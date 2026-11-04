"""Preparation controls only: no native imports, model execution or training."""
import builtins
from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import importlib.util
import io
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
FILE = HERE / 'state009.py'
if FILE.exists():
    spec = importlib.util.spec_from_file_location('state009_fixture_subject', FILE)
    subject = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(subject)
else:
    subject = None


class PreparationControls(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(subject, 'STATE-009 worker is not implemented')

    def test_default_and_import_do_not_generate_data_or_load_native_framework(self):
        previous_import = builtins.__import__
        def checked(name, *a, **kw):
            if name.split('.')[0] in ('torch', 'numpy'):
                raise AssertionError('Native import in preparation')
            return previous_import(name, *a, **kw)
        with patch('builtins.__import__', checked), patch.object(subject.data, 'make_split', side_effect=AssertionError('Generated data')):
            spec.loader.exec_module(subject)
            with redirect_stdout(io.StringIO()) as capture:
                self.assertEqual(subject.main([]), 0)
            self.assertEqual(json.loads(capture.getvalue())['status'], 'prepared_unexecuted')

    def test_readout_reference_requires_key_and_blind_ignores_key(self):
        feature = [1.] + [0.] * 31
        weights = [[float(i)] + [0.] * 31 for i in range(32)]
        bias = [0.] * 32
        self.assertEqual(subject.readout_reference(weights, bias, feature, 0, False), list(map(float, range(8))))
        self.assertEqual(subject.readout_reference(weights, bias, feature, 3, False), list(map(float, range(24, 32))))
        self.assertEqual(subject.readout_reference(weights, bias, feature, 0, True),
                         subject.readout_reference(weights, bias, feature, 3, True))
        self.assertEqual(subject.readout_reference(weights, bias, feature, 2, True), list(map(float, range(12, 20))))

    def test_snapshot_format_is_bounded_finite_exact_shape_and_nonpickle(self):
        layout = subject.snapshot_layout('reader')
        tensors = {name: (shape, [0.] * math.prod(shape)) for name, shape in layout.items()}
        raw, inventory = subject.encode_snapshot(tensors, 'reader')
        self.assertEqual(len(raw), 1056 * 4)
        self.assertEqual(raw, struct.pack('<1056f', *([0.] * 1056)))
        self.assertEqual(subject.decode_snapshot(raw, inventory, 'reader'), tensors)
        broken = deepcopy(tensors)
        broken['heads.bias'][1][0] = float('nan')
        with self.assertRaises(ValueError): subject.encode_snapshot(broken, 'reader')
        with self.assertRaises(ValueError): subject.decode_snapshot(raw + b'\0', inventory, 'reader')
        bad_inventory = deepcopy(inventory)
        bad_inventory[1]['offset_bytes'] = 0
        with self.assertRaises(ValueError): subject.decode_snapshot(raw, bad_inventory, 'reader')

    def test_snapshot_write_is_exclusive_and_hash_verified(self):
        tensors = {name: (shape, [0.] * math.prod(shape)) for name, shape in subject.snapshot_layout('reader').items()}
        raw, inventory = subject.encode_snapshot(tensors, 'reader')
        with tempfile.TemporaryDirectory(prefix='state009-fixture-', dir=HERE.parent) as directory:
            self.assertTrue(Path(directory).resolve().is_relative_to(HERE.parent.resolve()))
            record = subject.write_snapshot_bytes(Path(directory), 'trained_selected-reader.f32', raw, inventory, 'reader')
            self.assertEqual(record['sha256'], hashlib.sha256(raw).hexdigest())
            self.assertEqual(subject.verify_snapshot_file(Path(directory), record), tensors)
            with self.assertRaises(FileExistsError):
                subject.write_snapshot_bytes(Path(directory), 'trained_selected-reader.f32', raw, inventory, 'reader')
            with self.assertRaises(ValueError):
                subject.write_snapshot_bytes(Path(directory), '../escape.f32', raw, inventory, 'reader')
            file = Path(directory) / record['name']
            file.write_bytes(raw[:-1])
            with self.assertRaises(ValueError): subject.verify_snapshot_file(Path(directory), record)

    def test_fixed_config_rejects_changed_science(self):
        config = subject.default_config()
        self.assertEqual(subject.validate_config(config), config)
        for key, value in [('updates', 1009), ('seeds', [11, 23]), ('reader_parameters', True), ('arms', list(reversed(subject.ARMS)))]:
            broken = deepcopy(config); broken[key] = value
            with self.assertRaises(ValueError, msg=key): subject.validate_config(broken)

    def test_history_inputs_exclude_query_key_and_target_metadata(self):
        episodes = subject.data.make_split(subject.data.DIAGNOSTIC_NAME)
        first, other_query = episodes[0], episodes[1]
        self.assertEqual(subject.history_rows(first), subject.history_rows(other_query))
        changed = deepcopy(first); changed['target'] = 99; changed['query_key'] = 99
        changed['events'][-1] = ('QUERY', 3, None)
        self.assertEqual(subject.history_rows(first), subject.history_rows(changed))
        self.assertEqual(len(subject.history_rows(first)), 63)
        self.assertTrue(all(len(row) == 15 for row in subject.history_rows(first)))
        histories, indices = subject.history_index(episodes)
        self.assertEqual(len(histories), 336)
        self.assertEqual(indices[0], indices[1])
        self.assertNotEqual(indices[0], indices[2])

    def test_group_metrics_rows_and_blind_control_are_checked(self):
        episodes = subject.data.make_split(subject.data.DIAGNOSTIC_NAME)
        metrics = subject.data.prediction_metrics(episodes, [0] * len(episodes))
        evaluation = dict(loss=1., seconds=.1, preserved=True, metrics=metrics)
        subject.validate_evaluation(evaluation, episodes, retain_rows=True, blind=True)
        broken = deepcopy(evaluation); broken['metrics']['rows'][0][-1] = 7
        with self.assertRaises(ValueError): subject.validate_evaluation(broken, episodes, retain_rows=True, blind=True)
        perfect = deepcopy(evaluation)
        perfect['metrics'] = subject.data.prediction_metrics(episodes, [e['target'] for e in episodes])
        with self.assertRaises(ValueError): subject.validate_evaluation(perfect, episodes, retain_rows=True, blind=True)

    def test_screens_report_mixed_negative_outcomes_without_promoting(self):
        rows = []
        for seed in subject.SEEDS:
            for arm in subject.ARMS:
                rows.append(dict(seed=seed, arm=arm, individual=168 if arm != 'trained_blind' else 84,
                                 quartets=17 if arm == 'trained_selected' else 0))
        result = subject.screen_scores(rows)
        self.assertTrue(result['all_trained_signal'])
        self.assertTrue(result['all_trained_feature_advantage'])
        self.assertFalse(result['all_trained_reliability'])
        self.assertFalse(result['candidate_for_revia'])
        rows[0]['quartets'] = 0
        self.assertFalse(subject.screen_scores(rows)['all_trained_signal'])
        rows[1]['quartets'] = 1
        with self.assertRaises(ValueError): subject.screen_scores(rows)

    def test_snapshot_total_raw_bytes_and_fixed_reader_schedule(self):
        self.assertEqual(subject.snapshot_bytes('encoder'), 4968 * 4)
        self.assertEqual(subject.snapshot_bytes('reader'), 1056 * 4)
        self.assertEqual(6 * subject.snapshot_bytes('encoder') + 9 * subject.snapshot_bytes('reader'), 157248)
        schedule = list(subject.reader_schedule())
        self.assertEqual(len(schedule), 1008)
        self.assertEqual(schedule[0], (1, 0, 16))
        self.assertEqual(schedule[168], (169, 0, 16))
        self.assertEqual(schedule[-1], (1008, 2672, 2688))


if __name__ == '__main__':
    unittest.main()
