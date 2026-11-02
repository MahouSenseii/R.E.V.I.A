"""Independent stdlib oracle for reviewed FIRST-001 worker result directories.

Does not execute the uploaded prototype or worker. Read only pinned model bytes
and each F/U/P/C result.json plus its emitted model.rnb.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import unittest

def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]

def load_original(path):
    data = Path(path).read_bytes()
    offset = 0
    def take(fmt):
        nonlocal offset
        value = struct.unpack_from('<' + fmt, data, offset)
        offset += struct.calcsize('<' + fmt)
        return value[0] if len(value) == 1 else value
    def name():
        nonlocal offset
        size = take('B')
        value = data[offset:offset + size].decode('ascii')
        offset += size
        return value
    assert data[:4] == b'RNL1'
    offset = 4
    brain_name = name()
    inputs, states = take('HH')
    decay, threshold = take('ff')
    topk, codes, count = take('BBB')
    book = [take('f') for _ in range(codes)]
    experts = []
    for _ in range(count):
        expert_name = name()
        gates = [take('f') for _ in range(inputs)]
        packed = [take('B') for _ in range((inputs * states + 3) // 4)]
        indices = [(packed[i // 4] >> (2 * (i % 4))) & 3 for i in range(inputs * states)]
        experts.append({'name': expert_name, 'gates': gates, 'indices':indices, 'weights': [book[i] for i in indices]})
    assert offset == len(data)
    assert (inputs, states, topk, codes, count) == (3, 3, 1, 3, 3)
    return {'name':brain_name, 'inputs':inputs, 'states':states, 'decay':decay, 'threshold':threshold, 'book':book, 'experts':experts, 'bytes':data}

def float_bits(value):
    return struct.unpack('<I',struct.pack('<f',value))[0]

def frozen_corpus(threshold):
    z,u,n = [0.,0.,0.],[1.,0.,0.],[-1.,0.,0.]
    cases = [('Z',[z]*16),('R',[u]*16),('A',[u if i%2==0 else n for i in range(16)]+[u]),('B',[z,u,z]*4)]
    threshold_bits = float_bits(threshold)
    for name, offset in [('N-below',-1),('N-at',0),('N-above',1)]:
        delta=struct.unpack('<f',struct.pack('<I',threshold_bits+offset))[0]
        cases.append((name,[u,[1.,delta,0.]]*8))
    cases.append(('Router',[z,u,[0.,1.,0.],[0.,0.,1.],[1.,1.,0.],[0.,1.,1.],[1.,0.,1.],[-1.,-1.,-1.],[.5,.5,.5]]))
    return [(name,state,signals) for state in ([0.,0.,0.],[.25,-.5,.75]) for name,signals in cases]

def expected_container(model, variant):
    def name(value):
        return bytes([len(value)])+value.encode('ascii')
    data = bytearray(b'RNLF' if variant=='F' else b'RNL1' if variant=='P' else b'RNLU')
    data += name(model['name'])
    data += struct.pack('<HHffBBB',3,3,model['decay'],model['threshold'],1,0 if variant=='F' else 3,3)
    if variant!='F':
        data += struct.pack('<3f',*model['book'])
    for expert in model['experts']:
        data += name(expert['name'])+struct.pack('<3f',*expert['gates'])
        if variant=='F':
            data += struct.pack('<9f',*expert['weights'])
        elif variant=='P':
            packed=bytearray(3)
            for i,index in enumerate(expert['indices']):
                packed[i//4] |= index << (2*(i%4))
            data += packed
        else:
            data += bytes(expert['indices'])
    return bytes(data)

def oracle_step(model, state, signal):
    scores = []
    for e in model['experts']:
        score = 0.0
        for x, g in zip(signal, e['gates']):
            score = f32(score + f32(x * g))
        for m, g in zip(state, e['gates']):
            score = f32(score + f32(f32(f32(.15) * m) * g))
        scores.append(score)
    selected = max(range(len(scores)), key=lambda i: (scores[i], -i))
    weights = model['experts'][selected]['weights']
    output, after = [], []
    for row in range(model['states']):
        total = 0.0
        for col, x in enumerate(signal):
            if x != 0.0:
                total = f32(total + f32(weights[row * model['inputs'] + col] * x))
        y = f32(math.tanh(f32(total + f32(f32(.25) * state[row]))))
        output.append(y)
        after.append(f32(f32(model['decay'] * state[row]) + f32(f32(1.0 - model['decay']) * y)))
    return selected, scores, output, after

class ResultChecks(unittest.TestCase):
    def setUp(self):
        self.model = load_original(ARGS.model)
        self.results = {}
        for variant in ('F', 'U', 'P', 'C'):
            path = Path(ARGS.results) / variant / 'result.json'
            self.assertTrue(path.is_file(), 'Missing worker result: ' + variant)
            self.results[variant] = json.loads(path.read_text('utf-8'))
    def close_vector(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            self.assertTrue(math.isfinite(a) and math.isfinite(b))
            self.assertLessEqual(abs(a - b), 1e-6)
    def test_complete_trajectories_match_independent_f32_oracle(self):
        for variant in ('F','U','P'):
            traces = self.results[variant]['traces']
            self.assertGreaterEqual(len(traces), 12)
            for case in traces:
                state = [f32(x) for x in case['initial_state']]
                self.assertGreaterEqual(len(case['steps']), 2)
                for step in case['steps']:
                    signal = [f32(x) for x in step['input']]
                    self.close_vector(step['state_before'], state)
                    expert, scores, output, after = oracle_step(self.model, state, signal)
                    self.assertEqual(step['selected'], [expert])
                    self.assertFalse(step['cached'])
                    self.assertEqual(step['matrix_macs'], sum(x != 0 for x in signal) * 3)
                    self.assertEqual(step['router_products'],27)
                    self.assertEqual(step['activations'],3)
                    self.assertEqual(step['state_updates'],3)
                    self.close_vector(step['scores'], scores)
                    self.close_vector(step['output'], output)
                    self.close_vector(step['state_after'], after)
                    state = after
    def test_pairwise_variants_share_inputs_and_full_numeric_trajectories(self):
        f = self.results['F']['traces']
        for variant in ('U','P','C'):
            traces = self.results[variant]['traces']
            self.assertEqual([x['name'] for x in traces], [x['name'] for x in f])
            for a, b in zip(f, traces):
                self.assertEqual(a['initial_state'], b['initial_state'])
                self.assertEqual([s['input'] for s in a['steps']], [s['input'] for s in b['steps']])
                if variant == 'C':
                    continue
                for sa, sb in zip(a['steps'], b['steps']):
                    self.assertEqual(sa['selected'], sb['selected'])
                    for key in ('state_before','output','state_after'):
                        self.close_vector(sa[key], sb[key])
    def test_zero_and_nonzero_initial_state_coverage(self):
        traces = self.results['F']['traces']
        zero = [x for x in traces if all(v == 0 for v in x['initial_state'])]
        nonzero = [x for x in traces if any(v != 0 for v in x['initial_state'])]
        self.assertGreaterEqual(len(zero), 6)
        self.assertEqual(len(zero), len(nonzero))
    def test_cache_reproduces_authored_policy_and_exposes_recurrent_divergence(self):
        divergent = 0
        for case, ref in zip(self.results['C']['traces'], self.results['U']['traces']):
            state = [f32(x) for x in case['initial_state']]
            latest = [0.0] * 3
            previous = None
            for step, reference in zip(case['steps'], ref['steps']):
                signal = [f32(x) for x in step['input']]
                delta = None if previous is None else max(abs(f32(a-b)) for a,b in zip(signal, previous))
                cached = previous is not None and delta <= self.model['threshold']
                self.assertEqual(step['cached'], cached)
                self.close_vector(step['state_before'], state)
                if cached:
                    self.assertEqual(step['selected'], [])
                    self.assertEqual(step['scores'], [])
                    self.assertEqual(step['matrix_macs'], 0)
                    self.assertEqual(step['router_products'],0)
                    self.assertEqual(step['activations'],0)
                    self.assertEqual(step['state_updates'],0)
                else:
                    expert, scores, latest, state = oracle_step(self.model, state, signal)
                    self.assertEqual(step['selected'], [expert])
                    self.close_vector(step['scores'], scores)
                    self.assertEqual(step['matrix_macs'],sum(x != 0 for x in signal)*3)
                    self.assertEqual(step['router_products'],27)
                    self.assertEqual(step['activations'],3)
                    self.assertEqual(step['state_updates'],3)
                    previous = signal
                self.assertLessEqual(abs(step['delta']-(0 if delta is None else delta)),1e-7)
                self.close_vector(step['state_after'], state)
                self.close_vector(step['output'], latest)
                if any(abs(a-b) > 1e-6 for a,b in zip(step['state_after'], reference['state_after'])):
                    divergent += 1
        self.assertGreater(divergent, 0, 'Negative controls failed to expose cache semantic change')
    def test_complete_files_and_original_packed_bytes(self):
        expected = {'F':200,'U':131,'P':113,'C':131}
        for variant, size in expected.items():
            data = (Path(ARGS.results) / variant / 'model.rnb').read_bytes()
            self.assertEqual(len(data), size)
            self.assertEqual(data,expected_container(self.model,variant))
            if variant == 'P':
                self.assertEqual(data, self.model['bytes'])
    def test_frozen_corpus_bits_identity_and_accounting(self):
        expected = frozen_corpus(self.model['threshold'])
        for variant,result in self.results.items():
            self.assertEqual(result['variant'],variant)
            self.assertEqual(result['source_sha256'],'d5ee18bbe9df39eb9d99b571c93ae1f0bbeec97bf19b387587a71d4563d61b0f')
            self.assertEqual(result['threshold_bits'],float_bits(self.model['threshold']))
            self.assertEqual(float_bits(result['threshold']),float_bits(self.model['threshold']))
            self.assertEqual(result['decoded_weights'],[e['weights'] for e in self.model['experts']])
            self.assertEqual(len(result['traces']),16)
            self.assertEqual(result['trace_steps'],236)
            for case,(name,initial,signals) in zip(result['traces'],expected):
                self.assertEqual(case['name'],name)
                self.assertEqual(case['initial_state_bits'],[float_bits(v) for v in initial])
                self.assertEqual(case['initial_state'],initial)
                self.assertEqual(len(case['steps']),len(signals))
                for step,signal in zip(case['steps'],signals):
                    self.assertEqual(step['input_bits'],[float_bits(v) for v in signal])
                    for key in ('input','state_before','scores','output','state_after'):
                        self.assertEqual(len(step[key]),len(step[key+'_bits']))
                        for number,bit_pattern in zip(step[key],step[key+'_bits']):
                            from_bits=struct.unpack('<f',struct.pack('<I',bit_pattern))[0]
                            self.assertEqual(f32(number),from_bits)
            storage=result['storage']
            matrix={'F':108,'U':27,'P':9,'C':27}[variant]
            book=0 if variant=='F' else 12
            self.assertEqual(storage['matrix_payload_bytes'],matrix)
            self.assertEqual(storage['codebook_bytes'],book)
            self.assertEqual(storage['gates_bytes'],36)
            self.assertEqual(storage['headers_names_bytes'],56)
            self.assertEqual(storage['serialized_bytes'],matrix+book+36+56)
            self.assertEqual(storage['padding_bits'],18 if variant=='P' else 0)
            self.assertGreaterEqual(storage['runtime_matrix_capacity_bytes'],matrix)
            self.assertGreaterEqual(storage['runtime_codebook_capacity_bytes'],book)
            self.assertGreaterEqual(storage['runtime_gate_capacity_bytes'],36)
            self.assertGreater(storage['runtime_object_capacity_bytes'],0)
            self.assertEqual(storage['resident_experts'],3)
            self.assertTrue(storage['capacity_sums_are_not_process_memory'])
            self.assertIsNone(storage['runtime_allocator_overhead_bytes'])
            self.assertIsNone(result['correctness_decision'])
            self.assertIsNone(result['performance_claim'])

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--model',required=True)
    parser.add_argument('--results',required=True)
    ARGS, rest = parser.parse_known_args()
    unittest.main(argv=['check_first001.py', *rest])
