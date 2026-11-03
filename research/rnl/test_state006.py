"""Small stdlib controls; these do not validate the unexecuted native worker."""
import builtins
import contextlib
import io
import importlib
import json
from collections import Counter
from copy import deepcopy
import unittest
from unittest.mock import patch

import state006_data as data
import state006 as experiment


def passing_cells():
    metrics = dict(checkpoint_A_A=.98, final_A=.97, final_B=.97,
                   final_OOD=.94, untouched=.6, overwrite=.98,
                   state_reset=.12, state_exchange=.13, clean_reset=1.)
    return [dict(schema='STATE-006-cell-v1', status='complete', model=model,
                 seed=seed, metrics={**metrics, 'final_OOD': .94 if model == 'gru' else .81})
            for seed in (11, 23, 37) for model in ('gru', 'rnn')]


class DataControls(unittest.TestCase):
    def test_independent_oracle_last_write_and_invalid_query(self):
        events = [('SET', 0, 2), ('SET', 1, 4), ('SET', 0, 7),
                  ('NOOP', None, None), ('QUERY', 0, None)]
        self.assertEqual(data.oracle(events), 7)
        with self.assertRaises(ValueError):
            data.oracle([('QUERY', 3, None)])
        with self.assertRaises(ValueError):
            data.oracle(events + [('QUERY', 1, None)])

    def test_determinism_lag_overwrite_and_no_answer_channel(self):
        for lag in (1, 8, 16, 32):
            episode = data.make_episode(123, lag)
            self.assertEqual(episode, data.make_episode(123, lag))
            events = episode['events']
            self.assertEqual(len(events), 64)
            self.assertEqual([e[0] for e in events[:4]], ['SET'] * 4)
            self.assertEqual({e[1] for e in events[:4]}, set(range(4)))
            key = events[-1][1]
            writes = [(i, e[2]) for i, e in enumerate(events) if e[:2] == ('SET', key)]
            self.assertEqual(63 - writes[-1][0], lag)
            self.assertNotEqual(writes[-1][1], writes[-2][1])
            rows, target = data.encode(events)
            self.assertEqual(target, data.oracle(events))
            self.assertEqual((len(rows), len(rows[0])), (64, 15))
            self.assertEqual(rows[-1][7:], [0.] * 8)
            changed = list(events)
            i, value = writes[-1]
            changed[i] = ('SET', key, (value + 1) % 8)
            changed_rows, changed_target = data.encode(changed)
            self.assertNotEqual(target, changed_target)
            self.assertEqual(rows[-1], changed_rows[-1])
            self.assertEqual(sum(a != b for a, b in zip(rows, changed_rows)), 1)
        probe = data.make_episode(123, 1, untouched=True)
        self.assertEqual(sum(e[:2] == ('SET', probe['query_key']) for e in probe['events']), 1)

    def test_group_disjoint_balance_and_manifest_reproducible(self):
        a = data.make_split('control_A', 64, (1, 8), 55)
        b = data.make_split('control_B', 64, (1, 8), 55)
        self.assertEqual(a, data.make_split('control_A', 64, (1, 8), 55))
        self.assertTrue({x['seed'] for x in a}.isdisjoint(x['seed'] for x in b))
        self.assertEqual(Counter((x['query_key'], data.oracle(x['events'])) for x in a),
                         Counter({(k, v): 2 for k in range(4) for v in range(8)}))
        self.assertEqual(Counter(x['lag'] for x in a), Counter({i: 8 for i in range(1, 9)}))
        manifest = data.dataset_manifest()
        self.assertEqual(manifest, data.dataset_manifest())
        self.assertEqual(len(manifest['sha256']), 64)
        self.assertEqual(manifest['splits']['eval_OOD']['count'], 512)
        with self.assertRaises(ValueError):
            data.make_split('bad', 33, (1, 8), 55)

    def test_untouched_split_balances_initial_answers(self):
        episodes = data.make_split('probe', 128, (1, 1), 55, untouched=True)
        self.assertEqual(Counter((e['query_key'], data.oracle(e['events'])) for e in episodes),
                         Counter({(k, v): 4 for k in range(4) for v in range(8)}))
        for e in episodes:
            self.assertEqual(sum(x[:2] == ('SET', e['query_key']) for x in e['events']), 1)

    def test_older_history_is_independent_of_eventual_answer(self):
        # Counterfactual targets must leave the history unchanged except for
        # the designated final write and its required different predecessor.
        histories = []
        saw_answer_in_older_history = False
        for target in range(8):
            episode = data._episode('prefix-independence', 18, False, key=0, target=target)
            writes = [i for i, e in enumerate(episode['events']) if e[:2] == ('SET', 0)]
            self.assertGreaterEqual(len(writes), 3)
            last, previous = writes[-1], writes[-2]
            self.assertNotEqual(episode['events'][last][2], episode['events'][previous][2])
            histories.append([e for i, e in enumerate(episode['events']) if i not in (last, previous)])
            saw_answer_in_older_history |= any(episode['events'][i][2] == target for i in writes[:-2])
        self.assertTrue(saw_answer_in_older_history)
        self.assertTrue(all(history == histories[0] for history in histories))


