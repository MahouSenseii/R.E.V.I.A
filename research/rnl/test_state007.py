"""Stdlib scientific controls; these do not run a native training worker."""
import builtins
import contextlib
from collections import Counter
from copy import deepcopy
import importlib
import io
import json
import sys
import unittest
from unittest.mock import patch

import state006 as previous
import state006_data as data
import state007 as experiment


def config():
    return dict(schema='STATE-007-config-v1', models=['rnn', 'gru'], seeds=[11, 23, 37],
                checkpoints=[0, 32, 64, 128, 192, 384, 768, 1152], updates=1152, batch=16,
                train_count=3072, epochs=6, probe_count=128, diagnostic_count=512,
                final_count=512, lag_range=[1, 8],
                torch_version='2.15.0+cpu', diagnostic_name='state007_diagnostic_A',
                final_name='state007_final_A', evaluation_seed=7007,
                improvement_threshold=.10, train_threshold=.95, final_threshold=.90,
                reference_A_sha256={f'{model}/{seed}': 'a' * 64 for model in ('rnn', 'gru') for seed in (11, 23, 37)})


def metric(correct, count):
    return dict(correct=correct, count=count, accuracy=correct / count, loss=.5)


def baseline(count):
    return dict(count=count, uniform_expected_accuracy=.125, key_only_correct=count // 8,
                key_only_accuracy=.125, dictionary_oracle_correct=count, dictionary_oracle_accuracy=1.)


def cells():
    result = []
    for model in ('rnn', 'gru'):
        for seed in (11, 23, 37):
            curves = [dict(updates=step, weight_sha256=('a' if step == 192 else 'b') * 64,
                           training_probe=metric(124, 128), diagnostic=metric(384 if step < 1152 else 448, 512),
                           evaluation_seconds=.01) for step in experiment.CHECKPOINTS]
            result.append(dict(schema='STATE-007-cell-v1', status='complete', model=model, seed=seed,
                               native_executed=True, cpu_only=True, torch_version='2.15.0+cpu',
                               capacity=previous.capacity(model), reference_A_matches=True, reference_A_sha256='a' * 64,
                               training=dict(updates=1152, unique_training_episodes=3072, passes=6), curves=curves,
                               online=dict(loss=[.5] * 1152, accuracy=[.5] * 1152, update_seconds=[.001] * 1152),
                               final_holdout=dict(metric(480, 512), evaluations=1, evaluated_after_updates=1152),
                               data_identity=dict(train=f'{seed:064x}', probe=f'{seed + 100:064x}', diagnostic='d' * 64, final='f' * 64),
                               evaluation_preserved_weights_and_rng=True,
                               baselines=dict(training_probe=baseline(128), diagnostic=baseline(512), final_holdout=baseline(512)),
                               timings=dict(setup_seconds=.1, training_and_curve_seconds=2., curve_evaluation_seconds=.08, total_seconds=3.)))
    return result


