"""STATE-008 scientific preparation controls; no native framework execution."""
import builtins
from copy import deepcopy
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
FILE = HERE / 'state008.py'
if FILE.exists():
    spec = importlib.util.spec_from_file_location('state008_fixture_subject', FILE)
    subject = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(subject)
else:
    subject = None


def fixture_cells(config):
    """Synthetic validator records, never measured model outcomes."""
    diagnostic = subject.data.make_split('state008_diagnostic')
    final = subject.data.make_split('state008_final')
    def evaluation(episodes, perfect):
        predictions = [e['target'] if perfect else 0 for e in episodes]
        return dict(metrics=subject.data.prediction_metrics(episodes, predictions), loss=.1, seconds=.01, preserved=True)
    cells = []
    for model in ('rnn', 'gru'):
        for seed in (11, 23, 37):
            weights = config['reference_weights'][f'{model}/{seed}']
            cells.append(dict(schema='STATE-008-cell-v1',status='complete',model=model,seed=seed,
                native_executed=True,cpu_only=True,torch_version=config['torch_version'],
                capacity=subject.previous.capacity(model),
                training=dict(updates=1152,passes=6,unique_training_episodes=3072),
                weight_checks=[dict(updates=n,observed=weights[str(n)],expected=weights[str(n)],matched=True)
                               for n in subject.CHECKPOINTS],
                data_identity=dict(train=config['reference_train'][str(seed)],
                                   diagnostic=config['set_fingerprints']['state008_diagnostic'],
                                   final=config['set_fingerprints']['state008_final']),
                original_training_probe=[dict(updates=n,correct=128,count=128,accuracy=1.,loss=.1) for n in subject.CHECKPOINTS],
                diagnostic=[dict(updates=n,evaluation=evaluation(diagnostic,n!=0)) for n in (0,192,1152)],
                final_holdout=dict(evaluated_after_updates=1152,evaluations=1,evaluation=evaluation(final,True)),
                evaluation_preserved_weights_and_rng=True,
                online_summary=dict(count=1152,loss_mean=.3,loss_min=.1,loss_max=1.,accuracy_mean=.125),
                timings=dict(setup_seconds=.1,training_seconds=.2,total_seconds=1.)))
    return cells


