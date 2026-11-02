"""PREFIX-004 data-only controls. No model, Windows APIs, sockets or subprocesses."""
import contextlib
from copy import deepcopy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('prefix004', HERE/'prefix004.py')
experiment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(experiment)


def stream(events, *, done=True):
    rows = [b'data: '+json.dumps(e).encode()+b'\n\n' for e in events]
    if done:
        rows.append(b'data: [DONE]\n\n')
    return io.BytesIO(b''.join(rows))


class PrefixPreparationChecks(unittest.TestCase):
    def test_frozen_public_input_and_default_validation_have_no_live_path(self):
        with patch.object(experiment.guard, '_apis', side_effect=AssertionError('No Windows API')), \
             patch.object(experiment.backend, 'fresh_port', side_effect=AssertionError('No port')), \
             patch.object(experiment, 'stage_inputs', side_effect=AssertionError('No staging')), \
             patch.object(experiment.http.client, 'HTTPConnection', side_effect=AssertionError('No HTTP')), \
             patch.object(experiment.sys, 'argv', ['prefix004.py']), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(experiment.main(), 0)
        report = json.loads(output.getvalue())
        self.assertFalse(report['binary_model_network_or_staging_execution'])
        self.assertIsNone(report['live_metrics'])
        self.assertEqual(report['planned_owned_launches'], 21)

    def test_schedule_balances_modes_and_fresh_seed_contract(self):
        rows = experiment.schedule()
        self.assertEqual(len(rows), 20)
        for repeat in range(1, 6):
            group = [r for r in rows if r['repeat'] == repeat]
            self.assertEqual({(r['variant'], r['cache_prompt']) for r in group},
                             {('shared', False), ('shared', True), ('changed', False), ('changed', True)})
        fixture = experiment.validate_fixtures(HERE/'prefix004-fixtures.json')
        u = experiment.request_body('fixture', fixture, 'shared', False)
        m = experiment.request_body('fixture', fixture, 'shared', True)
        self.assertFalse(u.pop('cache_prompt')); self.assertTrue(m.pop('cache_prompt'))
        self.assertEqual(u, m)
        self.assertNotIn('tools', u)
        self.assertFalse(u['chat_template_kwargs']['enable_thinking'])
        schema = u['response_format']['json_schema']['schema']
        self.assertNotIn('const', json.dumps(schema))
        self.assertEqual(schema['properties']['source']['enum'], ['D1', 'D2'])

    def test_sse_retains_revia_counts_and_times_from_choices_empty_final_chunk(self):
        events = [{'choices': [{'delta': {'role': 'assistant', 'content': ''}, 'finish_reason': None}]},
                  {'choices': [{'delta': {'content': '{"budget_eur":7200,"source":"D2"}'}, 'finish_reason': 'stop'}]},
                  {'choices': [], 'usage': {'prompt_tokens': 400, 'completion_tokens': 12},
                   'timings': {'cache_n': 300, 'prompt_n': 100, 'prompt_ms': 2.5, 'predicted_n': 12, 'predicted_ms': 30.5}}]
        body = stream(events)
        times = iter([10.125, 10.5])
        result = experiment.collect_sse(body.readline, 10, clock=lambda: next(times))
        self.assertEqual(result['ttft_ms'], 125)
        self.assertEqual(result['request_ms'], 500)
        self.assertEqual(result['metrics'], {'cache_n': 300, 'prompt_n': 100, 'predicted_n': 12,
                                            'prompt_ms': 2.5, 'predicted_ms': 30.5})
        self.assertEqual(result['usage']['prompt_tokens'], 400)
        self.assertEqual(result['response_content_sha256'], hashlib.sha256(result['content'].encode()).hexdigest())
        self.assertEqual(result['response_checksum_scope'], 'complete-content')

    def test_missing_metrics_remain_unknown_and_no_content_has_no_ttft(self):
        body = stream([{'choices': [{'delta': {}, 'finish_reason': 'stop'}]}])
        result = experiment.collect_sse(body.readline, 10, clock=lambda: 10.5)
        self.assertIsNone(result['ttft_ms'])
        self.assertIn('ttft_ms', result['unknown_reasons'])
        self.assertIn('prompt_ms', result['unknown_reasons'])
        self.assertTrue(all(v is None for v in result['metrics'].values()))
        self.assertEqual(experiment.metric_values({'cache_n': False, 'prompt_n': -1, 'prompt_ms': float('nan')}),
                         {name: None for name in experiment.COUNT_FIELDS+experiment.TIME_FIELDS})

    def test_incomplete_oversized_or_credential_echo_streams_are_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'termination'):
            experiment.collect_sse(stream([{'choices': []}], done=False).readline, 0)
        with self.assertRaisesRegex(RuntimeError, 'cap'):
            experiment.collect_sse(io.BytesIO(b'data: '+b'x'*experiment.LINE_CAP+b'\n').readline, 0)
        body = stream([{'choices': [], 'unexpected': 'fixture-owned-secret'}])
        with self.assertRaisesRegex(RuntimeError, 'Credential echo'):
            experiment.collect_sse(body.readline, 0, secret='fixture-owned-secret')
        body = stream([{'choices': [{'delta': {'reasoning_content': 'unexpected'}, 'finish_reason': 'stop'}]}])
        with self.assertRaisesRegex(RuntimeError, 'thinking'):
            experiment.collect_sse(body.readline, 0)

    def test_cpu_arguments_do_not_enable_gpu_rpc_or_private_slots(self):
        args = experiment.cpu_arguments(Path('staged/server.exe'), Path('staged/model.gguf'), 12345, 'fixture', Path('owned/key'))
        self.assertEqual(args[args.index('--device')+1], 'none')
        self.assertEqual(args[args.index('--gpu-layers')+1], '0')
        self.assertEqual(args[args.index('--parallel')+1], '1')
        self.assertIn('--no-slots', args)
        self.assertIn('--offline', args)
        self.assertIn('--no-warmup', args)
        self.assertNotIn('--rpc', args)
        self.assertEqual(len(experiment.RUNTIME_HASHES), 9)

    def test_token_layout_allows_a_different_seed_length_but_bounds_equal_probes(self):
        seed = list(range(249))+[999]
        shared = list(range(300))
        changed = [1000]+list(range(1, 300))
        rows = {name: {'options': {option: {'tokens': values[:], 'count': len(values)}
                                  for option in ('false', 'true')}}
                for name, values in [('seed', seed), ('shared', shared), ('changed', changed)]}
        experiment.validate_token_layout(rows)
        broken = deepcopy(rows)
        broken['changed']['options']['false']['tokens'].append(7)
        with self.assertRaisesRegex(RuntimeError, 'probe token lengths'):
            experiment.validate_token_layout(broken)
        broken = deepcopy(rows)
        for row in broken.values():
            for option in row['options'].values():
                option['tokens'] = option['tokens'][:255]
        with self.assertRaisesRegex(RuntimeError, '256..1024'):
            experiment.validate_token_layout(broken)
        self.assertEqual(experiment.ALIAS, 'rnl-prefix004-0.8b')

    def test_partial_stream_records_survive_incomplete_malformed_oversized_and_timeout(self):
        prefix = stream([{'choices': [{'delta': {'content': 'public prefix'}, 'finish_reason': None}]}], done=False).getvalue()
        readers = [io.BytesIO(prefix).readline, io.BytesIO(prefix+b'data: {bad\n\n').readline,
                   io.BytesIO(prefix+b'data: '+b'x'*experiment.LINE_CAP+b'\n').readline]
        # Individually valid lines can still exceed the complete-stream cap.
        huge = stream([{'choices': [{'delta': {'content': 'x'*15000}, 'finish_reason': None}]}]*5)
        with self.assertRaises(experiment.ResponseFailure) as overflow:
            experiment.collect_sse(huge.readline, 10, clock=lambda: 10.125)
        self.assertGreater(overflow.exception.partial['sse_bytes'], experiment.SSE_CAP)
        self.assertFalse(overflow.exception.partial['complete'])
        timed = io.BytesIO(prefix)
        def timeout(n):
            if timed.tell() < len(prefix):
                return timed.readline(n)
            raise TimeoutError('public fixture read timeout')
        readers.append(timeout)
        for reader in readers:
            with self.subTest(reader=reader), self.assertRaises(experiment.ResponseFailure) as caught:
                experiment.collect_sse(reader, 10, clock=lambda: 10.125)
            partial = caught.exception.partial
            self.assertEqual(partial['content'], 'public prefix')
            self.assertEqual(partial['ttft_ms'], 125)
            self.assertGreater(partial['sse_bytes'], 0)
            self.assertFalse(partial['complete'])
            self.assertIsNone(partial['metrics']['cache_n'])
            self.assertIn('cache_n', partial['unknown_reasons'])
            self.assertEqual(partial['response_checksum_scope'], 'collected-content-prefix')

    def test_credential_split_across_content_deltas_is_withheld_in_partial_evidence(self):
        secret = 'fixture-owned-secret'
        body = stream([{'choices': [{'delta': {'content': 'fixture-owned-'}, 'finish_reason': None}]},
                       {'choices': [{'delta': {'content': 'secret'}, 'finish_reason': 'stop'}]}])
        with self.assertRaises(experiment.ResponseFailure) as caught:
            experiment.collect_sse(body.readline, 10, secret=secret, clock=lambda: 10.125)
        partial = caught.exception.partial
        self.assertNotIn(secret, json.dumps(partial))
        self.assertTrue(partial['content_withheld'])
        self.assertEqual(partial['content'], '')
        self.assertIsNone(partial['response_content_sha256'])

    def test_http_failure_is_retained_by_the_cell_without_reading_an_error_body(self):
        response = Mock(status=503)
        connection = Mock()
        connection.getresponse.return_value = response
        row = {}
        with patch.object(experiment, 'http_deadline', return_value=contextlib.nullcontext(1)), \
             patch.object(experiment.http.client, 'HTTPConnection', return_value=connection):
            with self.assertRaises(experiment.ResponseFailure):
                experiment.capture_response(row, 'target', object(), 12345, 'fixture-secret', {})
        self.assertEqual(row['failed_phase'], 'target')
        self.assertEqual(row['target']['http_status'], 503)
        self.assertEqual(row['target']['response_status'], 'http_failure')
        self.assertIsNone(row['target']['ttft_ms'])
        response.read.assert_not_called()
        connection.close.assert_called_once()

    def test_count_limits_and_backend_usage_reconciliation_preserve_inconsistent_numbers(self):
        def result(predicted=1):
            return experiment.response_record('x', {'cache_n': 0, 'prompt_n': 400, 'predicted_n': predicted},
                                              {'prompt_tokens': 400, 'completion_tokens': predicted})
        for cap, predicted in [(1, 2), (32, 33)]:
            row = result(predicted)
            with self.assertRaisesRegex(RuntimeError, 'output cap'):
                experiment.require_counts(row, cap)
            self.assertEqual(row['metrics']['predicted_n'], predicted)
            self.assertFalse(row['counts_validity']['consistent'])
        row = result()
        row['metrics']['prompt_n'] = 399
        with self.assertRaisesRegex(RuntimeError, r'cache_n \+ prompt_n'):
            experiment.require_counts(row, 1)
        self.assertEqual(row['metrics']['prompt_n'], 399)

    def test_final_write_overrun_replaces_the_canonical_attempt_status(self):
        class Receipt:
            saved = None
            replacements = 0
            def json(self, name, value, **_):
                self.saved = deepcopy(value)
            def replace_final(self, value):
                self.saved = deepcopy(value)
                self.replacements += 1
        receipt = Receipt()
        times = iter([experiment.ATTEMPT_SECONDS-.1, experiment.ATTEMPT_SECONDS+.1])
        report = {'status': 'completed'}
        experiment.finalize_report(receipt, report, 0, clock=lambda: next(times))
        self.assertEqual(receipt.saved['status'], 'stopped')
        self.assertEqual(receipt.replacements, 1)

    def test_live_monitor_handoff_never_closes_resources_under_the_worker(self):
        server = experiment.OwnedServer([], Path('owned'), Path('owned/temp'), Mock(), 'fixture', 'fixture-secret', 100)
        server.kernel = Mock()
        server.kernel.TerminateJobObject.return_value = True
        server.kernel.WaitForSingleObject.return_value = 0
        server.kernel.GetExitCodeProcess.return_value = False
        server.info.process = 123
        server.assigned, server.job = True, 124
        server.handles = [123, 124]
        server.monitor, server.log = Mock(), Mock()
        server.monitor.is_alive.return_value = True
        server.close()
        self.assertTrue(server.record['resources_retained_due_live_monitor'])
        self.assertTrue(server.record['errors'])
        server.log.close.assert_not_called()
        server.kernel.CloseHandle.assert_not_called()


