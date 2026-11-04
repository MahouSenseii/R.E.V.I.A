"""STATE-008 stdlib data/metric controls. No Torch or model execution."""
from collections import Counter
from copy import deepcopy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
if (ROOT / 'state008_data.py').exists():
    SPEC = importlib.util.spec_from_file_location('state008_data_test_subject', ROOT / 'state008_data.py')
    subject = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(subject)
else:
    subject = None


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def quartet():
    version = 'STATE-008-quartet-v1'
    gid = digest([version, 'fixture', 8008, [0, 1], [4, 7]])
    base = [('SET', key, key) for key in range(4)] + [('NOOP', None, None)] * 59
    result = []
    for assignment in range(2):
        values = (4, 7) if assignment == 0 else (7, 4)
        for query in range(2):
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
        self.assertIsNotNone(subject, 'STATE-008 data implementation is absent')

    def test_hand_quartet_and_prediction_row_contract(self):
        result = subject.prediction_metrics(quartet(), [4, 7, 7, 4])
        self.assertEqual(result['individual'], dict(correct=4, count=4, accuracy=1.))
        self.assertEqual(result['quartets']['correct'], 1)
        self.assertEqual(result['query_pairs']['correct'], 2)
        self.assertEqual(result['value_pairs']['correct'], 2)
        self.assertEqual(result['rows'][0], [0, 0, 0, 0, 3, 4, 3, 4])
        self.assertEqual(result['baselines']['last_set']['individual']['correct'], 0)
        self.assertEqual(result['baseline_agreement']['dictionary']['both_correct'], 4)
        self.assertNotIn('torch', sys.modules)
        self.assertNotIn('numpy', sys.modules)

    def test_query_blind_and_assignment_blind_rules_fail_whole_group(self):
        for predictions, query_pairs, value_pairs in (([4, 4, 7, 7], 0, 1), ([4, 7, 4, 7], 1, 0)):
            result = subject.prediction_metrics(quartet(), predictions)
            self.assertEqual(result['individual']['correct'], 2)
            self.assertEqual(result['quartets']['correct'], 0)
            self.assertEqual(result['query_pairs']['correct'], query_pairs)
            self.assertEqual(result['value_pairs']['correct'], value_pairs)

    def test_reject_bad_predictions_metadata_and_unintended_history_change(self):
        for predictions in ([4, 7, 7], [True, 7, 7, 4], [4., 7, 7, 4], [8, 7, 7, 4]):
            with self.assertRaises(ValueError):
                subject.prediction_metrics(quartet(), predictions)
        for mutation in ('target', 'history', 'query_value', 'lag', 'missing_variant'):
            episodes = quartet()
            if mutation == 'target': episodes[0]['target'] = 2
            elif mutation == 'history': episodes[2]['events'][10] = ('SET', 3, 5)
            elif mutation == 'query_value': episodes[0]['events'][-1] = ('QUERY', 0, 4)
            elif mutation == 'lag': episodes[0]['lag'] = 2
            else: episodes.pop()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                subject.prediction_metrics(episodes, [4] * len(episodes))

    def test_disjointness_allows_intended_twins_and_rejects_cross_split_history(self):
        original = dict(events=[('SET', k, 0) for k in range(4)] +
                         [('NOOP', None, None)] * 59 + [('QUERY', 0, None)], seed='0' * 64)
        report = subject.validate_disjoint([quartet()], iter([original]))
        self.assertEqual(report['new_unique_histories'], 2)
        self.assertEqual(report['original_episodes'], 1)
        with self.assertRaises(ValueError):
            subject.validate_disjoint([quartet(), deepcopy(quartet())], iter([original]))
        twins = deepcopy(quartet())
        for e in twins:
            e['group_id'] = 'b' * 64
            e['seed'] = digest(['STATE-008-quartet-v1', e['group_id'], e['assignment'], e['query_variant']])
        with self.assertRaisesRegex(ValueError, 'Observable history overlap'):
            subject.validate_disjoint([quartet(), twins], iter([original]))
        same_history = dict(events=quartet()[0]['events'], seed='0' * 64)
        with self.assertRaisesRegex(ValueError, 'New/original training history overlap'):
            subject.validate_disjoint([quartet()], iter([same_history]))

    def test_fingerprint_uses_canonical_order_and_does_not_mutate_siblings(self):
        episodes = quartet()
        raw = b''.join(json.dumps(e, sort_keys=True, separators=(',', ':')).encode() + b'\n' for e in episodes)
        self.assertEqual(subject.fingerprint(episodes), hashlib.sha256(raw).hexdigest())
        self.assertNotEqual(subject.fingerprint(episodes[::-1]), subject.fingerprint(episodes))
        generated = subject.make_split(subject.NAMES[0])
        before = deepcopy(generated[1:4])
        generated[0]['events'][4] = ('SET', 2, 0)
        self.assertEqual(generated[1:4], before)

    def test_full_splits_balance_controls_and_reproducibility(self):
        splits = [subject.make_split(name) for name in subject.NAMES]
        self.assertEqual((subject.GROUPS, subject.COUNT, subject.SEED), (168, 672, 8008))
        for name, episodes in zip(subject.NAMES, splits):
            report = subject.validate_split(episodes)
            self.assertEqual(report['count'], 672)
            self.assertEqual(report['groups'], 168)
            self.assertEqual(Counter((e['query_key'], e['target']) for e in episodes),
                             Counter({(k, v): 21 for k in range(4) for v in range(8)}))
            self.assertEqual(Counter(e['lag'] for e in episodes), Counter({lag: 96 for lag in range(2, 9)}))
            self.assertEqual(report['baselines']['constant0']['individual']['correct'], 84)
            self.assertEqual(report['baselines']['query_key']['individual']['correct'], 84)
            self.assertEqual(report['baselines']['last_set']['individual']['correct'], 0)
            self.assertEqual(report['baselines']['dictionary']['quartets']['correct'], 168)
            self.assertEqual(subject.fingerprint(episodes), subject.fingerprint(subject.make_split(name)))
        self.assertNotEqual(subject.fingerprint(splits[0]), subject.fingerprint(splits[1]))
        for name, seed in [('wrong', 8008), (subject.NAMES[0], 11), (subject.NAMES[0], True)]:
            with self.assertRaises(ValueError): subject.make_split(name, seed)


if __name__ == '__main__':
    unittest.main()
