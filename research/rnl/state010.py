"""STATE-010 prepared saved-encoder linear/identity/ReLU reader study.

Import/default/validation are stdlib-only. Only a future explicit --worker path
uses the existing CPU framework under external approved resource supervision.
No encoder training/replay, network calls, pickle or arbitrary native loader.
"""
import argparse
from copy import deepcopy
import hashlib
import json
import math
from pathlib import Path
import re
import stat
import struct
import time

import state009 as old
import state010_data as data

previous, replay, original = old.previous, old.replay, old.original
SCHEMA = 'STATE-010-cell-v1'
MODELS, SEEDS = ('gru',), (11, 23, 37)
ARMS = ('direct_linear', 'factorized_identity', 'factorized_relu')
CURVES = (0, 168, 1008)
SOURCE_RAW_SHA256 = '6aab4c7e3f9b0109f07c46a5b0c64c65f9579875a70e1a4a832b8c38b61cde57'
SNAPSHOTS = {'direct-initial.f32': 'direct', 'factorized-initial.f32': 'factorized',
             'direct_linear-final.f32': 'direct', 'factorized_identity-final.f32': 'factorized',
             'factorized_relu-final.f32': 'factorized'}
history_rows, history_index = old.history_rows, old.history_index


def _inventory(layout):
    answer, offset = [], 0
    for name, shape in layout.items():
        size = 4 * math.prod(shape)
        answer.append(dict(name=name, shape=shape, offset_bytes=offset, bytes=size))
        offset += size
    return answer


def default_config():
    """Pure template; preparation supplies exact raw-file and data fingerprints."""
    before = old.read_config(Path(__file__).parent / 'STATE-009-config.json')
    references = {str(seed): before['reference_weights'][str(seed)]['1152'] for seed in SEEDS}
    return dict(schema='STATE-010-config-v1', models=list(MODELS), seeds=list(SEEDS), arms=list(ARMS),
        curves=list(CURVES), encoder_updates=0, updates=1008, batch=16, train_count=2688, epochs=6,
        train_blocks=list(data.TRAIN_NAMES), groups=168, count=672, evaluation_seed=10010,
        diagnostic_name=data.DIAGNOSTIC_NAME, final_name=data.FINAL_NAME,
        history_events=63, features=32, reader_seed_base=1001000,
        learning_rate=.003, weight_decay=0, gradient_clip=1., torch_version='2.15.0+cpu',
        source_raw_result_sha256=SOURCE_RAW_SHA256, reference_encoder_parameter_sha256=references,
        source_encoders={str(seed): dict(name='trained-encoder.f32', kind='encoder', bytes=19872,
            sha256='0' * 64, parameter_sha256=references[str(seed)], tensors=_inventory(old.snapshot_layout('encoder')))
            for seed in SEEDS},
        set_fingerprints={name: '0' * 64 for name in data.NAMES},
        signal_final_quartets=17, signal_final_individual=168, decoder_advantage_quartets=17,
        reliability_final_quartets=135, reliability_final_individual=605)


def validate_source_record(record, expected_parameter_sha):
    if (not isinstance(record, dict)
            or set(record) != {'name', 'kind', 'bytes', 'sha256', 'parameter_sha256', 'tensors'}
            or record['name'] != 'trained-encoder.f32' or record['kind'] != 'encoder'
            or type(record['bytes']) is not int or record['bytes'] != 19872
            or not replay._sha(record['sha256']) or record['parameter_sha256'] != expected_parameter_sha
            or not replay._same_typed(record['tensors'], _inventory(old.snapshot_layout('encoder')))):
        raise ValueError('Fixed source encoder identity/shape/inventory required')
    return record


def validate_config(config):
    template = default_config()
    if not isinstance(config, dict) or set(config) != set(template):
        raise ValueError('Exact scientific configuration fields required')
    for key, expected in template.items():
        if key == 'set_fingerprints':
            if (not isinstance(config[key], dict) or set(config[key]) != set(data.NAMES)
                    or not all(replay._sha(value) for value in config[key].values())):
                raise ValueError('Six exact fresh-set fingerprints required')
        elif key == 'source_encoders':
            if not isinstance(config[key], dict) or set(config[key]) != set(map(str, SEEDS)):
                raise ValueError('Three fixed saved encoders required')
            for seed in SEEDS:
                validate_source_record(config[key][str(seed)], template['reference_encoder_parameter_sha256'][str(seed)])
        elif not replay._same_typed(config[key], expected):
            raise ValueError('Frozen scientific setting mismatch: ' + key)
    return config


