"""PREFIX-004 preparation/owned CPU experiment; default is offline validation.

--execute is an operator opt-in, not approval. Do not use it until the exact
protocol and file hashes have been reviewed by the parent. No app attachment,
settings changes, GPU sampling, downloads, or publication exist in this driver.
"""
import argparse
from contextlib import contextmanager
import ctypes as C
from ctypes import wintypes as W
import hashlib
import http.client
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
import uuid

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TASK = REPO.parent
BACKEND_PATH = REPO/'Tools/Quality/contextBenchmark.py'
GUARD_PATH = HERE/'run_bounded.py'
BACKEND_SHA = '914b87e3fbb2e9fdaec751d7ebbb996b6903bf1db35861c9b11fc4aa655bb52c'
GUARD_SHA = '31edb4ff8a997363d07ea8823a2efb7bdde5ed2566872a9e6adbda20b5b62bb7'
MODEL_SHA = 'bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517'
MODEL_BYTES = 532517120
RUNTIME_HASHES = {
    'llama-server.exe': 'c932a2ac50dbdc5768399723e2e6e6295f7abcdf5243220729022c38ae2c415a',
    'llama-server-impl.dll': '167a1a8890390fb47b5056123879c0c1116c39e62a92398c77dbaeb1d3bf806d',
    'llama-common.dll': 'e616788eafbfad9c15a8c3adfab8107f232457ef0b5c70f963ee827dfaca2aee',
    'llama.dll': 'a96f1296b0def822306ca80e8d1ee71bf450d609d2b5fde2733a7d8ab5549cab',
    'ggml.dll': '1defc07c048a19d5edc8243f79eb120b1864dd6ae4cd816577dee2e47db1aca5',
    'ggml-base.dll': '842c3084ed1817c3a0bbc0da396c0b46e18d0b3f2a87171104a6c12029524988',
    'ggml-cpu-x64.dll': '1c923a0d3b6a85b075583442c027871c8e739f910f088bff4b41ac0c01136165',
    'mtmd.dll': '958c1df28e095d837231afd0c53644761accea484011e66db0b60c780f95b0b6',
    'libomp140.x86_64.dll': '4a20c1e5c115c29771a12324513eb109badac72180f79481527ad79d996ffb33',
}
MEMORY_CAP = 2*1024**3
ARTIFACT_CAP = 10*1024**2
STAGE_CAP = 650*1024**2
LOG_CAP = SSE_CAP = 65536
LINE_CAP = 16384
ATTEMPT_SECONDS = 1200
HTTP_SECONDS = 30
CONTEXT = 2048
ALIAS = 'rnl-prefix004-0.8b'
COUNT_FIELDS = ('cache_n', 'prompt_n', 'predicted_n')
TIME_FIELDS = ('prompt_ms', 'predicted_ms')


def require(value, why):
    if not value:
        raise RuntimeError(why)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(8*1024**2), b''):
            digest.update(block)
    return digest.hexdigest()