class ScientificControls(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(subject, 'STATE-008 worker is not implemented')

    def test_import_default_cannot_enter_native_or_generate_data(self):
        original = builtins.__import__
        def guarded(name, *args, **kwargs):
            if name.split('.')[0] in ('torch', 'numpy'):
                raise AssertionError('Native framework forbidden in preparation')
            return original(name, *args, **kwargs)
        with patch('builtins.__import__', guarded), patch.object(subject.data, 'make_split', side_effect=AssertionError('Default generated data')):
            spec.loader.exec_module(subject)
            with redirect_stdout(io.StringIO()) as output:
                self.assertEqual(subject.main([]), 0)
        self.assertFalse(json.loads(output.getvalue())['native_executed'])
        self.assertNotIn('torch', sys.modules)

    def test_configuration_rejects_mutated_science(self):
        config = subject.read_config(HERE / 'STATE-008-config.json')
        for key, value in [('updates', 192), ('evaluation_seed', True), ('diagnostic_predictions_at', [0, 1152]),
                           ('count', 512), ('torch_version', 'other')]:
            changed = deepcopy(config); changed[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                subject.validate_config(changed)
        changed = deepcopy(config); changed['reference_weights']['rnn/11'].pop('1152')
        with self.assertRaises(ValueError):
            subject.validate_config(changed)

    def test_final_split_is_held_until_exact_final_update(self):
        config = subject.read_config(HERE / 'STATE-008-config.json')
        for step in (0, 192, 1151, True):
            with self.subTest(step=step), patch.object(subject.data, 'make_split', side_effect=AssertionError('Early final generation')):
                with self.assertRaises(RuntimeError):
                    subject.final_split(config, step)

    def test_record_rows_and_aggregates_are_reconciled(self):
        episodes = subject.data.make_split('state008_diagnostic')[:4]
        predictions = [episode['target'] for episode in episodes]
        record = dict(metrics=subject.data.prediction_metrics(episodes, predictions), loss=.1, seconds=.1, preserved=True)
        subject.validate_evaluation(record, episodes)
        changed = deepcopy(record); changed['metrics']['rows'][0][-1] = (changed['metrics']['rows'][0][-1] + 1) % 8
        with self.assertRaises(ValueError):
            subject.validate_evaluation(changed, episodes)
        changed = deepcopy(record); changed['loss'] = float('nan')
        with self.assertRaises(ValueError):
            subject.validate_evaluation(changed, episodes)
        changed = deepcopy(record); changed['preserved'] = False
        with self.assertRaises(ValueError):
            subject.validate_evaluation(changed, episodes)
        changed = deepcopy(record); changed['metrics']['quartets']['correct'] = True
        with self.assertRaises(ValueError):
            subject.validate_evaluation(changed, episodes)

    def test_predeclared_screen_integer_boundaries(self):
        self.assertTrue(callable(getattr(subject, 'screen', None)), 'Explicit screen count contract is missing')
        config = subject.read_config(HERE / 'STATE-008-config.json')
        self.assertTrue(subject.screen(config,0,17,168,17)['material_signal'])
        for values in ((0,16,168,17),(0,17,167,17),(0,17,168,16)):
            self.assertFalse(subject.screen(config,*values)['material_signal'])
        self.assertTrue(subject.screen(config,0,17,605,135)['reliability'])
        for values in ((0,17,604,135),(0,17,605,134)):
            self.assertFalse(subject.screen(config,*values)['reliability'])
        self.assertFalse(subject.screen(config,0,0,0,0)['material_signal'])
        with self.assertRaises(ValueError):
            subject.screen(config,False,17,168,17)

    def test_complete_records_and_screen_boundaries(self):
        config = subject.read_config(HERE / 'STATE-008-config.json')
        cells = fixture_cells(config)
        report = subject.assess(cells, config)
        self.assertEqual(report['status'], 'complete')
        self.assertTrue(report['models']['gru']['material_signal_passed'])
        self.assertTrue(report['models']['rnn']['reliability_passed'])
        for changed in (cells[:-1], cells + [cells[0]], [{'status': 'incomplete'}]):
            self.assertEqual(subject.assess(changed, config)['status'], 'incomplete')
        broken = deepcopy(cells); broken[0]['weight_checks'][-1]['observed'] = '0' * 64
        self.assertEqual(subject.assess(broken, config)['status'], 'incomplete')

    def test_realistic_compact_output_fits_existing_capture(self):
        config = subject.read_config(HERE / 'STATE-008-config.json')
        cell = fixture_cells(config)[0]
        raw = json.dumps(cell, allow_nan=False).encode()
        self.assertLess(len(raw), 128 * 1024)
        mixed = deepcopy(cell)
        for row in mixed['diagnostic']:
            episodes = subject.data.make_split('state008_diagnostic')
            predictions = [e['target'] if i % 3 else (e['target']+1)%8 for i,e in enumerate(episodes)]
            row['evaluation']['metrics'] = subject.data.prediction_metrics(episodes,predictions)
        episodes = subject.data.make_split('state008_final')
        predictions = [e['target'] if i % 3 else (e['target']+1)%8 for i,e in enumerate(episodes)]
        mixed['final_holdout']['evaluation']['metrics'] = subject.data.prediction_metrics(episodes,predictions)
        subject.validate_cell(mixed,config)
        self.assertLess(len(json.dumps(mixed,allow_nan=False).encode()) + 16384,128*1024)
        self.assertEqual(sum(len(row['evaluation']['metrics']['rows']) for row in cell['diagnostic'])
                         + len(cell['final_holdout']['evaluation']['metrics']['rows']), 2688)
        self.assertNotIn('torch', sys.modules)


if __name__ == '__main__':
    unittest.main()
