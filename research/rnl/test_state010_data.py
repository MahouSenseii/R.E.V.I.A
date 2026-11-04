"""STATE-010 stdlib quartet controls; no framework or model execution."""
import builtins
from collections import Counter
from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
FILE = HERE / 'state010_data.py'
if FILE.exists():
    SPEC = importlib.util.spec_from_file_location('state010_data_fixture_subject', FILE)
    subject = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(subject)
else:
    subject = None


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def quartet():
    version = 'STATE-010-quartet-v1'
    gid = digest([version, 'fixture', 10010, [0, 1], [4, 7]])
    base = [('SET', key, key) for key in range(4)] + [('NOOP', None, None)] * 59
    result = []
    for assignment in (0, 1):
        values = (4, 7) if assignment == 0 else (7, 4)
        for query in (0, 1):
            events = list(base)
            events[60], events[61], events[62] = ('SET', 0, values[0]), ('SET', 1, values[1]), ('SET', 2, 3)
            events.append(('QUERY', query, None))
            result.append(dict(events=events, seed=digest([version, gid, assignment, query]),
                               group_id=gid, group_index=0, assignment=assignment,
                               query_variant=query, query_key=query, lag=3 - query,
                               target=values[query], keys=[0, 1], values=[4, 7],
                               designated_positions=[60, 61]))
    return result