def read_json(path, cap=65536):
    data = Path(path).read_bytes()
    require(len(data) <= cap, 'JSON input exceeds cap')
    return json.loads(data, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite JSON')))


def load_module(path, name, expected):
    require(sha(path) == expected, 'Reviewed helper fingerprint changed: '+path.name)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


backend = load_module(BACKEND_PATH, 'prefix004_backend', BACKEND_SHA)
guard = load_module(GUARD_PATH, 'prefix004_guard', GUARD_SHA)


def validate_fixtures(path):
    fixture = read_json(path)
    require(fixture['experiment'] == 'PREFIX-004', 'Wrong fixture identity')
    require(set(fixture['variants']) == {'seed', 'shared', 'changed'}, 'Need three frozen variants')
    base = backend.messages_for(backend.PROBES[0], 2)
    variants = fixture['variants']
    for messages in variants.values():
        require(len(messages) == len(base), 'Frozen conversation length changed')
        require(all(set(m) == {'role', 'content'} and isinstance(m['content'], str) for m in messages),
                'Fixture message shape changed')
        require(messages[1:-1] == base[1:-1], 'Public ORBIT history changed')
        require(messages[0]['role'] == 'system' and messages[-1]['role'] == 'user', 'System/question roles changed')
    require(variants['seed'][0] == variants['shared'][0], 'Shared system prefix differs')
    require(variants['shared'][-1] == variants['changed'][-1], 'Measured question differs')
    require(variants['seed'][-1] != variants['shared'][-1], 'Primer question must differ')
    a, b = variants['shared'][0]['content'], variants['changed'][0]['content']
    require(len(a.encode()) == len(b.encode()) and len(a) == len(b), 'System byte/character counts differ')
    pa, pb = a.split('\n\n'), b.split('\n\n')
    require(len(pa) == 3 and pb == [pa[1], pa[0], pa[2]], 'Only first two labelled rules may swap')
    require(pa[2] == backend.SYSTEM and pa[0].startswith('Rule A:') and pa[1].startswith('Rule B:'),
            'Frozen system rules changed')
    require(fixture['expected'] == {'budget_eur': 7200, 'source': 'D2'}, 'Expected public answer changed')
    require(fixture['sampling'] == {'temperature': 0, 'seed': 42, 'max_tokens': 32}, 'Sampling changed')
    return fixture


def request_body(alias, fixture, variant, cache, *, primer=False):
    probe = {'id': 'prefix004_orbit', 'field_types': {'budget_eur': 'integer', 'source': 'string'},
             'source_field': 'source', 'source_ids': ['D1', 'D2']}
    request = backend.completion_request(alias, probe, fixture['variants'][variant])
    request.update(fixture['sampling'])
    request.update(stream=True, stream_options={'include_usage': True}, cache_prompt=bool(cache))
    if primer:
        request['max_tokens'] = 1
    return request


def schedule():
    rows = []
    for repeat in range(1, 6):
        cells = [('shared', False), ('shared', True), ('changed', True), ('changed', False)] if repeat % 2 else [
            ('changed', False), ('changed', True), ('shared', True), ('shared', False)]
        rows += [{'repeat': repeat, 'variant': variant, 'cache_prompt': cache} for variant, cache in cells]
    return rows


def metric_values(timings):
    def number(name):
        value = timings.get(name)
        return value if type(value) in (int, float) and math.isfinite(value) and value >= 0 else None
    return {name: number(name) for name in COUNT_FIELDS+TIME_FIELDS}


def sanitize(value, secret):
    if isinstance(value, str):
        return value.replace(secret, '[REDACTED]') if secret else value
    if isinstance(value, dict):
        return {sanitize(key, secret): sanitize(item, secret) for key, item in value.items()}
    if isinstance(value, list):
        return [sanitize(item, secret) for item in value]
    return value


class ResponseFailure(RuntimeError):
    def __init__(self, reason, partial):
        super().__init__(reason)
        self.partial = partial


class PreflightFailure(ResponseFailure):
    pass


def response_record(content='', timing=None, usage=None, finish=None, first=None, elapsed=None,
                    size=0, done=False, *, complete=False, status='incomplete', secret='', reason=None):
    timing, usage = timing or {}, usage or {}
    withheld = bool(secret and secret in content)
    metrics = metric_values(timing)
    reasons = {name: 'Missing or invalid backend '+name for name, value in metrics.items() if value is None}
    for name in ('prompt_tokens', 'completion_tokens'):
        if type(usage.get(name)) is not int or usage[name] < 0:
            reasons['usage.'+name] = 'Missing or invalid reported token usage'
    if first is None:
        reasons['ttft_ms'] = 'No nonempty content delta observed'
    if elapsed is None:
        reasons['request_ms'] = 'Measurement clock unavailable'
    if withheld:
        reasons['response_content_sha256'] = 'Credential echo withheld; no content digest retained'
    result = {'content': '' if withheld else content, 'content_withheld': withheld,
              'response_content_sha256': None if withheld else hashlib.sha256(content.encode('utf-8')).hexdigest(),
              'response_checksum_scope': 'complete-content' if complete else 'collected-content-prefix',
              'finish_reason': finish, 'usage': usage, 'reported_timings': timing, 'metrics': metrics,
              'ttft_ms': None if first is None else first*1000,
              'request_ms': None if elapsed is None else elapsed*1000, 'unknown_reasons': reasons,
              'sse_bytes': size, 'done': done, 'complete': complete, 'response_status': status,
              'http_status': None, 'failure_reason': reason,
              'required_metrics_supported': all(type(metrics[name]) is int for name in COUNT_FIELDS)}
    return sanitize(result, secret)


def collect_sse(readline, started, *, secret='', clock=time.perf_counter):
    """Bounded parser carrying a sanitized partial record on every refusal."""
    size, pieces, timing, usage, finish, first, done = 0, [], {}, {}, None, None, False

    def snapshot(complete=False, reason=None):
        try:
            elapsed = clock()-started
            elapsed = elapsed if math.isfinite(elapsed) and elapsed >= 0 else None
        except Exception:
            elapsed = None
        return response_record(''.join(pieces), timing, usage, finish, first, elapsed, size, done,
                               complete=complete, status='complete' if complete else 'incomplete',
                               secret=secret, reason=reason)
    try:
        while True:
            line = readline(LINE_CAP+1)
            if not line:
                break
            size += len(line)
            require(len(line) <= LINE_CAP and size <= SSE_CAP, 'SSE byte/line cap exceeded')
            require(line.endswith(b'\n'), 'Incomplete SSE line')
            line = line.rstrip(b'\r\n')
            if not line.startswith(b'data:'):
                continue
            data = line[5:].lstrip(b' ')
            if data == b'[DONE]':
                done = True
                break
            event = json.loads(data, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite SSE JSON')))
            require(isinstance(event, dict), 'SSE event must be an object')
            require(not secret or secret not in json.dumps(event, ensure_ascii=False), 'Credential echo withheld')
            if isinstance(event.get('timings'), dict):
                timing = event['timings']
            if isinstance(event.get('usage'), dict):
                usage = event['usage']
            choices = event.get('choices', [])
            require(isinstance(choices, list) and len(choices) <= 1, 'Unexpected choice count')
            if not choices:
                continue
            choice = choices[0]
            require(isinstance(choice, dict), 'Invalid choice')
            reason = choice.get('finish_reason')
            if reason is not None:
                require(isinstance(reason, str) and reason, 'Invalid finish reason')
                finish = reason
            delta = choice.get('delta', {})
            require(isinstance(delta, dict), 'Invalid delta')
            require(not delta.get('tool_calls') and not delta.get('reasoning_content'), 'Unexpected tools or thinking')
            content = delta.get('content')
            require(content is None or isinstance(content, str), 'Invalid content delta')
            if content:
                if first is None:
                    first = clock()-started
                    require(math.isfinite(first) and first >= 0, 'Invalid first-content clock')
                pieces.append(content)
                # A credential can straddle independently valid JSON deltas.
                require(not secret or secret not in ''.join(pieces), 'Credential echo withheld')
        require(done and finish is not None, 'Incomplete SSE termination')
        result = snapshot(True)
        require(result['request_ms'] is not None and (first is None or first*1000 <= result['request_ms']), 'Invalid measurement clock')
        return result
    except BaseException as error:
        reason = sanitize(type(error).__name__+': '+str(error), secret)
        raise ResponseFailure(reason, snapshot(reason=reason)) from None


def lcp(a, b):
    return next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))


def validate_token_layout(result):
    for option in ('false', 'true'):
        vectors = {key: item['options'][option]['tokens'] for key, item in result.items()}
        n = len(vectors['shared'])
        require(256 <= n <= 1024 and len(vectors['changed']) == n, 'Frozen probe token lengths differ or are outside 256..1024')
        require(lcp(vectors['seed'], vectors['shared']) >= .75*n, 'Shared prefix shorter than frozen rule')
        require(lcp(vectors['seed'], vectors['changed']) <= .25*n, 'Changed prefix longer than frozen rule')


def clock_info():
    information = time.get_clock_info('perf_counter')
    previous, minimum = time.perf_counter(), math.inf
    for _ in range(1999):
        current = time.perf_counter()
        require(current >= previous, 'Nonmonotonic performance counter')
        if current > previous:
            minimum = min(minimum, current-previous)
        previous = current
    require(information.monotonic and information.resolution > 0 and math.isfinite(minimum) and minimum > 0,
            'Client timer resolution unavailable')
    return {'name': 'perf_counter', 'resolution_seconds': information.resolution,
            'empirical_min_positive_gap_seconds': minimum, 'sample_reads': 2000,
            'monotonic': True, 'timer_is_not_CPU_frequency': True}


