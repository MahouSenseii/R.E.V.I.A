"""Data-only independent oracle and frozen fixture generator for RNL-002."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import unittest

from check_first001 import f32, float_bits, frozen_corpus, load_original, oracle_step

def from_bits(bits):
    return struct.unpack('<f',struct.pack('<I',bits))[0]

def expected_cases(model):
    original=frozen_corpus(model['threshold'])
    result=[]
    for state in ([0.,0.,0.],[.25,-.5,.75]):
        result += [(name,initial,signals) for name,initial,signals in original if initial==state]
        result.append(('RouteRepeat',state,[[0.,1.,1.]]*16))
        result.append(('SignedZero',state,[[1.,0.,0.]]*2+[[1.,-0.,0.]]*2+[[1.,0.,0.]]*2))
    assert len(result)==20 and sum(len(x[2]) for x in result)==280
    return result

def fixture_bytes(model):
    cases=expected_cases(model)
    out=bytearray(b'RNC2'+struct.pack('<H',len(cases)))
    for name,initial,signals in cases:
        encoded=name.encode('ascii')
        out += bytes([len(encoded)])+encoded+struct.pack('<3fH',*initial,len(signals))
        for signal in signals:
            out += struct.pack('<3f',*signal)
    return bytes(out)

def product(model,expert,signal):
    weights=model['experts'][expert]['weights']
    result=[]
    for row in range(3):
        total=0.
        for col,x in enumerate(signal):
            if x!=0:
                total=f32(total+f32(weights[row*3+col]*x))
        result.append(total)
    return result

class CacheChecks(unittest.TestCase):
    def setUp(self):
        self.model=load_original(ARGS.model)
        self.cases=expected_cases(self.model)
        self.results={}
        for mode in ('U','M','B'):
            path=Path(ARGS.results)/mode/'result.json'
            self.assertTrue(path.is_file(),'Missing RNL-002 worker result: '+mode)
            self.results[mode]=json.loads(path.read_text('utf-8'))
    def near(self,actual,expected):
        self.assertEqual(len(actual),len(expected))
        for a,b in zip(actual,expected):
            self.assertLessEqual(abs(a-b),1e-6)
    def test_frozen_inputs_and_fp_environment(self):
        common_controls=self.results['U']['fp_environment']['mxcsr_control_before']
        for mode,result in self.results.items():
            self.assertEqual(result['mode'],mode)
            self.assertEqual(len(result['traces']),20)
            self.assertEqual(sum(len(c['steps']) for c in result['traces']),280)
            fp=result['fp_environment']
            self.assertTrue(fp['round_to_nearest'])
            self.assertTrue(fp['controls_unchanged'])
            self.assertEqual(fp['mxcsr_control_before'],fp['mxcsr_control_after'])
            self.assertEqual(fp['mxcsr_control_before'],common_controls)
            self.assertEqual(fp['fe_round_start'],fp['FE_TONEAREST'])
            self.assertEqual(fp['fe_round_end'],fp['fe_round_start'])
            self.assertFalse(fp['exception_flags_in_contract'])
            for case,(name,state,signals) in zip(result['traces'],self.cases):
                self.assertEqual(case['name'],name)
                self.assertEqual(case['initial_state'],state)
                self.assertEqual(case['initial_state_bits'],[float_bits(x) for x in state])
                self.assertEqual(len(case['steps']),len(signals))
                for step,signal in zip(case['steps'],signals):
                    self.assertEqual(step['input_bits'],[float_bits(x) for x in signal])
                    for key in ('input','state_before','scores','matrix_product','output','state_after'):
                        self.assertEqual(len(step[key]),len(step[key+'_bits']))
                        for value,bits in zip(step[key],step[key+'_bits']):
                            self.assertEqual(f32(value),from_bits(bits))
    def test_uncached_reference_matches_independent_fp32_oracle(self):
        for case in self.results['U']['traces']:
            state=case['initial_state']
            for step in case['steps']:
                signal=[from_bits(b) for b in step['input_bits']]
                expert,scores,output,after=oracle_step(self.model,state,signal)
                self.near(step['state_before'],state)
                self.assertEqual(step['selected'],[expert])
                self.near(step['scores'],scores)
                self.near(step['matrix_product'],product(self.model,expert,signal))
                self.near(step['output'],output)
                self.near(step['state_after'],after)
                self.assertFalse(step['cache_hit'])
                self.assertEqual(step['matrix_macs'],sum(x!=0 for x in signal)*3)
                state=after
    def test_complete_matrix_key_preserves_all_reference_bits(self):
        for u,m in zip(self.results['U']['traces'],self.results['M']['traces']):
            for a,b in zip(u['steps'],m['steps']):
                self.assertEqual(a['selected'],b['selected'])
                for key in ('input','state_before','scores','matrix_product','output','state_after'):
                    self.assertEqual(a[key+'_bits'],b[key+'_bits'])
    def test_all_modes_follow_their_declared_cache_recurrence(self):
        for mode,result in self.results.items():
            for case in result['traces']:
                state=case['initial_state'];entry=None
                for step in case['steps']:
                    signal=[from_bits(bits) for bits in step['input_bits']]
                    expert,scores,_,_=oracle_step(self.model,state,signal)
                    key=tuple(step['input_bits'])
                    hit=mode!='U' and entry is not None and key==entry[1] and (mode=='B' or expert==entry[0])
                    raw=entry[2] if hit else product(self.model,expert,signal)
                    if not hit and mode!='U':entry=(expert,key,raw)
                    output=[f32(math.tanh(f32(p+f32(f32(.25)*m)))) for p,m in zip(raw,state)]
                    after=[f32(f32(self.model['decay']*m)+f32(f32(1-self.model['decay'])*y)) for m,y in zip(state,output)]
                    self.assertEqual(step['selected'],[expert])
                    self.assertEqual(step['cache_hit'],hit)
                    self.near(step['state_before'],state)
                    self.near(step['scores'],scores)
                    self.near(step['matrix_product'],raw)
                    self.near(step['output'],output)
                    self.near(step['state_after'],after)
                    state=after
    def test_cache_decisions_and_work_counters(self):
        total_hits=0
        for mode in ('U','M','B'):
            totals={k:0 for k in ('matrix_macs','router_products','activations','state_updates','key_comparisons','key_expert_comparisons','cache_lookups','cache_hits','cache_misses','matrix_evaluations')}
            for case in self.results[mode]['traces']:
                entry=None
                for step in case['steps']:
                    selected=step['selected'][0]
                    key=tuple(step['input_bits'])
                    hit=mode!='U' and entry is not None and key==entry[1] and (mode=='B' or selected==entry[0])
                    expert_comparisons=int(mode=='M' and entry is not None)
                    key_comparisons=0
                    if mode!='U' and entry is not None and (mode=='B' or selected==entry[0]):
                        for current,old in zip(key,entry[1]):
                            key_comparisons+=1
                            if current!=old:break
                    self.assertEqual(step['key_expert_comparisons'],expert_comparisons)
                    self.assertEqual(step['key_comparisons'],key_comparisons)
                    self.assertEqual(step['cache_hit'],hit)
                    self.assertEqual(step['cache_lookups'],int(mode!='U'))
                    self.assertEqual(step['cache_hits'],int(hit))
                    self.assertEqual(step['cache_misses'],int(mode!='U' and not hit))
                    self.assertEqual(step['matrix_evaluations'],int(not hit))
                    signal=[from_bits(b) for b in step['input_bits']]
                    self.assertEqual(step['matrix_macs'],0 if hit else sum(x!=0 for x in signal)*3)
                    self.assertEqual(step['router_products'],27)
                    self.assertEqual(step['activations'],3)
                    self.assertEqual(step['state_updates'],3)
                    if mode=='U':
                        self.assertEqual(step['key_comparisons'],0)
                        self.assertEqual(step['key_expert_comparisons'],0)
                    if not hit and mode!='U':
                        entry=(selected,key,tuple(step['matrix_product_bits']))
                    elif hit:
                        self.assertEqual(tuple(step['matrix_product_bits']),entry[2])
                    if mode=='M':total_hits+=hit
                    for key in totals:totals[key]+=step[key]
            self.assertEqual(self.results[mode]['aggregate_counts'],totals)
            capacity=self.results[mode]['cache_capacity']
            self.assertEqual(capacity['logical_enabled'],mode!='U')
            if mode=='U':self.assertEqual(capacity['logical_capacity_bytes'],0)
            else:
                self.assertEqual(capacity['input_key_bits_bytes'],12)
                self.assertEqual(capacity['raw_product_bytes'],12)
                self.assertEqual(capacity['expert_index_bytes'],4)
                self.assertEqual(capacity['validity_bytes'],1)
                self.assertEqual(capacity['logical_capacity_bytes'],29+capacity['padding_bytes'])
            self.assertIsNone(capacity['process_memory_bytes'])
        self.assertGreater(total_hits,0,'M did not exercise actual reuse')
    def test_route_transition_negative_control_is_rejected(self):
        select=lambda mode:next(c for c in self.results[mode]['traces'] if c['name']=='RouteRepeat' and c['initial_state']==[0,0,0])
        u,m,b=[select(mode)['steps'] for mode in ('U','M','B')]
        self.assertEqual(u[0]['selected'],[1])
        self.assertEqual(u[1]['selected'],[2])
        self.assertFalse(m[0]['cache_hit'])
        self.assertFalse(m[1]['cache_hit'])
        self.assertTrue(b[1]['cache_hit'])
        self.assertEqual(u[0]['matrix_product'],[1,1,2])
        self.assertEqual(u[1]['matrix_product'],[1,1,1])
        self.assertEqual(b[1]['matrix_product'],[1,1,2])
        self.assertNotEqual(u[1]['state_after_bits'],b[1]['state_after_bits'])
        self.assertEqual(u[1]['state_after_bits'],m[1]['state_after_bits'])
    def test_signed_zero_uses_bits_and_always_updates_state(self):
        for case in self.results['M']['traces']:
            if case['name']!='SignedZero':continue
            self.assertEqual([s['selected'] for s in case['steps']],[[0]]*6)
            self.assertEqual([s['cache_hit'] for s in case['steps']],[False,True,False,True,False,True])
            self.assertEqual([s['input_bits'][1] for s in case['steps']],[0,0,0x80000000,0x80000000,0,0])
            self.assertTrue(all(s['state_updates']==3 for s in case['steps']))

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--model',required=True)
    parser.add_argument('--results')
    parser.add_argument('--write-fixtures')
    ARGS,rest=parser.parse_known_args()
    if ARGS.write_fixtures:
        data=fixture_bytes(load_original(ARGS.model))
        path=Path(ARGS.write_fixtures);path.parent.mkdir(parents=True,exist_ok=True)
        with path.open('xb') as f:f.write(data)
        print(json.dumps({'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'cases':20,'steps':280}))
    else:
        parser.error('--results required') if not ARGS.results else None
        unittest.main(argv=['check_cache002.py',*rest])
