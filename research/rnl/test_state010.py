"""STATE-010 stdlib preparation checks; never import native frameworks."""
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
FILE = HERE / 'state010.py'
if FILE.exists():
    spec = importlib.util.spec_from_file_location('state010_fixture_subject', FILE)
    subject = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(subject)
else:
    subject = None


def zero_tensors(kind):
    return {name: (shape, [0.] * math.prod(shape))
            for name, shape in subject.snapshot_layout(kind).items()}


def fixture_config():
    config = subject.default_config()
    config['set_fingerprints'] = {name: subject.data.fingerprint(subject.data.make_split(name)) for name in subject.data.NAMES}
    return config


def fixture_cell(config, seed):
    """Synthetic validator record, never measured/native execution evidence."""
    def record(name, kind):
        raw, inventory = subject.encode_snapshot(zero_tensors(kind), kind)
        return dict(name=name, kind=kind, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                    parameter_sha256=subject.tensor_digest(raw, inventory), tensors=inventory)
    snapshots = [record(name, kind) for name, kind in subject.SNAPSHOTS.items()]
    by_name = {r['name']: r for r in snapshots}
    diagnostic = subject.data.make_split(subject.data.DIAGNOSTIC_NAME)
    holdout = subject.data.make_split(subject.data.FINAL_NAME)
    probe = subject.data.make_split(subject.data.TRAIN_NAMES[0])
    def evaluate(episodes, retain):
        metrics = subject.data.prediction_metrics(episodes, [0] * len(episodes))
        if not retain: del metrics['rows']
        return dict(metrics=metrics, loss=1., seconds=.01, preserved=True)
    arms = []
    for name in subject.ARMS:
        direct = name == 'direct_linear'
        arms.append(dict(name=name, updates=1008, parameters=1056 if direct else 1072,
            main_forward_macs=1024, selection='supplied_observable_key', frozen_features_preserved=True,
            evaluation_preserved=True, rng_preserved=True,
            initial_reader_sha256=by_name['direct-initial.f32' if direct else 'factorized-initial.f32']['parameter_sha256'],
            final_reader_sha256=by_name[name + '-final.f32']['parameter_sha256'],
            blind_checks=[dict(stage=stage, groups=1, count=4, logits_identical=True,
                query_pairs_correct=0, quartets_correct=0, preserved=True) for stage in ('initial', 'final')],
            curves=[dict(updates=update, training_probe=evaluate(probe, False),
                diagnostic=evaluate(diagnostic, update == 1008)) for update in subject.CURVES],
            final_holdout=dict(evaluations=1, evaluated_after_updates=1008, all_reader_fits_complete=True,
                evaluation=evaluate(holdout, True))))
    return dict(schema=subject.SCHEMA, status='complete', model='gru', seed=seed, native_executed=True,
        cpu_only=True, torch_version=config['torch_version'], encoder_capacity=subject.previous.capacity('gru'),
        data_identity=config['set_fingerprints'], shared_feature_cache=True, final_features_after_all_fits=True,
        source_encoder=dict(updates=0, frozen_preserved=True,
            loaded_parameter_sha256=config['reference_encoder_parameter_sha256'][str(seed)],
            record=config['source_encoders'][str(seed)], source_raw_result_sha256=subject.SOURCE_RAW_SHA256),
        feature_sha256={name: 'a' * 64 for name in ('train', 'diagnostic', 'final')}, arms=arms, snapshots=snapshots)