class PreparationControls(unittest.TestCase):
    def test_metadata_import_never_imports_native_framework(self):
        original = builtins.__import__
        def guarded(name, *args, **kwargs):
            if name.split('.')[0] in {'torch', 'numpy'}:
                raise AssertionError('Native import forbidden')
            return original(name, *args, **kwargs)
        with patch('builtins.__import__', guarded):
            importlib.reload(experiment)
            result = experiment.validation_report(config())
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(experiment.main(['--validate-only']), 0)
        self.assertFalse(result['native_executed'])
        self.assertTrue(result['no_final_holdout_created'])
        self.assertFalse(json.loads(output.getvalue())['native_executed'])
        self.assertNotIn('torch', sys.modules)

    def test_configuration_rejects_changed_science_and_missing_reference(self):
        experiment.validate_config(config())
        for field, value in [('updates', 1024), ('evaluation_seed', True), ('final_name', 'eval_A'), ('improvement_threshold', .01)]:
            changed = config()
            changed[field] = value
            with self.assertRaises(ValueError):
                experiment.validate_config(changed)
        changed = config()
        changed['reference_A_sha256'].pop('gru/11')
        with self.assertRaises(ValueError):
            experiment.validate_config(changed)

    def test_schedule_keeps_original_pass_and_repeats_fixed_pool_six_times(self):
        schedule = list(experiment.batch_schedule())
        expected = [(first, first + 16) for first in range(0, 3072, 16)]
        self.assertEqual(len(schedule), 1152)
        for epoch in range(6):
            self.assertEqual([(first, last) for _, first, last in schedule[epoch * 192:(epoch + 1) * 192]], expected)
        with patch.object(data, 'make_split', return_value=[]) as make:
            experiment.training_pool(11)
        make.assert_called_once_with('train_A_11', count=3072, lag_range=(1, 8), seed=11, untouched=False)

    def test_probe_is_balanced_subset_and_eval_groups_are_disjoint(self):
        pool = data.make_split('state007_probe_control', 128, (1, 8), 77)
        probe = experiment.select_probe(pool)
        self.assertEqual([e['seed'] for e in probe], [e['seed'] for e in pool])
        self.assertEqual(Counter((e['query_key'], data.oracle(e['events'])) for e in probe),
                         Counter({(key, value): 4 for key in range(4) for value in range(8)}))
        self.assertEqual(experiment.baseline_records(probe), baseline(128))
        other = data.make_split('state007_other_control', 128, (1, 8), 77)
        experiment.require_disjoint(pool, other)
        with self.assertRaises(ValueError):
            experiment.require_disjoint(pool, probe)

    def test_final_holdout_gate_precedes_any_generation(self):
        with patch.object(data, 'make_split', return_value=[]) as make:
            for update in (0, 192, 1024, True):
                with self.assertRaises(RuntimeError):
                    experiment.final_holdout(config(), update)
            make.assert_not_called()
            experiment.final_holdout(config(), 1152)
        make.assert_called_once_with('state007_final_A', 512, (1, 8), 7007)

    def test_valid_progress_and_negative_results_are_not_operational_failures(self):
        good = cells()
        result = experiment.assess(good, config())
        self.assertEqual(result['status'], 'complete')
        self.assertFalse(result['candidate_for_revia'])
        self.assertTrue(result['models']['gru']['material_gain_screen'])
        self.assertTrue(result['models']['rnn']['basic_learnability_screen'])
        poor = cells()
        for cell in poor:
            cell['curves'][-1]['diagnostic'] = metric(64, 512)
            cell['curves'][-1]['training_probe'] = metric(16, 128)
            cell['final_holdout'].update(metric(64, 512))
        result = experiment.assess(poor, config())
        self.assertEqual(result['status'], 'complete')
        self.assertFalse(result['models']['gru']['material_gain_screen'])
        self.assertFalse(result['models']['rnn']['basic_learnability_screen'])

    def test_assessment_rejects_missing_duplicate_nonfinite_or_invalid_holdout(self):
        good = cells()
        self.assertEqual(experiment.assess(good[:-1], config())['status'], 'incomplete')
        self.assertEqual(experiment.assess(good + [good[0]], config())['status'], 'incomplete')
        for field in ('nonfinite', 'holdout', 'reference', 'shared_data', 'curve_type', 'baseline'):
            bad = deepcopy(good)
            if field == 'nonfinite':
                bad[0]['online']['loss'][0] = float('nan')
            elif field == 'holdout':
                bad[0]['final_holdout']['evaluated_after_updates'] = 1024
            elif field == 'reference':
                bad[0]['curves'][4]['weight_sha256'] = 'c' * 64
            elif field == 'shared_data':
                bad[0]['data_identity']['diagnostic'] = 'c' * 64
            elif field == 'curve_type':
                bad[0]['curves'].append(None)
            else:
                bad[0]['baselines']['final_holdout']['dictionary_oracle_correct'] = 0
            self.assertEqual(experiment.assess(bad, config())['status'], 'incomplete')

    def test_count_thresholds_and_directional_mixed_outcome(self):
        good = cells()
        for cell in good:
            cell['curves'][-1]['diagnostic'] = metric(436, 512)  # +52 correct, >=10 points.
            cell['curves'][-1]['training_probe'] = metric(122, 128)
            cell['final_holdout'].update(metric(461, 512))
        result = experiment.assess(good, config())
        self.assertTrue(result['models']['gru']['material_gain_screen'])
        self.assertTrue(result['models']['gru']['basic_learnability_screen'])
        good[0]['curves'][-1]['diagnostic'] = metric(435, 512)
        result = experiment.assess(good, config())
        self.assertFalse(result['models']['rnn']['material_gain_screen'])
        self.assertTrue(result['models']['rnn']['observed_all_seed_increase'])
        good[0]['curves'][-1]['diagnostic'] = metric(384, 512)
        self.assertFalse(experiment.assess(good, config())['models']['rnn']['observed_all_seed_increase'])


if __name__ == '__main__':
    unittest.main()