class DataControls(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(subject, 'STATE-010 data implementation is absent')

    def test_import_has_no_framework_or_generation_side_effects(self):
        original_import = builtins.__import__
        def no_framework(name, *args, **kwargs):
            if name.split('.')[0] in ('torch', 'numpy'):
                raise AssertionError('Native framework forbidden in data preparation')
            return original_import(name, *args, **kwargs)
        with patch('builtins.__import__', no_framework), patch('random.Random', side_effect=AssertionError('Data generation on import')):
            fresh = importlib.util.module_from_spec(SPEC)
            SPEC.loader.exec_module(fresh)
        self.assertNotIn('torch', sys.modules)
        self.assertNotIn('numpy', sys.modules)
        self.assertEqual(fresh.VERSION, 'STATE-010-quartet-v1')

    def test_frozen_names_and_seed_reject_old_or_arbitrary_inputs(self):
        self.assertEqual((subject.GROUPS, subject.COUNT, subject.SEED), (168, 672, 10010))
        self.assertEqual(subject.TRAIN_NAMES, tuple('state010_train_' + str(i) for i in range(4)))
        self.assertEqual(subject.DIAGNOSTIC_NAME, 'state010_diagnostic')
        self.assertEqual(subject.FINAL_NAME, 'state010_final')
        self.assertEqual(subject.NAMES, subject.TRAIN_NAMES + (subject.DIAGNOSTIC_NAME, subject.FINAL_NAME))
        for name, seed in [('state009_final', 10010), ('state008_final', 10010), ('wrong', 10010),
                           (subject.NAMES[0], 9009), (subject.NAMES[0], True), (subject.NAMES[0], 10010.)]:
            with self.subTest(name=name, seed=seed), self.assertRaises(ValueError):
                subject.make_split(name, seed)
        import state009_data as prior
        with self.assertRaisesRegex(ValueError, 'Variant identity mismatch'):
            subject.validate_split(prior.make_split(prior.FINAL_NAME))

    def test_hand_quartet_rows_oracle_and_query_blind_control(self):
        result = subject.prediction_metrics(quartet(), [4, 7, 7, 4])
        self.assertEqual(result['individual'], dict(correct=4, count=4, accuracy=1.))
        self.assertEqual(result['quartets']['correct'], 1)
        self.assertEqual(result['rows'][0], [0, 0, 0, 0, 3, 4, 3, 4])
        self.assertEqual(result['baselines']['last_set']['individual']['correct'], 0)
        self.assertEqual(result['baseline_agreement']['dictionary']['both_correct'], 4)
        for predictions, query_pairs, value_pairs in (([4, 4, 7, 7], 0, 1), ([4, 7, 4, 7], 1, 0)):
            metrics = subject.prediction_metrics(quartet(), predictions)
            self.assertEqual(metrics['individual']['correct'], 2)
            self.assertEqual(metrics['quartets']['correct'], 0)
            self.assertEqual(metrics['query_pairs']['correct'], query_pairs)
            self.assertEqual(metrics['value_pairs']['correct'], value_pairs)

    def test_invalid_predictions_metadata_and_related_history_are_rejected(self):
        for predictions in ([4, 7, 7], [True, 7, 7, 4], [4., 7, 7, 4], [8, 7, 7, 4]):
            with self.assertRaises(ValueError):
                subject.prediction_metrics(quartet(), predictions)
        for mutation in ('target', 'history', 'query_value', 'lag', 'missing_variant', 'identity'):
            episodes = quartet()
            if mutation == 'target': episodes[0]['target'] = 2
            elif mutation == 'history': episodes[2]['events'][10] = ('SET', 3, 5)
            elif mutation == 'query_value': episodes[0]['events'][-1] = ('QUERY', 0, 4)
            elif mutation == 'lag': episodes[0]['lag'] = 2
            elif mutation == 'identity': episodes[0]['seed'] = 'a' * 64
            else: episodes.pop()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                subject.prediction_metrics(episodes, [4] * len(episodes))

    def test_all_six_sets_balance_reproduce_and_keep_metric_indices_local(self):
        fingerprints = []
        for name in subject.NAMES:
            episodes = subject.make_split(name)
            report = subject.validate_split(episodes)
            self.assertEqual((report['count'], report['groups']), (672, 168))
            self.assertEqual(Counter((e['query_key'], e['target']) for e in episodes),
                             Counter({(k, v): 21 for k in range(4) for v in range(8)}))
            self.assertEqual(Counter(e['lag'] for e in episodes), Counter({lag: 96 for lag in range(2, 9)}))
            self.assertEqual({e['group_index'] for e in episodes}, set(range(168)))
            self.assertEqual(report['baselines']['constant0']['individual']['correct'], 84)
            self.assertEqual(report['baselines']['query_key']['individual']['correct'], 84)
            self.assertEqual(report['baselines']['last_set']['individual']['correct'], 0)
            self.assertEqual(report['baselines']['dictionary']['quartets']['correct'], 168)
            blind_predictions = [episode['values'][episode['assignment']] for episode in episodes]
            blind = subject.prediction_metrics(episodes, blind_predictions)
            self.assertEqual(blind['query_pairs']['correct'], 0)
            self.assertEqual(blind['quartets']['correct'], 0)
            self.assertEqual(subject.fingerprint(episodes), subject.fingerprint(subject.make_split(name)))
            fingerprints.append(subject.fingerprint(episodes))
        self.assertEqual(len(set(fingerprints)), 6)
        with self.assertRaisesRegex(ValueError, 'Group index collision'):
            subject.prediction_metrics(subject.make_split(subject.TRAIN_NAMES[0]) +
                                       subject.make_split(subject.TRAIN_NAMES[1]), [0] * 1344)

    def test_all_new_histories_disjoint_from_old_sets_and_original_training(self):
        import state006_data as original
        import state007 as prior007
        import state008_data as prior008
        import state009_data as prior009
        config007 = prior007.read_config(HERE / 'STATE-007-config.json')
        def references():
            for seed in original.SEEDS:
                name = 'train_A_' + str(seed)
                yield from original.iter_split(name, **original.split_specs()[name])
            yield from prior007.diagnostic_split(config007)
            yield from prior007.final_holdout(config007, 1152)
            for namespace in (prior008, prior009):
                for name in namespace.NAMES:
                    yield from namespace.make_split(name)
        report = subject.validate_disjoint([subject.make_split(name) for name in subject.NAMES], references())
        self.assertEqual(report, dict(status='passed', new_splits=6, new_groups=1008,
                                      new_unique_histories=2016, original_episodes=15616))

    def test_disjointness_rejects_overlap_and_accepts_intended_query_twins(self):
        original = dict(events=[('SET', k, 0) for k in range(4)] +
                         [('NOOP', None, None)] * 59 + [('QUERY', 0, None)], seed='0' * 64)
        self.assertEqual(subject.validate_disjoint([quartet()], iter([original]))['new_unique_histories'], 2)
        with self.assertRaises(ValueError):
            subject.validate_disjoint([quartet(), deepcopy(quartet())], iter([original]))
        twins = deepcopy(quartet())
        for e in twins:
            e['group_id'] = 'b' * 64
            e['seed'] = digest(['STATE-010-quartet-v1', e['group_id'], e['assignment'], e['query_variant']])
        with self.assertRaisesRegex(ValueError, 'Observable history overlap'):
            subject.validate_disjoint([quartet(), twins], iter([original]))
        same_history = dict(events=quartet()[0]['events'], seed='0' * 64)
        with self.assertRaisesRegex(ValueError, 'New/original training history overlap'):
            subject.validate_disjoint([quartet()], iter([same_history]))

    def test_fingerprint_is_order_sensitive_and_sibling_events_are_independent(self):
        episodes = quartet()
        raw = b''.join(json.dumps(e, sort_keys=True, separators=(',', ':')).encode() + b'\n' for e in episodes)
        self.assertEqual(subject.fingerprint(episodes), hashlib.sha256(raw).hexdigest())
        self.assertNotEqual(subject.fingerprint(episodes[::-1]), subject.fingerprint(episodes))
        generated = subject.make_split(subject.DIAGNOSTIC_NAME)
        siblings = deepcopy(generated[1:4])
        generated[0]['events'][4] = ('SET', 2, 0)
        self.assertEqual(generated[1:4], siblings)


if __name__ == '__main__':
    unittest.main()