def assess_results(runs, timer):
    """Pure fixed-rule assessment; synthetic controls need no live model."""
    gaps = [timer.get(name) for name in ('resolution_seconds', 'empirical_min_positive_gap_seconds')]
    gaps_valid = all(type(x) in (int, float) and math.isfinite(x) and x > 0 for x in gaps)
    quantum = max(gaps) if gaps_valid else None
    timer_valid = gaps_valid and timer.get('monotonic') is True and timer.get('sample_reads') == 2000

    def valid(value):
        return type(value) in (int, float) and math.isfinite(value) and value >= 0

    def summary(values):
        known = [value for value in values if valid(value)]
        return {'unit': 'ms', 'min': min(known) if known else None,
                'median': statistics.median(known) if known else None, 'max': max(known) if known else None,
                'known_samples': len(known), 'expected_samples': 5,
                'unknown_reason': None if len(known) == 5 else 'Missing or invalid duration in one or more fixed pairs'}

    scenarios = {}
    for variant in ('shared', 'changed'):
        pairs, u_times, m_times, u_prefill, m_prefill, ratios, prefill_ratios = [], [], [], [], [], [], []
        complete = True
        for repeat in range(1, 6):
            selected = [row for row in runs if row.get('repeat') == repeat and row.get('variant') == variant]
            if len(selected) != 2 or {row.get('cache_prompt') for row in selected} != {False, True}:
                complete = False
                continue
            arms = {row['cache_prompt']: row for row in selected}
            u, m = [arms[key].get('target', {}) for key in (False, True)]
            uc, mc = u.get('metrics', {}), m.get('metrics', {})
            coherent = all(type(t.get('usage', {}).get('prompt_tokens')) is int and
                           all(type(t.get('metrics', {}).get(name)) is int and t['metrics'][name] >= 0 for name in COUNT_FIELDS) and
                           t['metrics']['cache_n']+t['metrics']['prompt_n'] == t['usage']['prompt_tokens'] and
                           t['usage'].get('completion_tokens') == t['metrics']['predicted_n'] and
                           0 <= t['metrics']['predicted_n'] <= 32 for t in (u, m))
            same_prompt = coherent and u['usage']['prompt_tokens'] == m['usage']['prompt_tokens']
            correct = all(arms[key].get('grade', {}).get('passed') is True and arms[key].get('target', {}).get('complete') is True
                          for key in (False, True))
            mechanism = same_prompt and uc['cache_n'] == 0 and mc['cache_n'] > uc['cache_n'] and mc['prompt_n'] < uc['prompt_n']
            no_additional_reuse = same_prompt and mc['cache_n'] <= uc['cache_n'] and mc['prompt_n'] >= uc['prompt_n']
            ut, mt = u.get('ttft_ms'), m.get('ttft_ms')
            up, mp = uc.get('prompt_ms'), mc.get('prompt_ms')
            ratio = ut/mt if valid(ut) and valid(mt) and mt > 0 else None
            prefill_ratio = up/mp if valid(up) and valid(mp) and mp > 0 else None
            equal_decode = coherent and uc['predicted_n'] == mc['predicted_n']
            pairs.append({'repeat': repeat, 'prompt_and_backend_counts_consistent': same_prompt,
                          'prompt_tokens': u.get('usage', {}).get('prompt_tokens') if same_prompt else None,
                          'correct_complete_answers': correct, 'additional_reuse': bool(mechanism),
                          'no_additional_reuse': bool(no_additional_reuse), 'U_over_M_TTFT': ratio,
                          'U_over_M_prefill': prefill_ratio,
                          'prefill_ratio_unknown_reason': None if prefill_ratio is not None else 'Missing/nonpositive optional prefill timing',
                          'total_wall_comparable': equal_decode and correct,
                          'total_wall_unknown_reason': None if equal_decode and correct else 'Generated token counts or answer/finish contract differ'})
            u_times.append(ut); m_times.append(mt); u_prefill.append(up); m_prefill.append(mp)
            ratios.append(ratio); prefill_ratios.append(prefill_ratio)
        complete = complete and len(pairs) == 5
        fixed_prompt_count = complete and len({p['prompt_tokens'] for p in pairs}) == 1 and pairs[0]['prompt_tokens'] is not None
        timing_adequate = (complete and timer_valid and all(valid(x) and x/1000 >= 10000*quantum for x in u_times+m_times))
        ratios_known = complete and all(valid(x) for x in ratios)
        median_ratio = statistics.median(ratios) if ratios_known else None
        latency_rule = bool(complete and fixed_prompt_count and all(p['correct_complete_answers'] and p['prompt_and_backend_counts_consistent'] for p in pairs)
                            and timing_adequate and median_ratio is not None and median_ratio >= 1.10 and min(ratios) > 1.05
                            and max(m_times) < min(u_times))
        scenarios[variant] = {'pairs': pairs, 'U_TTFT': summary(u_times), 'M_TTFT': summary(m_times),
                              'U_prefill': summary(u_prefill), 'M_prefill': summary(m_prefill),
                              'five_paired_TTFT_ratios': ratios, 'five_paired_prefill_ratios': prefill_ratios,
                              'median_paired_TTFT_U_over_M': median_ratio,
                              'complete_five_pairs': complete, 'fixed_prompt_count_across_pairs': bool(fixed_prompt_count),
                              'timing_resolution_adequate': bool(timing_adequate),
                              'latency_rule_passes': latency_rule,
                              'additional_reuse_every_pair': bool(complete and all(p['additional_reuse'] for p in pairs)),
                              'no_additional_reuse_every_pair': bool(complete and all(p['no_additional_reuse'] for p in pairs))}
    confound = scenarios['changed']['latency_rule_passes'] and scenarios['changed']['no_additional_reuse_every_pair']
    supported = scenarios['shared']['latency_rule_passes'] and scenarios['shared']['additional_reuse_every_pair'] and not confound
    reasons = []
    if not scenarios['shared']['latency_rule_passes']:
        reasons.append('Shared-case latency gates are incomplete or failed')
    if not scenarios['shared']['additional_reuse_every_pair']:
        reasons.append('Additional shared-prefix reuse not demonstrated in every pair')
    if confound:
        reasons.append('Changed control gained latency without additional reuse; causal attribution withheld')
    return {'scenarios': scenarios, 'changed_no_reuse_latency_confound': bool(confound),
            'shared_causal_latency_gates_pass': bool(supported), 'unsupported_reasons': reasons,
            'performance_claim': None, 'claim_status': 'gates_pass_pending_independent_review' if supported else 'unsupported',
            'descriptive_fixed_pairs_not_population_significance': True}


class Budget:
    def __init__(self, directory):
        self.directory = directory
        self.used = 0
        self.lock = threading.Lock()

    def reserve(self, size, *, final=False):
        with self.lock:
            limit = ARTIFACT_CAP if final else ARTIFACT_CAP-1024**2
            require(self.used+size < limit, 'Aggregate artifact budget exceeded')
            self.used += size

    def json(self, name, value, *, final=False):
        require(Path(name).name == name, 'Result name must be a plain filename')
        self.reserve(len(json.dumps(value, indent=2, ensure_ascii=False).encode('utf-8'))+1, final=final)
        backend.write_new_json(self.directory/name, value)

    def replace_final(self, value):
        """Only our canonical attempt receipt may be rewritten after finalization."""
        destination = self.directory/'attempt.json'
        payload = (json.dumps(value, indent=2, ensure_ascii=False)+'\n').encode('utf-8')
        temporary = self.directory/('.attempt-final-'+uuid.uuid4().hex+'.tmp')
        with self.lock:
            old_size = destination.stat().st_size
            require(self.used+len(payload) < ARTIFACT_CAP, 'Final receipt replacement exceeds reserved budget')
            with temporary.open('xb') as output:
                output.write(payload)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, destination)
            self.used += len(payload)-old_size


