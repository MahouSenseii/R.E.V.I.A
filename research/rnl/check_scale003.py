"""Independent data-only RNL-003 checker. Never executes experiment workers.

Regenerates fixed inputs/model, checks small-D FP32 recurrence independently,
checks emitted all-size equality evidence, and evaluates preregistered timing
controls. Whole-process memory is reported, never turned into allocator totals.
"""
import argparse
from functools import lru_cache
import json
import math
from pathlib import Path
import re
import statistics
import struct
import unittest

from check_first001 import f32, float_bits, oracle_step

DIMS = (16, 64, 256, 1024)
BLOCKS = (1, 2, 10)
MASK = (1 << 64)-1
HEX = re.compile(r'^[0-9a-f]{16}$')


def xorshift(seed):
    value = seed
    while True:
        value ^= (value << 13) & 0xffffffff
        value ^= value >> 17
        value ^= (value << 5) & 0xffffffff
        value &= 0xffffffff
        yield value


@lru_cache(None)
def generate_model(d):
    draws = xorshift(0x524e4c33)
    experts = []
    for _ in range(3):
        gates = [next(draws) % 3 - 1 for _ in range(d)]
        indices = bytearray(next(draws) % 3 for _ in range(d*d))
        experts.append({'gates': gates, 'indices': indices})
    return {'inputs': d, 'states': d, 'decay': f32(.55), 'experts': experts}