def read_config(path):
    def pairs(items):
        out = {}
        for key, value in items:
            if key in out: raise ValueError('Duplicate configuration key')
            out[key] = value
        return out
    with open(path, encoding='utf-8') as stream:
        config = json.load(stream, object_pairs_hook=pairs,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite configuration')))
    return validate_config(config)


def serialize_report(report):
    """Exact compact finite JSON used by the prequalified capture budget."""
    return json.dumps(report, sort_keys=True, allow_nan=False, separators=(',', ':'))


def reader_schedule():
    for epoch in range(6):
        for first in range(0, 2688, 16):
            yield epoch * 168 + first // 16 + 1, first, first + 16


def snapshot_layout(kind):
    if kind == 'direct':
        return {'heads.bias': [32], 'heads.weight': [32, 32]}
    if kind == 'factorized':
        return {'heads.bias': [32], 'heads.weight': [32, 16],
                'shared.bias': [16], 'shared.weight': [16, 32]}
    raise ValueError('Fixed reader checkpoint kind required')


def snapshot_bytes(kind):
    return 4 * sum(math.prod(shape) for shape in snapshot_layout(kind).values())


def encode_snapshot(tensors, kind):
    layout = snapshot_layout(kind)
    if not isinstance(tensors, dict) or set(tensors) != set(layout):
        raise ValueError('Exact tensor-name set required')
    pieces = []
    for name, shape in layout.items():
        actual_shape, values = tensors[name]
        if (not replay._same_typed(actual_shape, shape) or len(values) != math.prod(shape)
                or any(type(v) not in (int, float) or not math.isfinite(v) for v in values)):
            raise ValueError('Finite values and exact tensor shape required')
        try: raw = struct.pack('<' + str(len(values)) + 'f', *values)
        except (OverflowError, struct.error) as exc: raise ValueError('FP32 range exceeded') from exc
        if not all(math.isfinite(v[0]) for v in struct.iter_unpack('<f', raw)):
            raise ValueError('Nonfinite FP32 checkpoint')
        pieces.append(raw)
    return b''.join(pieces), _inventory(layout)


def decode_snapshot(raw, inventory, kind):
    if (not isinstance(raw, bytes) or len(raw) != snapshot_bytes(kind)
            or not replay._same_typed(inventory, _inventory(snapshot_layout(kind)))):
        raise ValueError('Exact bounded bytes and fixed nonoverlapping inventory required')
    tensors = {}
    for row in inventory:
        values = [v[0] for v in struct.iter_unpack('<f', raw[row['offset_bytes']:row['offset_bytes'] + row['bytes']])]
        if not all(math.isfinite(v) for v in values): raise ValueError('Nonfinite checkpoint')
        tensors[row['name']] = (row['shape'], values)
    return tensors


tensor_digest = old.tensor_digest


def write_snapshot_bytes(directory, name, raw, inventory, kind):
    if SNAPSHOTS.get(name) != kind: raise ValueError('Fixed safe reader filename required')
    decode_snapshot(raw, inventory, kind)
    path = old._plain_directory(directory) / name
    with path.open('xb') as stream: stream.write(raw)
    record = dict(name=name, kind=kind, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                  parameter_sha256=tensor_digest(raw, inventory), tensors=inventory)
    verify_snapshot_file(directory, record)
    return record


def verify_snapshot_file(directory, record):
    if (not isinstance(record, dict)
            or set(record) != {'name', 'kind', 'bytes', 'sha256', 'parameter_sha256', 'tensors'}
            or SNAPSHOTS.get(record.get('name')) != record.get('kind')):
        raise ValueError('Fixed reader snapshot record required')
    kind = record['kind']
    if type(record['bytes']) is not int or record['bytes'] != snapshot_bytes(kind):
        raise ValueError('Exact checkpoint byte count required')
    path = old._plain_directory(directory) / record['name']
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400 or info.st_size != record['bytes']:
        raise ValueError('Plain exact-size checkpoint file required')
    raw = path.read_bytes()
    tensors = decode_snapshot(raw, record['tensors'], kind)
    if hashlib.sha256(raw).hexdigest() != record['sha256'] or tensor_digest(raw, record['tensors']) != record['parameter_sha256']:
        raise ValueError('Checkpoint hash mismatch')
    return tensors


def source_tensors(config, seed, directory):
    """Verify saved raw bytes/shape/parameter identity before any native setup."""
    validate_config(config)
    if type(seed) is not int or seed not in SEEDS: raise ValueError('Fixed encoder seed required')
    record = config['source_encoders'][str(seed)]
    validate_source_record(record, config['reference_encoder_parameter_sha256'][str(seed)])
    return old.verify_snapshot_file(directory, record)


def readout_reference(family, tensors, feature, key, blind=False):
    if (family not in ARMS or type(key) is not int or not 0 <= key < 4 or type(blind) is not bool
            or len(feature) != 32 or any(type(x) not in (int, float) or not math.isfinite(x) for x in feature)):
        raise ValueError('Fixed family, finite32Dfeature and observable key required')
    kind = 'direct' if family == 'direct_linear' else 'factorized'
    encode_snapshot(tensors, kind)  # Shape/finite validation, independent scalar math below.
    values = {name: pair[1] for name, pair in tensors.items()}
    if kind == 'factorized':
        hidden = [values['shared.bias'][i] + sum(values['shared.weight'][i * 32 + j] * feature[j] for j in range(32)) for i in range(16)]
        feature = [max(0., value) for value in hidden] if family == 'factorized_relu' else hidden
    width = len(feature)
    flat = [values['heads.bias'][i] + sum(values['heads.weight'][i * width + j] * feature[j] for j in range(width)) for i in range(32)]
    if not all(math.isfinite(x) for x in flat): raise ValueError('Nonfinite scalar reference')
    return [sum(flat[k * 8 + v] for k in range(4)) / 4 for v in range(8)] if blind else flat[key * 8:(key + 1) * 8]


def checked_split(config, name):
    episodes = data.make_split(name, config['evaluation_seed'])
    if data.fingerprint(episodes) != config['set_fingerprints'][name]: raise ValueError('Fresh dataset identity mismatch')
    data.validate_split(episodes)
    return episodes


def validate_evaluation(record, episodes, *, retain_rows):
    if (not isinstance(record, dict) or set(record) != {'loss', 'seconds', 'preserved', 'metrics'}
            or not replay._number(record['loss']) or not replay._number(record['seconds']) or record['preserved'] is not True):
        raise ValueError('Finite preserved evaluation required')
    metrics = record['metrics']; old._metric_counts(metrics)
    if retain_rows:
        rows = metrics.get('rows')
        if (not isinstance(rows, list) or len(rows) != len(episodes)
                or any(not isinstance(r, list) or len(r) != 8 or any(type(x) is not int for x in r) for r in rows)):
            raise ValueError('Exact compact integer prediction rows required')
        if not replay._same_typed(metrics, data.prediction_metrics(episodes, [r[-1] for r in rows])):
            raise ValueError('Predictions, identities and metrics disagree')
    elif 'rows' in metrics:
        raise ValueError('Unretained early/probe prediction rows forbidden')
    elif metrics.get('baselines') != data.prediction_metrics(episodes, [0] * len(episodes))['baselines']:
        raise ValueError('Exact task-reference records required')
    return record


def validate_blind_check(record, stage):
    expected = dict(stage=stage, groups=1, count=4, logits_identical=True,
                    query_pairs_correct=0, quartets_correct=0, preserved=True)
    if stage not in ('initial', 'final') or not replay._same_typed(record, expected):
        raise ValueError('Preserved four-case evaluation-only blind control required')
    return record


def screen_scores(rows):
    if (not isinstance(rows, list) or len(rows) != 9
            or {(r.get('seed'), r.get('arm')) for r in rows} != {(s, a) for s in SEEDS for a in ARMS}):
        raise ValueError('Nine fixed family/seed scores required')
    by_seed, paired = {}, []
    for row in rows:
        if (type(row['seed']) is not int or type(row.get('individual')) is not int or not 0 <= row['individual'] <= 672
                or type(row.get('quartets')) is not int or not 0 <= row['quartets'] <= 168):
            raise ValueError('Valid integer final scores required')
        by_seed.setdefault(row['seed'], {})[row['arm']] = row
    for seed in SEEDS:
        direct, identity, relu = (by_seed[seed][name] for name in ARMS)
        d, i = relu['quartets'] - direct['quartets'], relu['quartets'] - identity['quartets']
        paired.append(dict(seed=seed, relu_signal=relu['quartets'] >= 17 and relu['individual'] >= 168,
                           relu_advantage=d >= 17 and i >= 17, relu_minus_direct_quartets=d,
                           relu_minus_identity_quartets=i, relu_reliability=relu['quartets'] >= 135 and relu['individual'] >= 605))
    return dict(status='complete', paired_seeds=paired, all_relu_signal=all(r['relu_signal'] for r in paired),
        all_relu_advantage=all(r['relu_advantage'] for r in paired), all_relu_reliability=all(r['relu_reliability'] for r in paired),
        candidate_for_revia=False, integrated_into_revia=False,
        interpretation='Descriptive fixed-sample screens, not significance/integration gates. No absent-information or original-QUERY-transition causal claim.')


def validate_cell(cell, config):
    validate_config(config)
    if (not isinstance(cell, dict) or cell.get('schema') != SCHEMA or cell.get('status') != 'complete'
            or cell.get('model') != 'gru' or type(cell.get('seed')) is not int or cell['seed'] not in SEEDS
            or cell.get('native_executed') is not True or cell.get('cpu_only') is not True
            or cell.get('torch_version') != config['torch_version'] or cell.get('encoder_capacity') != previous.capacity('gru')
            or cell.get('data_identity') != config['set_fingerprints'] or cell.get('shared_feature_cache') is not True
            or cell.get('final_features_after_all_fits') is not True):
        raise ValueError('Complete saved-encoder native seed record required')
    seed, source = cell['seed'], cell.get('source_encoder')
    if (not isinstance(source, dict) or source.get('updates') != 0 or source.get('frozen_preserved') is not True
            or source.get('loaded_parameter_sha256') != config['reference_encoder_parameter_sha256'][str(seed)]
            or source.get('record') != config['source_encoders'][str(seed)]
            or source.get('source_raw_result_sha256') != SOURCE_RAW_SHA256):
        raise ValueError('Exact saved encoder load, with zero encoder updates required')
    hashes = cell.get('feature_sha256')
    if not isinstance(hashes, dict) or set(hashes) != {'train', 'diagnostic', 'final'} or not all(replay._sha(v) for v in hashes.values()):
        raise ValueError('One preserved shared feature cache identity per set required')
    arms = cell.get('arms')
    if not isinstance(arms, list) or [a.get('name') for a in arms] != list(ARMS): raise ValueError('Three fixed reader families required')
    diagnostic = checked_split(config, data.DIAGNOSTIC_NAME)
    final = checked_split(config, data.FINAL_NAME)
    probe = checked_split(config, data.TRAIN_NAMES[0])
    factor_initial = set()
    for arm in arms:
        name = arm['name']; kind = 'direct' if name == 'direct_linear' else 'factorized'
        if (arm.get('updates') != 1008 or arm.get('parameters') != snapshot_bytes(kind) // 4 or arm.get('main_forward_macs') != 1024
                or arm.get('selection') != 'supplied_observable_key' or arm.get('frozen_features_preserved') is not True
                or arm.get('evaluation_preserved') is not True or arm.get('rng_preserved') is not True
                or not replay._sha(arm.get('initial_reader_sha256')) or not replay._sha(arm.get('final_reader_sha256'))):
            raise ValueError('Fixed complete preserved reader fit required')
        if kind == 'factorized': factor_initial.add(arm['initial_reader_sha256'])
        controls = arm.get('blind_checks')
        if not isinstance(controls, list) or len(controls) != 2: raise ValueError('Both evaluation-only blind stages required')
        for record, stage in zip(controls, ('initial', 'final')): validate_blind_check(record, stage)
        curves = arm.get('curves')
        if not isinstance(curves, list) or [r.get('updates') for r in curves] != list(CURVES): raise ValueError('All fixed curves required')
        for curve in curves:
            validate_evaluation(curve['training_probe'], probe, retain_rows=False)
            validate_evaluation(curve['diagnostic'], diagnostic, retain_rows=curve['updates'] == 1008)
        holdout = arm.get('final_holdout')
        if (not isinstance(holdout, dict) or holdout.get('evaluations') != 1
                or holdout.get('evaluated_after_updates') != 1008 or holdout.get('all_reader_fits_complete') is not True):
            raise ValueError('Exactly one final evaluation after all fixed reader fits required')
        validate_evaluation(holdout['evaluation'], final, retain_rows=True)
    if len(factor_initial) != 1: raise ValueError('Identity and ReLU must share exact initial tensors')
    return cell


def validate_artifacts(cell, directory):
    records = cell.get('snapshots')
    if not isinstance(records, list) or len(records) != 5 or {r.get('name') for r in records} != set(SNAPSHOTS):
        raise ValueError('Five exact reader snapshots per seed required')
    for record in records: verify_snapshot_file(directory, record)
    arm = {a['name']: a for a in cell['arms']}
    expected = {'direct-initial.f32': arm['direct_linear']['initial_reader_sha256'],
                'factorized-initial.f32': arm['factorized_identity']['initial_reader_sha256'],
                **{name + '-final.f32': a['final_reader_sha256'] for name, a in arm.items()}}
    if any(r['parameter_sha256'] != expected[r['name']] for r in records): raise ValueError('Retained reader differs from evaluated tensors')
    if {p.name for p in Path(directory).iterdir()} != set(SNAPSHOTS): raise ValueError('Unexpected reader artifact files')


def assess(cells, config):
    if not isinstance(cells, list) or len(cells) != 3 or {c.get('seed') for c in cells} != set(SEEDS):
        raise ValueError('Three complete fixed seed workers required')
    rows = []
    for cell in cells:
        validate_cell(cell, config)
        for arm in cell['arms']:
            m = arm['final_holdout']['evaluation']['metrics']
            rows.append(dict(seed=cell['seed'], arm=arm['name'], individual=m['individual']['correct'], quartets=m['quartets']['correct']))
    return screen_scores(rows)


def validation_report(config):
    validate_config(config)
    splits = [checked_split(config, name) for name in data.NAMES]
    cfg9 = old.read_config(Path(__file__).parent / 'STATE-009-config.json')
    cfg8 = replay.read_config(Path(__file__).parent / 'STATE-008-config.json')
    cfg7 = original.read_config(Path(__file__).parent / 'STATE-007-config.json')
    def references():
        for seed in SEEDS: yield from original.training_pool(seed)
        yield from original.diagnostic_split(cfg7)
        yield from original.final_holdout(cfg7, 1152)
        for name in old.prior_data.NAMES: yield from replay.checked_split(cfg8, name)
        for name in old.data.NAMES: yield from old.checked_split(cfg9, name)
    separation = data.validate_disjoint(splits, references())
    return dict(schema='STATE-010-preparation-v1', status='prepared_native_unverified', native_executed=False,
                trained=False, encoder_updates=0, data={name: data.validate_split(es) for name, es in zip(data.NAMES, splits)},
                disjoint=separation, head_fits_future=9, retained_prediction_rows_future=12096,
                snapshot_parameter_bytes_future=63936, native_validation='Unexecuted; separately approved guarded worker required')


def _save_model(model, directory, name, kind):
    raw, inventory = encode_snapshot(old._model_tensors(model), kind)
    return write_snapshot_bytes(directory, name, raw, inventory, kind)


def _new_reader(torch, seed, factorized):
    rng = torch.get_rng_state().clone()
    try:
        torch.default_generator.manual_seed(1001000 + (100 if factorized else 0) + seed)
        class Reader(torch.nn.Module):
            def __init__(self):
                super().__init__()
                if factorized: self.shared = torch.nn.Linear(32, 16, device='cpu', dtype=torch.float32)
                self.heads = torch.nn.Linear(16 if factorized else 32, 32, device='cpu', dtype=torch.float32)
        return Reader()
    finally: torch.set_rng_state(rng)


def _head_logits(reader, features, keys, family, blind=False):
    if family != 'direct_linear':
        features = reader.shared(features)
        if family == 'factorized_relu': features = features.relu()
    logits = reader.heads(features).reshape(-1, 4, 8)
    return logits.mean(dim=1) if blind else logits.gather(1, keys.reshape(-1, 1, 1).expand(-1, 1, 8)).squeeze(1)


def _oracle_check(torch, reader, family, feature):
    tensors = old._model_tensors(reader)
    with torch.no_grad():
        for blind in (False, True):
            for key in range(4):
                actual = _head_logits(reader, feature, torch.tensor([key], dtype=torch.long), family, blind)[0].tolist()
                expected = readout_reference(family, tensors, feature[0].tolist(), key, blind)
                if any(abs(a - b) > 1e-5 for a, b in zip(actual, expected)):
                    raise RuntimeError('Native forward differs from independent scalar reference')


def _blind_check(torch, reader, family, features, diagnostic, stage):
    mode, weights, rng = reader.training, previous._weight_digest(reader), torch.get_rng_state().clone()
    try:
        reader.eval()
        with torch.no_grad():
            logits = _head_logits(reader, features[:4], torch.tensor([e['events'][-1][1] for e in diagnostic[:4]], dtype=torch.long), family, True)
            if not bool(torch.isfinite(logits).all()): raise RuntimeError('Nonfinite blind-check logits')
            identical = all(bool(torch.equal(logits[i], logits[i + 1])) for i in (0, 2))
            metrics = data.prediction_metrics(diagnostic[:4], logits.argmax(dim=1).tolist())
    finally: reader.train(mode)
    if previous._weight_digest(reader) != weights or not bool(torch.equal(rng, torch.get_rng_state())):
        raise RuntimeError('Blind pipeline check changed reader/RNG')
    return validate_blind_check(dict(stage=stage, groups=metrics['groups'], count=metrics['count'],
        logits_identical=identical, query_pairs_correct=metrics['query_pairs']['correct'],
        quartets_correct=metrics['quartets']['correct'], preserved=True), stage)


def _evaluate(torch, reader, family, features, episodes, retain_rows):
    start = time.perf_counter()
    mode, weights, rng = reader.training, previous._weight_digest(reader), torch.get_rng_state().clone()
    predictions, loss_sum = [], 0.
    try:
        reader.eval()
        with torch.no_grad():
            for first in range(0, len(episodes), 16):
                batch = episodes[first:first + 16]
                keys = torch.tensor([e['events'][-1][1] for e in batch], dtype=torch.long)
                target = torch.tensor([e['target'] for e in batch], dtype=torch.long)
                logits = _head_logits(reader, features[first:first + 16], keys, family)
                loss = torch.nn.functional.cross_entropy(logits, target, reduction='sum')
                if not bool(torch.isfinite(logits).all()) or not bool(torch.isfinite(loss)):
                    raise RuntimeError('Nonfinite readout evaluation')
                predictions.extend(logits.argmax(dim=1).tolist()); loss_sum += loss.item()
    finally: reader.train(mode)
    if (reader.training != mode or previous._weight_digest(reader) != weights
            or not bool(torch.equal(rng, torch.get_rng_state()))):
        raise RuntimeError('Reader evaluation changed weights, RNG or mode')
    metrics = data.prediction_metrics(episodes, predictions)
    if not retain_rows: del metrics['rows']
    record = dict(metrics=metrics, loss=loss_sum / len(episodes), seconds=time.perf_counter() - start, preserved=True)
    return validate_evaluation(record, episodes, retain_rows=retain_rows)


def run_worker(model_name, seed, config, encoder_dir, directory, report):
    validate_config(config)
    if model_name != 'gru' or type(seed) is not int or seed not in SEEDS: raise ValueError('Fixed saved-encoder seed required')
    directory = old._plain_directory(directory)
    if any(directory.iterdir()): raise ValueError('Fresh empty reader checkpoint directory required')
    start = time.perf_counter()
    # The only source read/validation completes before native framework entry.
    tensors = source_tensors(config, seed, encoder_dir)
    expected = config['reference_encoder_parameter_sha256'][str(seed)]
    report.update(arms=[], snapshots=[], data_identity={}, timings={},
        source_encoder=dict(record=deepcopy(config['source_encoders'][str(seed)]),
            source_raw_result_sha256=SOURCE_RAW_SHA256, loaded_parameter_sha256=None, updates=0, frozen_preserved=False))
    blocks = [checked_split(config, name) for name in data.TRAIN_NAMES]
    diagnostic = checked_split(config, data.DIAGNOSTIC_NAME)
    report['data_identity'].update({name: config['set_fingerprints'][name] for name in (*data.TRAIN_NAMES, data.DIAGNOSTIC_NAME)})
    report['native_executed'] = True
    torch, encoder = previous._native_setup('gru', seed)
    if torch.__version__ != config['torch_version']: raise RuntimeError('Exact existing CPU framework required')
    loaded = {name: torch.tensor(values, dtype=torch.float32, device='cpu').reshape(shape) for name, (shape, values) in tensors.items()}
    encoder.load_state_dict(loaded, strict=True)
    if previous._weight_digest(encoder) != expected: raise RuntimeError('Native saved encoder parameter mismatch')
    old._freeze(encoder); old._frozen(torch, encoder, expected)
    report.update(cpu_only=True, torch_version=torch.__version__, encoder_capacity=previous.capacity('gru'))
    report['source_encoder']['loaded_parameter_sha256'] = expected
    training = [e for block in blocks for e in block]
    features = {'train': old._cache(torch, encoder, training), 'diagnostic': old._cache(torch, encoder, diagnostic)}
    feature_hashes = {name: old._feature_hash(value) for name, value in features.items()}
    report['shared_feature_cache'] = True
    direct, factor = _new_reader(torch, seed, False), _new_reader(torch, seed, True)
    direct_hash, factor_hash = previous._weight_digest(direct), previous._weight_digest(factor)
    report['snapshots'].extend([_save_model(direct, directory, 'direct-initial.f32', 'direct'),
                                _save_model(factor, directory, 'factorized-initial.f32', 'factorized')])
    report['timings']['load_and_initial_cache_seconds'] = time.perf_counter() - start
    readers = {}
    for name in ARMS:
        arm_start = time.perf_counter(); rng = torch.get_rng_state().clone()
        kind = 'direct' if name == 'direct_linear' else 'factorized'
        reader = deepcopy(direct if kind == 'direct' else factor)
        initial_hash = direct_hash if kind == 'direct' else factor_hash
        if previous._weight_digest(reader) != initial_hash or sum(p.numel() for p in reader.parameters()) != snapshot_bytes(kind) // 4:
            raise RuntimeError('Fixed reader initialization/capacity mismatch')
        _oracle_check(torch, reader, name, features['train'][:1])
        arm = dict(name=name, updates=0, parameters=snapshot_bytes(kind) // 4, main_forward_macs=1024,
                   selection='supplied_observable_key', initial_reader_sha256=initial_hash,
                   curves=[], blind_checks=[_blind_check(torch, reader, name, features['diagnostic'], diagnostic, 'initial')], final_holdout=None)
        report['arms'].append(arm)
        opt = torch.optim.Adam(reader.parameters(), lr=.003, weight_decay=0)
        def curve(update):
            arm['curves'].append(dict(updates=update,
                training_probe=_evaluate(torch, reader, name, features['train'][:672], blocks[0], False),
                diagnostic=_evaluate(torch, reader, name, features['diagnostic'], diagnostic, update == 1008)))
        curve(0); losses, correct = [], 0
        for update, first, last in reader_schedule():
            reader.train(); opt.zero_grad(set_to_none=True)
            batch = training[first:last]
            keys = torch.tensor([e['events'][-1][1] for e in batch], dtype=torch.long)
            target = torch.tensor([e['target'] for e in batch], dtype=torch.long)
            logits = _head_logits(reader, features['train'][first:last], keys, name)
            loss = torch.nn.functional.cross_entropy(logits, target)
            if not bool(torch.isfinite(logits).all()) or not bool(torch.isfinite(loss)): raise RuntimeError('Nonfinite reader training')
            losses.append(loss.item()); correct += int((logits.argmax(dim=1) == target).sum().item())
            loss.backward(); torch.nn.utils.clip_grad_norm_(reader.parameters(), 1., error_if_nonfinite=True); opt.step()
            arm['updates'] = update
            if update in CURVES: curve(update)
        del opt
        old._freeze(reader)
        arm['blind_checks'].append(_blind_check(torch, reader, name, features['diagnostic'], diagnostic, 'final'))
        if any(old._feature_hash(features[key]) != value for key, value in feature_hashes.items()): raise RuntimeError('Shared frozen features changed')
        old._frozen(torch, encoder, expected)
        if not bool(torch.equal(rng, torch.get_rng_state())): raise RuntimeError('Reader fit changed preserved CPU RNG')
        arm.update(final_reader_sha256=previous._weight_digest(reader), frozen_features_preserved=True,
                   evaluation_preserved=True, rng_preserved=True, seconds=time.perf_counter() - arm_start,
                   online_summary=dict(count=len(losses), loss_mean=sum(losses) / len(losses),
                       loss_min=min(losses), loss_max=max(losses), accuracy_mean=correct / (1008 * 16)))
        readers[name] = reader
    # Keep all three tiny readers, then create/evaluate final features only now.
    if any(a['updates'] != 1008 for a in report['arms']): raise RuntimeError('All reader fits must complete before final data')
    final = checked_split(config, data.FINAL_NAME)
    features['final'] = old._cache(torch, encoder, final)
    report['data_identity'][data.FINAL_NAME] = data.fingerprint(final)
    report['final_features_after_all_fits'] = True
    for arm in report['arms']:
        name = arm['name']; reader = readers[name]
        old._frozen(torch, reader, arm['final_reader_sha256'])
        arm['final_holdout'] = dict(evaluations=1, evaluated_after_updates=1008, all_reader_fits_complete=True,
            evaluation=_evaluate(torch, reader, name, features['final'], final, True))
        kind = 'direct' if name == 'direct_linear' else 'factorized'
        report['snapshots'].append(_save_model(reader, directory, name + '-final.f32', kind))
    if any(old._feature_hash(features[key]) != value for key, value in feature_hashes.items()): raise RuntimeError('Shared cache changed during holdout')
    old._frozen(torch, encoder, expected)
    report['feature_sha256'] = {key: old._feature_hash(value) for key, value in features.items()}
    report['source_encoder']['frozen_preserved'] = True
    report['timings']['total_seconds'] = time.perf_counter() - start


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--worker', action='store_true'); mode.add_argument('--validate-only', action='store_true')
    parser.add_argument('--config'); parser.add_argument('--model', choices=MODELS); parser.add_argument('--seed', type=int)
    parser.add_argument('--encoder-dir'); parser.add_argument('--checkpoint-dir')
    args = parser.parse_args(argv)
    if not args.worker and not args.validate_only:
        print(serialize_report(dict(status='prepared_unexecuted', native_executed=False, trained=False)))
        return 0
    config = read_config(args.config)
    if args.validate_only:
        print(serialize_report(validation_report(config))); return 0
    report = dict(schema=SCHEMA, status='incomplete', model=args.model, seed=args.seed, native_executed=False, cpu_only=False)
    try:
        run_worker(args.model, args.seed, config, args.encoder_dir, args.checkpoint_dir, report)
        report['status'] = 'complete'; validate_cell(report, config); validate_artifacts(report, args.checkpoint_dir)
    except Exception as exc:
        report.update(status='incomplete', failure=dict(type=type(exc).__name__, message=str(exc)[:2000]))
    print(serialize_report(report))
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