def finalize_report(budget, report, began, *, clock=time.monotonic):
    report['elapsed_seconds'] = clock()-began
    report['completion_requires_controller_zero_exit'] = True
    if report['elapsed_seconds'] > ATTEMPT_SECONDS:
        report.update(status='stopped', reason='Aggregate deadline exceeded, including cleanup')
    budget.json('attempt.json', report, final=True)
    elapsed_after_write = clock()-began
    if elapsed_after_write > ATTEMPT_SECONDS:
        report.update(status='stopped', reason='Aggregate deadline exceeded during finalization', elapsed_seconds=elapsed_after_write)
        budget.replace_final(report)


class OwnedServer:
    """Small experiment controller reusing the unchanged guard's Win32 ABI.

    Creates only its own suspended process, verifies limits before resume, then
    monitors memory/logs/deadline separately from the HTTP caller. No PID attach.
    """
    def __init__(self, argv, cwd, temp, budget, label, secret, deadline):
        self.argv, self.cwd, self.temp = argv, cwd, temp
        self.budget, self.label, self.secret, self.deadline = budget, label, secret.encode('ascii'), deadline
        self.handles = []
        self.info = guard.ProcessInfo()
        self.job = None
        self.assigned = False
        self.fault = None
        self.closed = threading.Event()
        self.lock = threading.Lock()
        self.monitor = None
        self.log = None
        self.last_stage_check = 0
        self.record = {'label': label, 'limits_verified': False, 'cleanup_exit_confirmed': False,
                       'memory_cap_bytes': MEMORY_CAP, 'log_cap_bytes': LOG_CAP, 'raw_log_bytes': 0,
                       'peak_working_set_bytes': 0, 'peak_commit_bytes': 0, 'errors': []}

    def abort(self, why):
        with self.lock:
            if self.closed.is_set():
                return
            self.fault = self.fault or why
            if self.info.process:
                if self.assigned:
                    guard._ok(self.kernel.TerminateJobObject(self.job, 0xE004), 'TerminateJobObject(owned model)')
                else:
                    guard._ok(self.kernel.TerminateProcess(self.info.process, 0xE004), 'TerminateProcess(own suspended model)')

    def ensure(self):
        require(not self.fault, self.fault or 'Owned model failed')
        require(self.kernel.WaitForSingleObject(self.info.process, 0) == 258, 'Owned model exited unexpectedly')
        require(time.monotonic() < self.deadline-2, 'Aggregate deadline reached')

    def snapshot(self):
        self.ensure()
        counters = guard.MemoryCounters()
        counters.cb = C.sizeof(counters)
        guard._ok(self.memory.GetProcessMemoryInfo(self.info.process, C.byref(counters), C.sizeof(counters)), 'GetProcessMemoryInfo(own model)')
        with self.lock:
            self.record['peak_working_set_bytes'] = max(self.record['peak_working_set_bytes'], counters.peak_ws)
            self.record['peak_commit_bytes'] = max(self.record['peak_commit_bytes'], counters.peak_commit)
        require(counters.peak_ws <= MEMORY_CAP and counters.peak_commit <= MEMORY_CAP, 'Owned model memory cap exceeded')
        return {'working_set_bytes': counters.ws, 'private_bytes': counters.private,
                'commit_bytes': counters.commit, 'peak_working_set_bytes': counters.peak_ws,
                'peak_commit_bytes': counters.peak_commit}

    def __enter__(self):
        require(os.name == 'nt', 'Windows APIs required; no weaker launch fallback')
        self.kernel, self.memory = guard._apis()
        try:
            mask, system = guard.SIZE(), guard.SIZE()
            guard._ok(self.kernel.GetProcessAffinityMask(self.kernel.GetCurrentProcess(), C.byref(mask), C.byref(system)), 'Parent affinity')
            cpu = mask.value & -mask.value
            require(cpu, 'No allowed CPU')
            self.job = guard._ok(self.kernel.CreateJobObjectW(None, None), 'Create owned job')
            self.handles.append(self.job)
            limits = guard.ExtendedLimits()
            limits.basic.flags = 0x8 | 0x10 | 0x100 | 0x2000
            limits.basic.processes, limits.basic.affinity, limits.process_memory = 1, cpu, MEMORY_CAP
            guard._ok(self.kernel.SetInformationJobObject(self.job, 9, C.byref(limits), C.sizeof(limits)), 'Set owned job limits')
            verify = guard.ExtendedLimits()
            guard._ok(self.kernel.QueryInformationJobObject(self.job, 9, C.byref(verify), C.sizeof(verify), None), 'Read owned job limits')
            require(verify.basic.flags & limits.basic.flags == limits.basic.flags and verify.basic.processes == 1 and
                    verify.basic.affinity == cpu and verify.process_memory == MEMORY_CAP and not verify.basic.flags & (0x800 | 0x1000), 'Job limits mismatch')
            self.record.update(affinity_mask=cpu, job_limit_flags=verify.basic.flags)
            sa = guard.Security(C.sizeof(guard.Security), None, True)
            self.read, write = W.HANDLE(), W.HANDLE()
            guard._ok(self.kernel.CreatePipe(C.byref(self.read), C.byref(write), C.byref(sa), 4096), 'Create owned log pipe')
            self.handles.extend([self.read, write])
            guard._ok(self.kernel.SetHandleInformation(self.read, 1, 0), 'Remove read-pipe inheritance')
            null = self.kernel.CreateFileW('NUL', 0x80000000, 3, C.byref(sa), 3, 0, None)
            require(null != C.c_void_p(-1).value, 'Cannot open null stdin')
            self.handles.append(null)
            startup = guard.Startup()
            startup.cb, startup.flags, startup.show = C.sizeof(startup), 0x100 | 1, 0
            startup.stdin, startup.stdout, startup.stderr = null, write, write
            environment = {key: os.environ[key] for key in ('SystemRoot', 'WINDIR') if key in os.environ}
            private = self.temp.parent/'private'
            for name in ('programdata', 'appdata', 'localappdata'):
                root = private/name
                require(root.is_dir() and not (root/'llama.cpp/config.ini').exists(), 'Automatic config must be absent in private roots')
            environment.update(TEMP=str(self.temp), TMP=str(self.temp), PROGRAMDATA=str(private/'programdata'),
                               APPDATA=str(private/'appdata'), LOCALAPPDATA=str(private/'localappdata'),
                               OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1')
            env = C.create_unicode_buffer('\0'.join(f'{k}={v}' for k, v in sorted(environment.items()))+'\0\0')
            command = C.create_unicode_buffer(subprocess.list2cmdline(self.argv))
            guard._ok(self.kernel.CreateProcessW(self.argv[0], command, None, None, True, 0x4 | 0x400 | 0x08000000,
                                                env, str(self.cwd), C.byref(startup), C.byref(self.info)), 'Create owned suspended server')
            self.handles.extend([self.info.process, self.info.thread])
            self.record['owned_pid'] = self.info.pid
            self.kernel.CloseHandle(write)
            self.handles.remove(write)
            guard._ok(self.kernel.AssignProcessToJobObject(self.job, self.info.process), 'Assign owned server job')
            self.assigned = True
            guard._ok(self.kernel.SetProcessAffinityMask(self.info.process, cpu), 'Set owned server affinity')
            actual, system = guard.SIZE(), guard.SIZE()
            guard._ok(self.kernel.GetProcessAffinityMask(self.info.process, C.byref(actual), C.byref(system)), 'Read owned server affinity')
            require(actual.value == cpu, 'Affinity mismatch')
            low, high, flags = guard.SIZE(), guard.SIZE(), W.DWORD()
            guard._ok(self.kernel.GetProcessWorkingSetSizeEx(self.info.process, C.byref(low), C.byref(high), C.byref(flags)), 'Get working-set limits')
            require(low.value <= MEMORY_CAP, 'Minimum working set exceeds cap')
            guard._ok(self.kernel.SetProcessWorkingSetSizeEx(self.info.process, low.value, MEMORY_CAP, 0x4 | 0x2), 'Set hard working-set maximum')
            guard._ok(self.kernel.GetProcessWorkingSetSizeEx(self.info.process, C.byref(low), C.byref(high), C.byref(flags)), 'Read hard working-set maximum')
            require(high.value <= MEMORY_CAP and flags.value & 0x4, 'Working-set limit mismatch')
            self.record.update(limits_verified=True, working_set_max_bytes=high.value, working_set_flags=flags.value)
            self.log = (self.budget.directory/(self.label+'.server.log')).open('xb')
            guard._ok(self.kernel.ResumeThread(self.info.thread) != 0xFFFFFFFF, 'Resume owned server')
            self.monitor = threading.Thread(target=self._watch, name='prefix004-owned-monitor', daemon=True)
            self.monitor.start()
            return self
        except BaseException:
            self.close()
            raise

    def _watch(self):
        pending = b''
        try:
            while True:
                if not self.closed.is_set():
                    if time.monotonic() >= self.deadline-2:
                        raise TimeoutError('Aggregate deadline watchdog')
                    try:
                        self.snapshot()
                    except BaseException:
                        if not self.closed.is_set():
                            raise
                    if time.monotonic()-self.last_stage_check >= 1:
                        files = [p for p in self.temp.parent.rglob('*') if p.is_file()]
                        require(len(files) < 256 and sum(p.stat().st_size for p in files) < STAGE_CAP,
                                'Owned staging/cache-file budget exceeded')
                        self.last_stage_check = time.monotonic()
                available = W.DWORD()
                okay = self.kernel.PeekNamedPipe(self.read, None, 0, None, C.byref(available), None)
                require(okay or C.get_last_error() == 109, 'Owned log pipe failed')
                if available.value:
                    amount = min(16384, available.value)
                    buffer, count = C.create_string_buffer(amount), W.DWORD()
                    guard._ok(self.kernel.ReadFile(self.read, buffer, amount, C.byref(count), None), 'Read owned server log')
                    self.record['raw_log_bytes'] += count.value
                    require(self.record['raw_log_bytes'] <= LOG_CAP, 'Owned server log cap exceeded')
                    pending = (pending+buffer.raw[:count.value]).replace(self.secret, b'[REDACTED]')
                    keep = max(0, len(pending)-len(self.secret)+1)
                    if keep:
                        self.budget.reserve(keep)
                        self.log.write(pending[:keep])
                        self.log.flush()
                        pending = pending[keep:]
                else:
                    if self.closed.is_set():
                        break
                    self.closed.wait(.01)
        except BaseException as error:
            try:
                self.abort(type(error).__name__+': '+str(error))
            except BaseException as cleanup:
                self.record['errors'].append('Watchdog termination: '+str(cleanup))
        finally:
            if pending:
                data = pending.replace(self.secret, b'[REDACTED]')
                try:
                    self.budget.reserve(len(data))
                    self.log.write(data)
                except BaseException as error:
                    self.fault = self.fault or 'Final log write: '+str(error)

    def close(self):
        # Stop deadline/memory checks, then kill only the owned process and let
        # the monitor drain the remaining bounded pipe before handles close.
        self.closed.set()
        if self.info.process:
            try:
                if self.assigned:
                    guard._ok(self.kernel.TerminateJobObject(self.job, 0xE004), 'Close owned server job')
                else:
                    guard._ok(self.kernel.TerminateProcess(self.info.process, 0xE004), 'Close own suspended server')
                self.record['cleanup_exit_confirmed'] = self.kernel.WaitForSingleObject(self.info.process, 2000) == 0
                code = W.DWORD()
                if self.kernel.GetExitCodeProcess(self.info.process, C.byref(code)):
                    self.record['exit_code'] = code.value
            except BaseException as error:
                self.record['errors'].append('Owned cleanup: '+str(error))
        if self.monitor:
            self.monitor.join(2)
            if self.monitor.is_alive():
                self.record['errors'].append('Owned monitor did not join')
                self.fault = self.fault or 'Owned monitor did not join; resources retained until controller exit'
                self.record.update(fault=self.fault, resources_retained_due_live_monitor=True)
                # The daemon still owns these objects. Never race it by closing
                # its log/pipe/process handles, and never proceed to another cell.
                return
        if self.log:
            self.log.close()
        with self.lock:
            if hasattr(self, 'kernel'):
                for handle in reversed(self.handles):
                    if handle:
                        self.kernel.CloseHandle(handle)
            self.handles.clear()
            self.info.process = None
        self.record['fault'] = self.fault

    def __exit__(self, *_):
        self.close()
        require(self.record['cleanup_exit_confirmed'] and not self.record['errors'], 'Owned server cleanup unconfirmed')
        require(not self.fault, self.fault or 'Owned server failed')


@contextmanager
def http_deadline(server):
    server.ensure()
    seconds = min(HTTP_SECONDS, server.deadline-time.monotonic()-2)
    require(seconds > .1, 'Insufficient HTTP budget')
    timer = threading.Timer(seconds, lambda: server.abort('HTTP absolute deadline exceeded'))
    timer.daemon = True
    timer.start()
    began = time.monotonic()
    try:
        yield seconds
        require(time.monotonic()-began <= seconds, 'HTTP deadline elapsed including response handling')
        server.ensure()
    finally:
        timer.cancel()
        timer.join(.1)


def json_request(server, port, secret, path, payload=None):
    with http_deadline(server) as timeout:
        response = backend.request_json(port, path, payload, timeout=timeout, api_key=secret)
    require(len(json.dumps(response, ensure_ascii=False).encode()) <= 65536, 'Metadata response exceeds retained cap')
    return response


def stream_request(server, port, secret, payload):
    connection, http_status = None, None
    started = time.perf_counter()
    result = response_record(elapsed=0, secret=secret, reason='HTTP request has not completed')
    try:
        with http_deadline(server) as timeout:
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
            started = time.perf_counter()
            connection.request('POST', '/v1/chat/completions', body=json.dumps(payload).encode(),
                               headers={'Content-Type': 'application/json', 'Authorization': 'Bearer '+secret})
            response = connection.getresponse()
            http_status = response.status
            require(response.status == 200, 'Completion HTTP status '+str(response.status))
            require(response.getheader('Content-Type', '').startswith('text/event-stream'), 'Expected SSE response')
            result = collect_sse(response.readline, started, secret=secret)
        result['http_status'] = http_status
        return result
    except ResponseFailure as error:
        error.partial['http_status'] = http_status
        raise
    except BaseException as error:
        reason = sanitize(type(error).__name__+': '+str(error), secret)
        result.update(http_status=http_status, complete=False, response_status='http_failure', failure_reason=reason)
        result['request_ms'] = max(0, time.perf_counter()-started)*1000
        raise ResponseFailure(reason, sanitize(result, secret)) from None
    finally:
        if connection is not None:
            connection.close()


def capture_response(entry, field, server, port, secret, payload):
    try:
        entry[field] = stream_request(server, port, secret, payload)
        return entry[field]
    except ResponseFailure as error:
        entry[field] = error.partial
        entry['failed_phase'] = field
        raise


def require_counts(result, max_tokens):
    # Caller retains the result, including explicit nulls, before this gate.
    metrics, usage = result['metrics'], result['usage']
    reasons = []
    for name in ('prompt_tokens', 'completion_tokens'):
        if type(usage.get(name)) is not int or usage[name] < 0:
            reasons.append('Missing or invalid usage.'+name)
            result['unknown_reasons']['usage.'+name] = reasons[-1]
    if not result['required_metrics_supported']:
        reasons.append('Mandatory backend token counts unsupported or unavailable')
    supported = not reasons
    if supported:
        if metrics['cache_n']+metrics['prompt_n'] != usage['prompt_tokens']:
            reasons.append('Backend cache_n + prompt_n differs from usage.prompt_tokens')
        if metrics['predicted_n'] != usage['completion_tokens']:
            reasons.append('Backend predicted_n differs from usage.completion_tokens')
        if metrics['predicted_n'] > max_tokens or usage['completion_tokens'] > max_tokens:
            reasons.append('Reported generated tokens exceed declared output cap')
    result['counts_validity'] = {'supported': supported, 'consistent': supported and not reasons, 'reasons': reasons,
                                'declared_output_cap': max_tokens}
    require(not reasons, '; '.join(reasons))


def readiness_error(error):
    """Keep useful codes, never arbitrary messages/bodies/credentials/paths."""
    result = {'exception_type': type(error).__name__[:64]}
    for name, attribute in (('errno', 'errno'), ('winerror', 'winerror'), ('http_status', 'status')):
        value = getattr(error, attribute, None)
        result[name] = value if type(value) is int else None
    return result


def readiness_event(trace, stage, elapsed, *, response=None, error=None, alias=None):
    trace['stage'] = stage
    trace['events_seen'] += 1
    event = {'ordinal': trace['events_seen'], 'stage': stage, 'elapsed_ms': max(0, elapsed)*1000,
             'http_status': 200 if error is None else None}
    if error is not None:
        event.update(readiness_error(error))
        if stage == 'wrong_key_probe' and event['http_status'] in (401, 403):
            trace['wrong_key_rejected'] = True
        elif isinstance(error, (OSError, http.client.HTTPException, backend.BackendHttpError)):
            trace['last_transport_error'] = dict(event)
    elif stage in ('initial_health', 'inner_health'):
        okay = response.get('status') == 'ok'
        event['health_ok'] = okay
        trace['health_ok'] = trace['health_ok'] or okay
    elif stage == 'authorized_tokenizer':
        trace['authorized_tokenizer'] = isinstance(response.get('tokens'), list) and bool(response['tokens'])
    elif stage == 'model_inventory':
        data = response.get('data')
        trace['model_alias_verified'] = (isinstance(data, list) and all(isinstance(item, dict) for item in data)
                                          and [item.get('id') for item in data] == [alias])
    elif stage == 'properties':
        settings = response.get('default_generation_settings')
        trace['context_slots_verified'] = (isinstance(settings, dict) and settings.get('n_ctx') == CONTEXT
                                           and response.get('total_slots') == 1)
    trace['events'].append(event)
    if len(trace['events']) > 32:
        del trace['events'][16]  # Preserve the first 16 and most recent 16.
    trace['events_dropped'] = trace['events_seen']-len(trace['events'])


def ready(server, port, alias, secret):
    started = time.monotonic()
    trace = {'status': 'checking', 'ready': False, 'stage': 'not_started', 'events': [],
             'events_seen': 0, 'events_dropped': 0, 'last_transport_error': None,
             'health_ok': False, 'wrong_key_rejected': False, 'authorized_tokenizer': False,
             'model_alias_verified': False, 'context_slots_verified': False}
    # The existing finally receipt saves this record even if readiness aborts.
    server.record['readiness'] = trace
    timer = threading.Timer(min(45, server.deadline-time.monotonic()-2),
                            lambda: server.abort('Startup absolute 45-second deadline exceeded'))
    timer.daemon = True
    timer.start()
    try:
        properties = _ready(server, port, alias, secret)
        trace.update(status='ready', ready=True)
        return properties
    except BaseException as error:
        trace.update(status='failed', terminal_error=readiness_error(error))
        raise
    finally:
        trace['elapsed_ms'] = max(0, time.monotonic()-started)*1000
        timer.cancel()
        timer.join(.1)


def _ready(server, port, alias, secret):
    started = time.monotonic()
    trace = server.record['readiness']
    while time.monotonic()-started < 45:
        server.ensure()
        try:
            health = bounded_original(server, backend.request_json, port, '/health', None,
                                      HTTP_SECONDS, secret, trace, 'initial_health', alias)
            if health.get('status') == 'ok':
                # Reuse the existing exact authenticated inventory checks, but
                # its request function is bounded by this controller's watchdog.
                original = backend.request_json
                def observed(p, path, payload=None, timeout=180, api_key=None):
                    stage = ('inner_health' if path == '/health' else
                             ('authorized_tokenizer' if api_key == secret else 'wrong_key_probe') if path == '/tokenize' else
                             'model_inventory' if path == '/v1/models' else 'properties' if path == '/props' else 'other')
                    return bounded_original(server, original, p, path, payload, timeout, api_key, trace, stage, alias)
                backend.request_json = observed
                try:
                    return backend.wait_ready(ServerPoll(server), port, alias, CONTEXT, api_key=secret)
                finally:
                    backend.request_json = original
        except backend.BackendHttpError as error:
            if error.status != 503:
                raise
        except (ConnectionError, OSError, http.client.HTTPException):
            pass
        time.sleep(.1)
    raise TimeoutError('Owned server startup exceeded 45 seconds')


def bounded_original(server, function, port, path, payload, timeout, key, trace=None, stage=None, alias=None):
    started = time.monotonic()
    try:
        with http_deadline(server) as allowance:
            value = function(port, path, payload, timeout=min(timeout, allowance), api_key=key)
        require(len(json.dumps(value, ensure_ascii=False).encode()) <= 65536, 'Readiness metadata exceeds cap')
    except BaseException as error:
        if trace is not None:
            readiness_event(trace, stage, time.monotonic()-started, error=error)
        raise
    if trace is not None:
        readiness_event(trace, stage, time.monotonic()-started, response=value, alias=alias)
    return value


class ServerPoll:
    def __init__(self, server):
        self.server = server

    def poll(self):
        self.server.ensure()
        return None


def token_preflight(server, port, alias, secret, fixture):
    result = {}
    try:
        _token_preflight(server, port, alias, secret, fixture, result)
        validate_token_layout(result)
        return result
    except Exception as error:
        reason = sanitize('Token preflight: '+type(error).__name__+': '+str(error), secret)
        raise PreflightFailure(reason, sanitize(result, secret)) from None


def _token_preflight(server, port, alias, secret, fixture, result):
    for variant in ('seed', 'shared', 'changed'):
        body = request_body(alias, fixture, variant, False)
        rendered = json_request(server, port, secret, '/apply-template', {
            'model': alias, 'messages': body['messages'], 'add_generation_prompt': True,
            'chat_template_kwargs': body['chat_template_kwargs'], 'response_format': body['response_format']})
        prompt = rendered.get('prompt')
        require(isinstance(prompt, str) and prompt, 'Rendered prompt missing')
        item = {'rendered_utf8_sha256': hashlib.sha256(prompt.encode()).hexdigest(), 'options': {}}
        result[variant] = item
        for add in (False, True):
            response = json_request(server, port, secret, '/tokenize', {'model': alias, 'content': prompt,
                'add_special': add, 'parse_special': True, 'with_pieces': False})
            tokens = response.get('tokens')
            require(isinstance(tokens, list) and tokens and all(type(x) is int and x >= 0 for x in tokens), 'Exact tokens unavailable')
            item['options'][str(add).lower()] = {'count': len(tokens), 'tokens': tokens,
                'tokens_json_sha256': hashlib.sha256(json.dumps(tokens, separators=(',', ':')).encode()).hexdigest()}
            require(len(tokens)+32 < CONTEXT, 'Fixed input exceeds context reserve')


def cpu_arguments(server, model, port, alias, key):
    return [str(server), '--model', str(model), '--alias', alias, '--host', '127.0.0.1', '--port', str(port),
            '--ctx-size', str(CONTEXT), '--parallel', '1', '--device', 'none', '--gpu-layers', '0', '--split-mode', 'none',
            '--threads', '1', '--threads-batch', '1', '--batch-size', '128', '--ubatch-size', '128',
            '--cache-type-k', 'f16', '--cache-type-v', 'f16', '--flash-attn', 'off', '--fit', 'off', '--offline',
            '--no-context-shift', '--no-slots', '--cache-ram', '0', '--no-warmup', '--reasoning', 'off', '--api-key-file', str(key)]


def task_owned(path):
    raw = Path(path).absolute()
    require(not any(parent.is_symlink() or (hasattr(parent, 'is_junction') and parent.is_junction())
                    for parent in (raw, *raw.parents) if parent != parent.parent), 'Linked task path refused')
    resolved = raw.resolve()
    require(resolved != TASK and resolved.is_relative_to(TASK.resolve()), 'Path must stay below this task workspace')
    return resolved


def stage_inputs(identity_path, stage):
    identity = read_json(identity_path)
    files = identity['runtime_files']
    require({Path(x['path']).name for x in files} == set(RUNTIME_HASHES) and len(files) == 9, 'Runtime allowlist differs')
    model = identity['proposed_model']
    require(model['sha256'] == MODEL_SHA and model['bytes'] == MODEL_BYTES, 'Model manifest differs')
    require(Path(model['path']).name == 'Qwen3.5-0.8B-Q4_K_M.gguf', 'Unexpected model filename')
    sources = [(Path(x['path']), RUNTIME_HASHES[Path(x['path']).name], x['bytes'], 'runtime') for x in files]
    sources.append((Path(model['path']), MODEL_SHA, MODEL_BYTES, 'model'))
    require(sum(size for _, _, size, _ in sources) < STAGE_CAP, 'Staged input cap exceeded')
    # Entire staging operation is future --execute work, never validation work.
    stage.mkdir(exist_ok=False)
    for name in ('runtime', 'model', 'temp', 'credentials', 'private'):
        (stage/name).mkdir()
    for name in ('programdata', 'appdata', 'localappdata'):
        (stage/'private'/name).mkdir()
    for source, expected, size, kind in sources:
        require(source.is_file() and not source.is_symlink() and source.stat().st_size == size and sha(source) == expected,
                'Available source artifact identity changed')
        target = stage/kind/source.name
        shutil.copyfile(source, target)
        require(target.stat().st_size == size and sha(target) == expected, 'Staged artifact identity mismatch')
    require(set(x.name for x in (stage/'runtime').iterdir()) == set(RUNTIME_HASHES), 'Unexpected staged runtime file')
    return stage/'runtime/llama-server.exe', stage/'model'/Path(model['path']).name


def verify_stage(stage, model):
    require(set(p.name for p in (stage/'runtime').iterdir()) == set(RUNTIME_HASHES), 'Staged runtime allowlist changed')
    for name, expected in RUNTIME_HASHES.items():
        require(sha(stage/'runtime'/name) == expected, 'Staged runtime fingerprint changed')
    require(model.stat().st_size == MODEL_BYTES and sha(model) == MODEL_SHA, 'Staged model fingerprint changed')


def reviewed_inputs(args):
    review = read_json(args.reviewed_manifest)
    paths = {'protocol': HERE/'PREFIX-004.md', 'driver': Path(__file__), 'fixtures': args.fixtures,
             'backend_helper': BACKEND_PATH, 'resource_helper': GUARD_PATH, 'identity_manifest': args.identities}
    require(set(review.get('sha256', {})) == set(paths), 'Need complete reviewed input fingerprints')
    for name, path in paths.items():
        require(review['sha256'][name] == sha(path), 'Reviewed input changed: '+name)
    return {name: review['sha256'][name] for name in paths}


def run(args, fixture):
    require(os.name == 'nt', 'Execution requires Windows resource controls')
    for name in ('identities', 'reviewed_manifest', 'stage', 'output'):
        require(getattr(args, name) is not None, 'Execute requires --'+name.replace('_', '-'))
        setattr(args, name, task_owned(getattr(args, name)))
    require(not args.output.exists() and not args.stage.exists(), 'Stage and output must both be fresh')
    require(args.stage != args.output and not args.stage.is_relative_to(args.output) and not args.output.is_relative_to(args.stage),
            'Staged inputs and result artifacts must be separate')
    began = time.monotonic()
    deadline = began+ATTEMPT_SECONDS
    fingerprints = reviewed_inputs(args)
    args.output.mkdir(exist_ok=False)
    budget = Budget(args.output)
    report = {'experiment': 'PREFIX-004', 'status': 'starting', 'started_utc': backend.utc_now(), 'source_hashes': fingerprints,
              'live_model_execution_approved_by_this_flag': False, 'runs': [], 'tokenization_choice': None,
              'client_timer': None, 'assessment': None,
              'limits': {'attempt_seconds': ATTEMPT_SECONDS, 'server_memory_bytes': MEMORY_CAP,
                         'artifacts_bytes_exclusive': ARTIFACT_CAP, 'staging_bytes_exclusive': STAGE_CAP},
              'limitations': ['Direct backend qualification, not full Revia runtime or 9B Main performance',
                             'Resource guard is not a security sandbox; controller is outside the server job',
                             'One CPU affinity does not prove one OS thread; hard working-set limits may affect timings',
                             'Backend cache hits and numeric repeatability are measured, never assumed'], 'performance_claim': None}
    active = None
    secret = ''
    previous_temp = backend.tempfile.tempdir
    try:
        report['client_timer'] = clock_info()
        server_path, model_path = stage_inputs(args.identities, args.stage)
        backend.tempfile.tempdir = str(args.stage/'credentials')
        jobs = [{'label': 'preflight'}]+[dict(row, label=f"r{row['repeat']}-{row['variant']}-{'M' if row['cache_prompt'] else 'U'}") for row in schedule()]
        reference = None
        affinity = None
        for job in jobs:
            require(time.monotonic() < deadline-5, 'Aggregate deadline exhausted')
            # Freeze all source/helper/input fingerprints again before each launch.
            require(reviewed_inputs(args) == fingerprints, 'Inputs changed during experiment')
            verify_stage(args.stage, model_path)
            with backend.EphemeralApiKey() as credential:
                secret = credential.value
                port, alias = backend.fresh_port(), ALIAS
                argv = cpu_arguments(server_path, model_path, port, alias, credential.path)
                entry = dict(job, started_utc=backend.utc_now())
                started = time.perf_counter()
                active = OwnedServer(argv, args.stage/'runtime', args.stage/'temp', budget, job['label'], secret, deadline)
                try:
                    with active:
                        affinity = affinity or active.record['affinity_mask']
                        require(active.record['affinity_mask'] == affinity, 'Logical CPU changed between cells')
                        props = ready(active, port, alias, secret)
                        entry['startup_ms'] = (time.perf_counter()-started)*1000
                        require(entry['startup_ms'] <= 45000, 'Startup exceeded declared 45-second cap')
                        entry['after_ready'] = active.snapshot()
                        try:
                            tokens = token_preflight(active, port, alias, secret, fixture)
                        except PreflightFailure as error:
                            entry['token_preflight_failure'] = error.partial
                            raise
                        if reference is None:
                            reference = tokens
                            budget.json('token-preflight.json', tokens)
                            budget.json('properties.json', props)
                        else:
                            require(tokens == reference, 'Rendered template/token identities changed between cells')
                            primer = capture_response(entry, 'primer', active, port, secret,
                                                      request_body(alias, fixture, 'seed', True, primer=True))
                            require_counts(primer, 1)
                            prompt_count = primer['usage'].get('prompt_tokens')
                            require(type(prompt_count) is int, 'Primer prompt token usage missing')
                            candidates = [name for name in ('false', 'true') if reference['seed']['options'][name]['count'] == prompt_count]
                            require(candidates, 'No exact tokenizer choice reconciles primer prompt usage')
                            choice = report['tokenization_choice'] or candidates[0]
                            require(choice in candidates, 'Tokenizer/usage convention changed')
                            report['tokenization_choice'] = choice
                            entry['after_primer'] = active.snapshot()
                            entry['before_target'] = active.snapshot()
                            target = capture_response(entry, 'target', active, port, secret,
                                                      request_body(alias, fixture, job['variant'], job['cache_prompt']))
                            entry['after_target'] = active.snapshot()
                            require_counts(target, 32)
                            require(job['cache_prompt'] or target['metrics']['cache_n'] == 0,
                                    'Cache-disabled control reported reuse; unsupported comparison')
                            expected_tokens = reference[job['variant']]['options'][choice]['count']
                            require(type(target['usage'].get('prompt_tokens')) is int and target['usage']['prompt_tokens'] == expected_tokens,
                                    'Target usage/tokenizer prompt count mismatch')
                            require(type(target['usage'].get('completion_tokens')) is int and target['usage']['completion_tokens'] == target['metrics']['predicted_n'],
                                    'Generated usage/backend count mismatch')
                            require(target['ttft_ms'] is not None, 'No visible first content token')
                            entry['grade'] = backend.grade(target['content'], target['finish_reason'], fixture['expected'])
                            require(entry['grade']['passed'], 'Public answer or finish contract failed')
                finally:
                    entry['owned_server'] = active.record
                    entry['finished_utc'] = backend.utc_now()
                    report['runs'].append(entry)
                    budget.json(job['label']+'.json', entry)
                active = None
        require(len(report['runs']) == 21, 'Fixed launch count differs')
        report['assessment'] = assess_results(report['runs'], report['client_timer'])
        report['status'] = 'completed'
    except BaseException as error:
        report['status'] = 'interrupted' if isinstance(error, KeyboardInterrupt) else 'stopped'
        report['reason'] = (type(error).__name__+': '+str(error)).replace(secret, '[REDACTED]') if secret else type(error).__name__+': '+str(error)
    finally:
        backend.tempfile.tempdir = previous_temp
        report['finished_utc'] = backend.utc_now()
        finalize_report(budget, report, began)
    print(json.dumps({'status': report['status'], 'runs': len(report['runs']), 'elapsed_seconds': report['elapsed_seconds'],
                      'reason': report.get('reason'), 'performance_claim': None}))
    return 0 if report['status'] == 'completed' else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--validate-only', action='store_true')
    mode.add_argument('--execute', action='store_true', help='Requires prior external parent protocol review; this flag grants no approval')
    parser.add_argument('--fixtures', type=Path, default=HERE/'prefix004-fixtures.json')
    parser.add_argument('--identities', type=Path)
    parser.add_argument('--reviewed-manifest', type=Path)
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    fixture = validate_fixtures(args.fixtures)
    if not args.execute:
        print(json.dumps({'status': 'offline_preparation_validated', 'binary_model_network_or_staging_execution': False,
                          'variants': list(fixture['variants']), 'planned_owned_launches': 21,
                          'fixtures_sha256': sha(args.fixtures), 'driver_sha256': sha(__file__),
                          'backend_helper_sha256': BACKEND_SHA, 'resource_helper_sha256': GUARD_SHA,
                          'live_metrics': None, 'performance_claim': None}))
        return 0
    return run(args, fixture)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print('PREFIX-004 refused: '+type(error).__name__+': '+str(error), file=sys.stderr)
        raise SystemExit(2)