class AssessmentChecks(unittest.TestCase):
    def data(self):
        rows = []
        for repeat in range(1, 6):
            for variant in ('shared', 'changed'):
                for cache in (False, True):
                    reuse = 300 if cache and variant == 'shared' else 0
                    target = experiment.response_record('public fixture',
                        {'cache_n': reuse, 'prompt_n': 400-reuse, 'prompt_ms': 10 if reuse else 40,
                         'predicted_n': 12, 'predicted_ms': 20}, {'prompt_tokens': 400, 'completion_tokens': 12},
                        finish='stop', first=.05 if reuse else .1, elapsed=.2, done=True, complete=True, status='complete')
                    rows.append({'repeat': repeat, 'variant': variant, 'cache_prompt': cache,
                                 'grade': {'passed': True}, 'target': target})
        timer = {'resolution_seconds': 1e-6, 'empirical_min_positive_gap_seconds': 1e-6,
                 'monotonic': True, 'sample_reads': 2000}
        return rows, timer

    def test_positive_constructed_case_and_failed_resolution_overlap_mechanism_controls(self):
        rows, timer = self.data()
        result = experiment.assess_results(rows, timer)
        self.assertTrue(result['shared_causal_latency_gates_pass'])
        self.assertIsNone(result['performance_claim'])
        self.assertEqual(result['scenarios']['shared']['five_paired_TTFT_ratios'], [2]*5)
        for failure in ('coarse', 'overlap', 'no-reuse', 'inconsistent-counts', 'missing-ttft'):
            changed, clock = deepcopy(rows), deepcopy(timer)
            m = next(r for r in changed if r['variant'] == 'shared' and r['cache_prompt'])['target']
            if failure == 'coarse':
                clock['empirical_min_positive_gap_seconds'] = .01
            elif failure == 'overlap':
                m['ttft_ms'] = 120
            elif failure == 'inconsistent-counts':
                m['metrics']['prompt_n'] -= 1
            elif failure == 'missing-ttft':
                m['ttft_ms'] = None
            else:
                for row in changed:
                    if row['variant'] == 'shared' and row['cache_prompt']:
                        row['target']['metrics'].update(cache_n=0, prompt_n=400)
            with self.subTest(failure=failure):
                self.assertFalse(experiment.assess_results(changed, clock)['shared_causal_latency_gates_pass'])

    def test_changed_control_confound_optional_times_and_decode_count_limits(self):
        rows, timer = self.data()
        for row in rows:
            if row['variant'] == 'changed' and row['cache_prompt']:
                row['target']['ttft_ms'] = 50
        result = experiment.assess_results(rows, timer)
        self.assertTrue(result['changed_no_reuse_latency_confound'])
        self.assertFalse(result['shared_causal_latency_gates_pass'])
        rows, timer = self.data()
        m = next(row for row in rows if row['variant'] == 'shared' and row['cache_prompt'])['target']
        m['metrics']['prompt_ms'] = None
        m['metrics']['predicted_n'] = m['usage']['completion_tokens'] = 13
        result = experiment.assess_results(rows, timer)
        shared = result['scenarios']['shared']
        self.assertIsNotNone(shared['M_prefill']['unknown_reason'])
        self.assertIsNone(shared['five_paired_prefill_ratios'][0])
        self.assertFalse(shared['pairs'][0]['total_wall_comparable'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