@lru_cache(None)
def generate_corpus(d, block):
    draws = xorshift(0x494e5033)
    result = []
    for group in range(100 // block):
        signal = [.25 if next(draws) & 1 else -.25 for _ in range(d)]
        signal[0] = .25 + group / 1024
        result.extend([signal] * block)
    assert len(result) == 100
    assert all(x != 0 and abs(x*1024) <= 355 for row in result for x in row)
    assert result[-1] != result[0]
    return result


def decode_bits(bits):
    return [struct.unpack('<f', struct.pack('<I', b))[0] for b in bits]


def fnv_bytes(data, value=14695981039346656037):
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & MASK
    return value


def fnv_words(words, value=14695981039346656037):
    for word in words:
        value = fnv_bytes(struct.pack('<I', word), value)
    return value


@lru_cache(None)
def model_fingerprint(d):
    value = 14695981039346656037
    for e in generate_model(d)['experts']:
        value = fnv_words([float_bits(v) for v in e['gates']], value)
        value = fnv_bytes(e['indices'], value)
    return f'{value:016x}'


@lru_cache(None)
def corpus_fingerprint(d, block):
    value = 14695981039346656037
    for signal in generate_corpus(d, block):
        value = fnv_words([float_bits(v) for v in signal], value)
    return f'{value:016x}'


def int_product(model, expert, signal):
    units = [int(x*1024) for x in signal]
    assert all(x*1024 == q for x, q in zip(signal, units))
    d = model['inputs']
    indices = model['experts'][expert]['indices']
    result = []
    for row in range(d):
        integer = sum((indices[row*d+col]-1)*units[col] for col in range(d))
        assert abs(integer) <= 363520
        result.append(integer/1024)
    return result


def oracle_model(model):
    return {**model, 'experts': [
        {**e, 'weights': [i-1 for i in e['indices']]}
        for e in model['experts']
    ]}


def assess_timing(result):
    """Pure preregistered decision, leaving cross-scenario control invalidation explicit."""
    freq = result['clock']['frequency_hz']
    quantum = result['clock']['empirical_min_ticks']
    pairs = result['pairs']
    ut = [p['U']['ticks'] for p in pairs]
    mt = [p['M']['ticks'] for p in pairs]
    ratios = [u/m for u, m in zip(ut, mt)]
    adequate = all(t/freq >= .005 and t >= 10000*quantum for t in ut+mt)
    ratio = statistics.median(ratios)
    win = adequate and ratio >= 1.10 and min(ratios) > 1.05 and max(mt) < min(ut)
    return {
        'dimension': result['dimension'], 'block': result['block'],
        'median_paired_u_over_m': ratio, 'paired_ratios': ratios,
        'adequate_resolution': adequate, 'local_benefit_rule_passes': win,
        'dimension_control_invalid': False,
        'u_ns_min_median_max': [t/freq*1e9/result['event_count'] for t in
                               (min(ut), statistics.median(ut), max(ut))],
        'm_ns_min_median_max': [t/freq*1e9/result['event_count'] for t in
                               (min(mt), statistics.median(mt), max(mt))],
    }


def assess_all(results):
    assessments = [assess_timing(r) for r in results]
    invalid = {a['dimension'] for a in assessments
               if a['block'] == 1 and a['median_paired_u_over_m'] >= 1.10}
    for a in assessments:
        a['dimension_control_invalid'] = a['dimension'] in invalid
        a['supported_local_benefit'] = (a['local_benefit_rule_passes'] and
                                        not a['dimension_control_invalid'])
    return assessments


class ScaleChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(ARGS.results)
        cls.validation = cls.read('validation')

    @classmethod
    def read(cls, name):
        path = cls.root/name/'result.json'
        if not path.is_file():
            raise AssertionError('Missing RNL-003 result: '+name)
        return json.loads(path.read_text('utf-8'))

    def fingerprint(self, r, d, block):
        self.assertEqual(r['dimension'], d)
        self.assertEqual(r['block'], block)
        self.assertEqual(r['model_fingerprint'], model_fingerprint(d))
        self.assertEqual(r['corpus_fingerprint'], corpus_fingerprint(d, block))

    def fp(self, r):
        fp = r['fp_environment']
        self.assertEqual(fp['fe_round_start'], fp['FE_TONEAREST'])
        self.assertEqual(fp['fe_round_start'], fp['fe_round_end'])
        self.assertEqual(fp['mxcsr_control_before'], fp['mxcsr_control_after'])
        self.assertEqual(fp['mxcsr_control_before'],
                         self.validation['fp_environment']['mxcsr_control_before'])
        self.assertEqual(fp['mxcsr_control_before'] & 0x6000, 0)
        self.assertTrue(fp['controls_unchanged'])

    def near(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            self.assertTrue(math.isfinite(a) and math.isfinite(b))
            self.assertLessEqual(abs(a-b), 1e-6)

    def counts(self, c, events, mode, block):
        self.assertEqual(c['events'], events)
        self.assertTrue(0 <= c['cache_hits'] <= events*(block-1)//block)
        self.assertEqual(c['matrix_evaluations'], events-c['cache_hits'])
        self.assertEqual(c['cache_lookups'], events if mode == 'M' else 0)
        for key in ('cache_hits', 'matrix_evaluations', 'cache_lookups',
                    'key_comparisons', 'expert_comparisons'):
            self.assertIsInstance(c[key], int)
            self.assertGreaterEqual(c[key], 0)
        if mode == 'U':
            self.assertEqual(c['cache_hits'], 0)
            self.assertEqual(c['key_comparisons'], 0)
            self.assertEqual(c['expert_comparisons'], 0)
        if block == 1:
            self.assertEqual(c['cache_hits'], 0)

    def test_validation_grid_provenance_and_all_size_equality(self):
        v = self.validation
        self.assertEqual(v['experiment'], 'RNL-003')
        self.assertEqual(v['kind'], 'validate')
        self.fp(v)
        scenarios = v['scenarios']
        self.assertEqual([(s['dimension'], s['block']) for s in scenarios],
                         [(d, b) for d in DIMS for b in BLOCKS])
        for s in scenarios:
            d, b = s['dimension'], s['block']
            self.fingerprint(s, d, b)
            self.assertEqual([t['seed'] for t in s['traces']], ['zero', 'tiled'])
            for t in s['traces']:
                self.assertEqual(t['steps'], 100)
                self.assertTrue(t['equal'])
                self.assertTrue(t['integer_oracle'])
                self.assertTrue(t['all_finite'])
                self.assertRegex(t['u_hash'], HEX)
                self.assertEqual(t['u_hash'], t['m_hash'])
                self.counts(t['u_counts'], 100, 'U', b)
                self.counts(t['m_counts'], 100, 'M', b)

    def test_small_dimension_independent_fp32_and_integer_oracles(self):
        model = generate_model(16)
        fpmodel = oracle_model(model)
        for s in self.validation['scenarios']:
            if s['dimension'] != 16:
                continue
            block = s['block']
            for trace in s['traces']:
                state = [(.25, -.5, .75)[i % 3] if trace['seed'] == 'tiled' else 0.
                         for i in range(16)]
                self.assertEqual(len(trace['u_trace']), 100)
                entry = None
                hits = 0
                hash_value = 14695981039346656037
                previous_actual = [float_bits(x) for x in state]
                for actual, signal in zip(trace['u_trace'], generate_corpus(16, block)):
                    self.assertEqual(actual['input_bits'], [float_bits(x) for x in signal])
                    self.assertEqual(actual['state_before_bits'], previous_actual)
                    selected, scores, output, after = oracle_step(fpmodel, state, signal)
                    self.assertEqual(actual['selected'], selected)
                    self.near(decode_bits(actual['state_before_bits']), state)
                    self.near(decode_bits(actual['scores_bits']), scores)
                    product = int_product(model, selected, signal)
                    self.assertEqual(actual['matrix_product_bits'], [float_bits(x) for x in product])
                    self.near(decode_bits(actual['output_bits']), output)
                    self.near(decode_bits(actual['state_after_bits']), after)
                    key = (selected, tuple(actual['input_bits']))
                    hits += key == entry
                    entry = key
                    for field in ('input_bits', 'state_before_bits', 'scores_bits'):
                        hash_value = fnv_words(actual[field], hash_value)
                    hash_value = fnv_words([selected], hash_value)
                    for field in ('matrix_product_bits', 'output_bits', 'state_after_bits'):
                        hash_value = fnv_words(actual[field], hash_value)
                    state = after
                    previous_actual = actual['state_after_bits']
                self.assertEqual(trace['u_hash'], f'{hash_value:016x}')
                self.assertEqual(trace['m_counts']['cache_hits'], hits)

    def test_timing_protocol_controls_and_complete_paired_results(self):
        if ARGS.validation_only:
            self.skipTest('Validation-only gate before timing workers')
        for d in DIMS:
            for block in BLOCKS:
                r = self.read(f'timing-D{d}-L{block}')
                self.assertEqual(r['kind'], 'timing')
                self.fingerprint(r, d, block)
                self.fp(r)
                clock = r['clock']
                self.assertGreater(clock['frequency_hz'], 0)
                self.assertGreater(clock['empirical_min_ticks'], 0)
                self.assertEqual(clock['sample_reads'], 2000)
                self.assertFalse(clock['nonmonotonic'])
                n = r['event_count']
                cap = min(100000, (200000000//(d*d)//100)*100)
                self.assertEqual(r['cap_event_count'], cap)
                self.assertEqual(n % 100, 0)
                self.assertTrue(100 <= n <= cap)
                cal = r['calibration']
                self.assertTrue(1 <= len(cal) <= 4)
                self.assertEqual(cal[0]['events'], 100)
                for i, batch in enumerate(cal):
                    self.assertEqual(batch['mode'], 'U')
                    self.assertGreater(batch['ticks'], 0)
                    self.counts(batch['counts'], batch['events'], 'U', block)
                    self.assertEqual(batch['repeated_events'], batch['events']*(block-1)//block)
                    if i+1 < len(cal):
                        seconds = batch['ticks']/clock['frequency_hz']
                        self.assertLess(seconds, .050)
                        self.assertLess(batch['events'], cap)
                        nxt = math.ceil(batch['events']*.050/seconds/100)*100
                        nxt = min(cap, max(batch['events']+100, nxt))
                        self.assertEqual(cal[i+1]['events'], nxt)
                self.assertEqual(cal[-1]['events'], n)
                self.assertTrue(cal[-1]['ticks']/clock['frequency_hz'] >= .050 or
                                n == cap or len(cal) == 4)
                self.assertEqual([w['mode'] for w in r['warmup']], ['U', 'M'])
                for field in ('counts', 'checksum_bits', 'state_hash', 'repeated_events'):
                    self.assertEqual(cal[-1][field], r['warmup'][0][field])
                self.assertEqual(len(r['pairs']), 5)
                all_batches = r['warmup'][:]
                for i, pair in enumerate(r['pairs']):
                    self.assertEqual(pair['order'], 'UM' if i % 2 == 0 else 'MU')
                    for mode in ('U', 'M'):
                        batch = pair[mode]
                        self.assertEqual(batch['mode'], mode)
                        all_batches.append(batch)
                    self.assertEqual(pair['U']['checksum_bits'], pair['M']['checksum_bits'])
                    self.assertEqual(pair['U']['state_hash'], pair['M']['state_hash'])
                for batch in all_batches:
                    self.assertEqual(batch['events'], n)
                    self.assertGreater(batch['ticks'], 0)
                    self.assertRegex(batch['checksum_bits'], HEX)
                    self.assertTrue(math.isfinite(struct.unpack('<d',
                        struct.pack('<Q', int(batch['checksum_bits'], 16)))[0]))
                    self.assertRegex(batch['state_hash'], HEX)
                    self.counts(batch['counts'], n, batch['mode'], block)
                    self.assertEqual(batch['repeated_events'], n*(block-1)//block)
                for mode in ('U', 'M'):
                    same = [b for b in all_batches if b['mode'] == mode]
                    for field in ('counts', 'checksum_bits', 'state_hash', 'repeated_events'):
                        self.assertTrue(all(b[field] == same[0][field] for b in same))

    def test_separate_process_memory_and_empty_controls(self):
        if ARGS.validation_only:
            self.skipTest('Validation-only gate before memory workers')
        for d in DIMS:
            for mode in ('U', 'M'):
                r = self.read(f'memory-D{d}-{mode}')
                self.assertEqual(r['kind'], 'memory')
                self.assertEqual(r['mode'], mode)
                self.fingerprint(r, d, 10)
                self.fp(r)
                self.counts(r['counts'], 100, mode, 10)
                self.check_memory(r['memory'])
                allocation = r['allocation']
                self.assertIsNone(allocation['allocator_overhead_bytes'])
                self.assertIsNone(allocation['crt_overhead_bytes'])
                self.assertIsNone(allocation['model_disk_file_bytes'])
                self.assertEqual(allocation['generated_matrix_payload_bytes'], 3*d*d)
                self.assertEqual(allocation['generated_gate_payload_bytes'], 12*d)
                components = (
                    'model_object_bytes', 'corpus_object_bytes', 'vm_object_bytes',
                    'gates_capacity_bytes', 'indices_capacity_bytes',
                    'corpus_vectors_capacity_bytes', 'corpus_float_capacity_bytes',
                    'vm_base_float_capacity_bytes', 'cache_input_capacity_bytes',
                    'cache_product_capacity_bytes')
                for key in components:
                    self.assertIsInstance(allocation[key], int)
                    self.assertGreaterEqual(allocation[key], 0)
                self.assertEqual(allocation['partial_total_bytes'],
                                 sum(allocation[k] for k in components))
                self.assertGreaterEqual(allocation['gates_capacity_bytes'], 12*d)
                self.assertGreaterEqual(allocation['indices_capacity_bytes'], 3*d*d)
                self.assertGreaterEqual(allocation['corpus_float_capacity_bytes'], 400*d)
                self.assertGreaterEqual(allocation['vm_base_float_capacity_bytes'], 12*d)
                for key in ('cache_input_capacity_bytes', 'cache_product_capacity_bytes'):
                    if mode == 'U':
                        self.assertEqual(allocation[key], 0)
                    else:
                        self.assertGreaterEqual(allocation[key], 4*d)
            u, m = [self.read(f'memory-D{d}-{mode}') for mode in ('U', 'M')]
            self.assertEqual(u['state_hash'], m['state_hash'])
            for key in components:
                if not key.startswith('cache_'):
                    self.assertEqual(u['allocation'][key], m['allocation'][key])
            self.assertEqual(m['allocation']['partial_total_bytes']-
                             u['allocation']['partial_total_bytes'],
                             m['allocation']['cache_input_capacity_bytes']+
                             m['allocation']['cache_product_capacity_bytes'])
        for i in (1, 2, 3):
            r = self.read(f'empty-{i}')
            self.assertEqual(r['kind'], 'empty')
            self.fp(r)
            self.check_memory(r['memory'])

    def check_memory(self, m):
        for key in ('working_set_bytes', 'private_bytes', 'peak_working_set_bytes',
                    'pagefile_usage_bytes', 'peak_pagefile_usage_bytes'):
            self.assertIsInstance(m[key], int)
            self.assertTrue(0 < m[key] <= 256*1024*1024)
        self.assertLessEqual(m['working_set_bytes'], m['peak_working_set_bytes'])
        self.assertLessEqual(m['pagefile_usage_bytes'], m['peak_pagefile_usage_bytes'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--results', required=True)
    parser.add_argument('--validation-only', action='store_true')
    parser.add_argument('--write-assessment')
    ARGS = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ScaleChecks)
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    if outcome.wasSuccessful() and ARGS.write_assessment:
        parser.error('Assessment requires full results') if ARGS.validation_only else None
        timings = [ScaleChecks.read(f'timing-D{d}-L{b}') for d in DIMS for b in BLOCKS]
        path = Path(ARGS.write_assessment)
        with path.open('x', encoding='utf-8') as output:
            json.dump({'experiment': 'RNL-003', 'assessments': assess_all(timings)}, output, indent=2)
            output.write('\n')
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