class PreparationControls(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(subject, 'STATE-010 worker is not implemented')

    def test_import_and_default_are_inert_and_native_framework_free(self):
        before = builtins.__import__
        def guarded(name, *a, **kw):
            if name.split('.')[0] in ('torch', 'numpy'):
                raise AssertionError('Native framework forbidden during preparation')
            return before(name, *a, **kw)
        with patch('builtins.__import__', guarded), patch.object(subject.data, 'make_split', side_effect=AssertionError('Default generated data')):
            spec.loader.exec_module(subject)
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(subject.main([]), 0)
            status = json.loads(out.getvalue())
            self.assertEqual(status['status'], 'prepared_unexecuted')
            self.assertFalse(status['native_executed'])
        self.assertFalse({'torch', 'numpy'}.intersection(sys.modules))

    def test_output_serialization_is_compact_lossless_and_finite(self):
        self.assertTrue(callable(getattr(subject, 'serialize_report', None)), 'Compact report serializer is missing')
        report = {'seed': 11, 'model': 'gru', 'rows': [[0, 0, 1, 2, 3, 4, 5, 6]], 'seconds': .25}
        text = subject.serialize_report(report)
        self.assertEqual(text, '{"model":"gru","rows":[[0,0,1,2,3,4,5,6]],"seconds":0.25,"seed":11}')
        self.assertEqual(json.loads(text), report)
        with self.assertRaises(ValueError): subject.serialize_report({'loss': float('nan')})

    def test_config_rejects_changed_science_or_source_parameter_identity(self):
        config = subject.default_config()
        self.assertEqual(subject.validate_config(config), config)
        for key, value in [('updates', 1009), ('encoder_updates', 1), ('evaluation_seed', True),
                           ('arms', list(reversed(subject.ARMS))), ('reader_seed_base', 900900)]:
            bad = deepcopy(config); bad[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): subject.validate_config(bad)
        for field, value in [('name', '../escape.f32'), ('kind', 'reader'), ('bytes', 19871), ('parameter_sha256', 'f' * 64)]:
            bad = deepcopy(config); bad['source_encoders']['11'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): subject.validate_config(bad)
        bad = deepcopy(config); bad['source_encoders']['11']['tensors'][0]['offset_bytes'] = 4
        with self.assertRaises(ValueError): subject.validate_config(bad)

    def test_scalar_forward_isolates_identity_from_relu_and_supplied_key(self):
        feature = [-2.] + [0.] * 31
        direct = zero_tensors('direct')
        direct['heads.weight'][1][0] = 1.
        direct['heads.bias'][1][24] = 3.
        self.assertEqual(subject.readout_reference('direct_linear', direct, feature, 0)[0], -2.)
        self.assertEqual(subject.readout_reference('direct_linear', direct, feature, 3)[0], 3.)
        factor = zero_tensors('factorized')
        factor['shared.weight'][1][0] = 1.
        factor['heads.weight'][1][0] = 1.
        self.assertEqual(subject.readout_reference('factorized_identity', factor, feature, 0)[0], -2.)
        self.assertEqual(subject.readout_reference('factorized_relu', factor, feature, 0)[0], 0.)
        feature[0] = 2.
        self.assertEqual(subject.readout_reference('factorized_identity', factor, feature, 0),
                         subject.readout_reference('factorized_relu', factor, feature, 0))
        self.assertEqual(subject.readout_reference('direct_linear', direct, feature, 0, blind=True),
                         subject.readout_reference('direct_linear', direct, feature, 3, blind=True))

    def test_snapshot_shapes_bytes_and_finite_strict_codec(self):
        self.assertEqual(subject.snapshot_bytes('direct'), 4224)
        self.assertEqual(subject.snapshot_bytes('factorized'), 4288)
        self.assertEqual(sum(subject.snapshot_bytes(k) for k in subject.SNAPSHOTS.values()), 21312)
        self.assertEqual(3 * sum(subject.snapshot_bytes(k) for k in subject.SNAPSHOTS.values()), 63936)
        for kind in ('direct', 'factorized'):
            tensors = zero_tensors(kind)
            raw, inventory = subject.encode_snapshot(tensors, kind)
            self.assertEqual(raw, b'\0' * subject.snapshot_bytes(kind))
            self.assertEqual(subject.decode_snapshot(raw, inventory, kind), tensors)
            with self.assertRaises(ValueError): subject.decode_snapshot(raw + b'\0', inventory, kind)
            bad = deepcopy(inventory); bad[-1]['offset_bytes'] = 0
            with self.assertRaises(ValueError): subject.decode_snapshot(raw, bad, kind)
            broken = deepcopy(tensors); next(iter(broken.values()))[1][0] = float('inf')
            with self.assertRaises(ValueError): subject.encode_snapshot(broken, kind)
            nonfinite = struct.pack('<f', float('nan')) + raw[4:]
            with self.assertRaises(ValueError): subject.decode_snapshot(nonfinite, inventory, kind)

    def test_checkpoint_writes_are_exclusive_names_bounded_and_hash_verified(self):
        raw, inventory = subject.encode_snapshot(zero_tensors('direct'), 'direct')
        # The task directory is explicitly writable; do not use sandbox-package Temp.
        with tempfile.TemporaryDirectory(prefix='state010-fixture-', dir=HERE.parent) as directory:
            directory = Path(directory).resolve()
            self.assertTrue(directory.is_relative_to(HERE.parent.resolve()), 'Fixture cleanup target must stay within the task preparation directory')
            record = subject.write_snapshot_bytes(directory, 'direct-initial.f32', raw, inventory, 'direct')
            self.assertEqual(record['sha256'], hashlib.sha256(raw).hexdigest())
            self.assertEqual(subject.verify_snapshot_file(directory, record), zero_tensors('direct'))
            with self.assertRaises(FileExistsError): subject.write_snapshot_bytes(directory, 'direct-initial.f32', raw, inventory, 'direct')
            with self.assertRaises(ValueError): subject.write_snapshot_bytes(directory, '../escape.f32', raw, inventory, 'direct')
            (directory / record['name']).write_bytes(raw[:-4])
            with self.assertRaises(ValueError): subject.verify_snapshot_file(directory, record)

    def test_fresh_version_metrics_are_recomputed_not_prior_version_globals(self):
        episodes = subject.data.make_split(subject.data.DIAGNOSTIC_NAME)
        metrics = subject.data.prediction_metrics(episodes, [0] * 672)
        record = dict(metrics=metrics, loss=1., seconds=.1, preserved=True)
        subject.validate_evaluation(record, episodes, retain_rows=True)
        bad = deepcopy(record); bad['metrics']['rows'][0][-1] = 7
        with self.assertRaises(ValueError): subject.validate_evaluation(bad, episodes, retain_rows=True)
        early = deepcopy(record); del early['metrics']['rows']
        subject.validate_evaluation(early, episodes, retain_rows=False)
        early['metrics']['baselines']['dictionary']['individual']['correct'] = 0
        with self.assertRaises(ValueError): subject.validate_evaluation(early, episodes, retain_rows=False)

    def test_history_cache_interface_excludes_query_and_target_metadata(self):
        episodes = subject.data.make_split(subject.data.DIAGNOSTIC_NAME)
        first, other = episodes[:2]
        self.assertEqual(subject.history_rows(first), subject.history_rows(other))
        bad = deepcopy(first); bad['query_key'] = 99; bad['target'] = 99; bad['events'][-1] = ('QUERY', 3, None)
        self.assertEqual(subject.history_rows(first), subject.history_rows(bad))
        self.assertEqual(len(subject.history_rows(first)), 63)

    def test_blind_pipeline_control_requires_both_stages_and_identical_logits(self):
        record = dict(stage='initial', groups=1, count=4, logits_identical=True,
                      query_pairs_correct=0, quartets_correct=0, preserved=True)
        self.assertEqual(subject.validate_blind_check(record, 'initial'), record)
        for key, value in [('stage', 'final'), ('count', 3), ('logits_identical', False), ('query_pairs_correct', 1)]:
            bad = deepcopy(record); bad[key] = value
            with self.assertRaises(ValueError, msg=key): subject.validate_blind_check(bad, 'initial')

    def test_screens_require_relu_signal_and_advantage_over_both_controls(self):
        rows = [dict(seed=s, arm=a, individual=168, quartets=17 if a == 'factorized_relu' else 0)
                for s in subject.SEEDS for a in subject.ARMS]
        result = subject.screen_scores(rows)
        self.assertTrue(result['all_relu_signal'])
        self.assertTrue(result['all_relu_advantage'])
        self.assertFalse(result['all_relu_reliability'])
        self.assertFalse(result['candidate_for_revia'])
        rows[0]['quartets'] = 1
        self.assertFalse(subject.screen_scores(rows)['all_relu_advantage'])
        rows[2]['quartets'] = 0
        self.assertFalse(subject.screen_scores(rows)['all_relu_signal'])

    def test_fixed_reader_schedule_has_no_encoder_updates(self):
        config = subject.default_config()
        self.assertEqual(config['encoder_updates'], 0)
        schedule = list(subject.reader_schedule())
        self.assertEqual(len(schedule), 1008)
        self.assertEqual(schedule[0], (1, 0, 16))
        self.assertEqual(schedule[168], (169, 0, 16))
        self.assertEqual(schedule[-1], (1008, 2672, 2688))

    def test_complete_record_requires_saved_encoder_paired_initializers_and_late_holdout(self):
        config = fixture_config()
        cells = [fixture_cell(config, seed) for seed in subject.SEEDS]
        result = subject.assess(cells, config)
        self.assertFalse(result['all_relu_signal'])
        self.assertFalse(result['candidate_for_revia'])
        bad = deepcopy(cells[0]); bad['source_encoder']['updates'] = 1
        with self.assertRaises(ValueError): subject.validate_cell(bad, config)
        bad = deepcopy(cells[0]); bad['arms'][2]['initial_reader_sha256'] = '1' * 64
        with self.assertRaises(ValueError): subject.validate_cell(bad, config)
        bad = deepcopy(cells[0]); bad['arms'][0]['final_holdout']['all_reader_fits_complete'] = False
        with self.assertRaises(ValueError): subject.validate_cell(bad, config)


if __name__ == '__main__':
    unittest.main()