class PreparationControls(unittest.TestCase):
    def test_capacity_and_offline_default_never_import_framework(self):
        self.assertEqual(experiment.capacity('gru'), dict(parameters=4968, dense_macs=289024, state_bytes=128))
        self.assertEqual(experiment.capacity('rnn'), dict(parameters=4964, dense_macs=279896, state_bytes=236))
        original = builtins.__import__
        def guarded(name, *args, **kwargs):
            if name.split('.')[0] in {'torch', 'numpy'}:
                raise AssertionError('native framework forbidden')
            return original(name, *args, **kwargs)
        with patch('builtins.__import__', guarded), contextlib.redirect_stdout(io.StringIO()) as out:
            importlib.reload(experiment)
            self.assertEqual(experiment.main([]), 0)
        report = json.loads(out.getvalue())
        self.assertEqual(report['status'], 'prepared_native_unverified')
        self.assertFalse(report['native_executed'])

    def test_assessment_incomplete_underfit_and_partial_success(self):
        good = passing_cells()
        self.assertEqual(experiment.assess(good)['status'], 'go')
        self.assertEqual(experiment.assess(good[:-1])['status'], 'incomplete')
        for field, value in [('checkpoint_A_A', .8), ('final_A', .94),
                             ('final_B', .94), ('overwrite', .94),
                             ('state_reset', .3), ('state_exchange', .3),
                             ('clean_reset', .99), ('final_OOD', .89)]:
            bad = deepcopy(good)
            bad[0]['metrics'][field] = value
            result = experiment.assess(bad)
            self.assertFalse(result['go'], field)
            self.assertTrue(result['failures'], field)
        bad = deepcopy(good)
        bad[0]['metrics']['final_A'] = .95
        self.assertFalse(experiment.assess(bad)['go'])  # 3pp old-task loss
        bad = deepcopy(good)
        bad[1]['metrics']['final_OOD'] = .90
        self.assertFalse(experiment.assess(bad)['go'])  # no gating advantage
        for altered in (good + [good[0]], [{**good[0], 'status': 'failed'}] + good[1:]):
            self.assertFalse(experiment.assess(altered)['go'])
        for invalid in (float('nan'), float('inf'), 10 ** 400, True, 1.1, None):
            bad = deepcopy(good)
            bad[0]['metrics']['final_A'] = invalid
            self.assertEqual(experiment.assess(bad)['status'], 'incomplete')

    def test_malformed_cells_never_raise_or_promote(self):
        for cells in (None, {}, [None], [{}], [dict(model=[], seed=11)],
                      [dict(model='gru', seed=True)], [dict(model='other', seed=11)]):
            with self.subTest(cells=cells):
                self.assertEqual(experiment.assess(cells)['status'], 'incomplete')


if __name__ == '__main__':
    unittest.main()
