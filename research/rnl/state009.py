"""STATE-009 frozen pre-query GRU-state readout probe, prepared only.

Default/import/validation are stdlib-only. The explicit future --worker path
requires separate approval and external CPU/memory/time/disk supervision.
Only synthetic data is used. Checkpoints are exclusive bounded raw little-endian
FP32 files plus strict shape/hash metadata; no pickle or native loader is used.
"""
import argparse
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import time

import state006 as previous
import state006_data as old_data
import state007 as original
import state008 as replay
import state008_data as prior_data
import state009_data as data

SCHEMA = 'STATE-009-cell-v1'
MODELS, SEEDS = ('gru',), (11, 23, 37)
ARMS = ('trained_selected', 'trained_blind', 'initial_selected')
CURVES = (0, 168, 1008)
CHECKPOINTS = original.CHECKPOINTS
SNAPSHOTS = {'initial-encoder.f32': 'encoder', 'trained-encoder.f32': 'encoder',
             **{arm + '-reader.f32': 'reader' for arm in ARMS}}


def default_config():
    """Pure settings template. Frozen data fingerprints are filled by preparation."""
    with (Path(__file__).parent / 'STATE-008-config.json').open(encoding='utf-8') as stream:
        before = json.load(stream)
    return dict(schema='STATE-009-config-v1', models=list(MODELS), seeds=list(SEEDS),
        arms=list(ARMS), checkpoints=list(CHECKPOINTS), curves=list(CURVES),
        encoder_updates=1152, encoder_train_count=3072, updates=1008, batch=16,
        train_count=2688, epochs=6, train_blocks=list(data.TRAIN_NAMES),
        groups=168, count=672, evaluation_seed=9009,
        diagnostic_name=data.DIAGNOSTIC_NAME, final_name=data.FINAL_NAME,
        history_events=63, features=32, reader_parameters=1056, reader_macs=1024,
        reader_seed_base=900900, torch_version='2.15.0+cpu',
        signal_final_quartets=17, signal_final_individual=168, trained_feature_advantage=17,
        reliability_final_quartets=135, reliability_final_individual=605,
        reference_weights={str(s): before['reference_weights'][f'gru/{s}'] for s in SEEDS},
        reference_train=before['reference_train'],
        set_fingerprints={name: '0' * 64 for name in data.NAMES})


def validate_config(config):
    expected = default_config()
    if not isinstance(config, dict) or set(config) != set(expected):
        raise ValueError('Exact scientific configuration fields required')
    for key, value in expected.items():
        if key == 'set_fingerprints':
            if (not isinstance(config[key], dict) or set(config[key]) != set(data.NAMES)
                    or not all(replay._sha(x) for x in config[key].values())):
                raise ValueError('All six frozen dataset fingerprints required')
        elif not replay._same_typed(config[key], value):
            raise ValueError('Frozen scientific setting mismatch: ' + key)
    return config


def read_config(path):
    def unique(pairs):
        value = {}
        for k, v in pairs:
            if k in value: raise ValueError('Duplicate configuration key')
            value[k] = v
        return value
    with open(path, encoding='utf-8') as stream:
        value = json.load(stream, object_pairs_hook=unique,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite configuration')))
    return validate_config(value)


def reader_schedule():
    for epoch in range(6):
        for first in range(0, 2688, 16):
            yield epoch * 168 + first // 16 + 1, first, first + 16


def history_rows(episode):
    """Encode only history events; never inspect query/key/target metadata."""
    events = episode['events']
    if len(events) != 64 or events[-1][0] != 'QUERY':
        raise ValueError('Exactly63 history events plus final QUERY required')
    rows = []
    for kind, key, value in events[:-1]:
        if kind not in ('SET', 'NOOP'):
            raise ValueError('QUERY may not enter encoder history')
        row = [0.] * 15
        row[0 if kind == 'SET' else 1] = 1.
        if kind == 'SET':
            if type(key) is not int or not 0 <= key < 4 or type(value) is not int or not 0 <= value < 8:
                raise ValueError('Valid SET history required')
            row[3 + key], row[7 + value] = 1., 1.
        elif key is not None or value is not None:
            raise ValueError('NOOP must carry no key/value')
        rows.append(row)
    return rows


def history_index(episodes):
    unique, mapping, indices = [], {}, []
    for episode in episodes:
        history = tuple(tuple(x) for x in episode['events'][:-1])
        if history not in mapping:
            mapping[history] = len(unique)
            unique.append(history)
        indices.append(mapping[history])
    return unique, indices


def readout_reference(weights, bias, feature, key, blind):
    """Independent scalar oracle for the future native gather/mean check."""
    if (type(key) is not int or not 0 <= key < 4 or type(blind) is not bool
            or len(weights) != 32 or any(len(row) != 32 for row in weights)
            or len(bias) != 32 or len(feature) != 32):
        raise ValueError('Exact reader shapes/key required')
    flat = [b + sum(w * x for w, x in zip(row, feature)) for row, b in zip(weights, bias)]
    if not all(math.isfinite(x) for x in flat): raise ValueError('Finite reader output required')
    return [sum(flat[k * 8 + v] for k in range(4)) / 4 for v in range(8)] if blind else flat[key * 8:(key + 1) * 8]


def snapshot_layout(kind):
    if kind == 'reader':
        return {'heads.bias': [32], 'heads.weight': [32, 32]}
    if kind == 'encoder':
        return {'head.bias': [8], 'head.weight': [8, 32],
                'recurrent.bias_hh_l0': [96], 'recurrent.bias_ih_l0': [96],
                'recurrent.weight_hh_l0': [96, 32], 'recurrent.weight_ih_l0': [96, 15]}
    raise ValueError('Fixed snapshot kind required')


def snapshot_bytes(kind):
    return 4 * sum(math.prod(shape) for shape in snapshot_layout(kind).values())


def encode_snapshot(tensors, kind):
    layout = snapshot_layout(kind)
    if not isinstance(tensors, dict) or set(tensors) != set(layout):
        raise ValueError('Exact tensor-name set required')
    parts, inventory, offset = [], [], 0
    for name, shape in layout.items():
        actual, values = tensors[name]
        if (actual != shape or len(values) != math.prod(shape)
                or any(type(x) not in (int, float) or not math.isfinite(x) for x in values)):
            raise ValueError('Finite FP32 values and fixed shapes required')
        try: raw = struct.pack('<' + str(len(values)) + 'f', *values)
        except (OverflowError, struct.error) as exc: raise ValueError('FP32 range exceeded') from exc
        if not all(math.isfinite(x) for x in struct.unpack('<' + str(len(values)) + 'f', raw)):
            raise ValueError('Nonfinite encoded FP32')
        inventory.append(dict(name=name, shape=shape, offset_bytes=offset, bytes=len(raw)))
        parts.append(raw); offset += len(raw)
    return b''.join(parts), inventory


def decode_snapshot(raw, inventory, kind):
    if not isinstance(raw, bytes) or len(raw) != snapshot_bytes(kind):
        raise ValueError('Exact bounded raw FP32 payload required')
    expected, offset, tensors = [], 0, {}
    for name, shape in snapshot_layout(kind).items():
        count = math.prod(shape); size = count * 4
        expected.append(dict(name=name, shape=shape, offset_bytes=offset, bytes=size))
        values = list(struct.unpack('<' + str(count) + 'f', raw[offset:offset + size]))
        if not all(math.isfinite(x) for x in values): raise ValueError('Nonfinite checkpoint')
        tensors[name] = (shape, values); offset += size
    if not replay._same_typed(inventory, expected): raise ValueError('Fixed nonoverlapping inventory required')
    return tensors


def tensor_digest(raw, inventory):
    digest = hashlib.sha256()
    for row in inventory:
        digest.update(row['name'].encode() + b'\0')
        digest.update(raw[row['offset_bytes']:row['offset_bytes'] + row['bytes']])
    return digest.hexdigest()


def _plain_directory(directory):
    directory = Path(directory)
    if not directory.is_absolute() or not directory.is_dir(): raise ValueError('Existing absolute owned directory required')
    for item in (directory, *directory.parents):
        info = os.lstat(item)
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Reparse checkpoint path refused')
    return directory


def write_snapshot_bytes(directory, name, raw, inventory, kind):
    if SNAPSHOTS.get(name) != kind: raise ValueError('Fixed safe checkpoint filename required')
    decode_snapshot(raw, inventory, kind)
    destination = _plain_directory(directory) / name
    with destination.open('xb') as stream: stream.write(raw)
    record = dict(name=name, kind=kind, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                  parameter_sha256=tensor_digest(raw, inventory), tensors=inventory)
    verify_snapshot_file(directory, record)
    return record


def verify_snapshot_file(directory, record):
    if not isinstance(record, dict) or set(record) != {'name', 'kind', 'bytes', 'sha256', 'parameter_sha256', 'tensors'}:
        raise ValueError('Exact bounded checkpoint record required')
    name, kind = record['name'], record['kind']
    if SNAPSHOTS.get(name) != kind or record['bytes'] != snapshot_bytes(kind):
        raise ValueError('Fixed checkpoint name/kind/bytes required')
    directory = _plain_directory(directory)
    file = directory / name; info = os.lstat(file)
    if (not stat.S_ISREG(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400
            or info.st_size != record['bytes']): raise ValueError('Plain exact-size checkpoint required')
    raw = file.read_bytes()
    if hashlib.sha256(raw).hexdigest() != record['sha256'] or tensor_digest(raw, record['tensors']) != record['parameter_sha256']:
        raise ValueError('Checkpoint identity mismatch')
    return decode_snapshot(raw, record['tensors'], kind)


def checked_split(config, name):
    episodes = data.make_split(name, config['evaluation_seed'])
    if data.fingerprint(episodes) != config['set_fingerprints'][name]: raise ValueError('Frozen fresh data mismatch')
    data.validate_split(episodes)
    return episodes


def _metric_counts(metrics, groups=True):
    if not isinstance(metrics, dict): raise ValueError('Metrics object required')
    for name, expected in [('individual', 672), ('query_pairs', 336), ('value_pairs', 336), ('quartets', 168)]:
        row = metrics.get(name)
        if (not isinstance(row, dict) or set(row) != {'correct', 'count', 'accuracy'}
                or type(row['correct']) is not int or not 0 <= row['correct'] <= expected
                or type(row['count']) is not int or row['count'] != expected
                or type(row['accuracy']) not in (float, int) or row['accuracy'] != row['correct'] / expected):
            raise ValueError('Exact finite count/fraction required: ' + name)


def validate_evaluation(record, episodes, *, retain_rows, blind):
    if (not isinstance(record, dict) or set(record) != {'loss', 'seconds', 'preserved', 'metrics'}
            or not replay._number(record['loss']) or not replay._number(record['seconds']) or record['preserved'] is not True):
        raise ValueError('Finite noninvasive evaluation required')
    metrics = record['metrics']; _metric_counts(metrics)
    if retain_rows:
        rows = metrics.get('rows')
        if (not isinstance(rows, list) or len(rows) != len(episodes)
                or any(not isinstance(row, list) or len(row) != 8 or any(type(x) is not int for x in row) for row in rows)):
            raise ValueError('Exact integer prediction rows required')
        recomputed = data.prediction_metrics(episodes, [row[-1] for row in rows])
        if not replay._same_typed(metrics, recomputed):
            raise ValueError('Prediction identities/counts disagree')
    elif 'rows' in metrics:
        raise ValueError('Early/training-probe individual rows are outside fixed retained scope')
    else:
        baseline = data.prediction_metrics(episodes, [0] * len(episodes))
        if metrics.get('baselines') != baseline['baselines']: raise ValueError('Exact task references required')
    if blind:
        if metrics['query_pairs']['correct'] != 0 or metrics['quartets']['correct'] != 0:
            raise ValueError('Query-blind control solved an impossible group')
        if retain_rows:
            rows = metrics['rows']
            if any(rows[i][-1] != rows[i + 1][-1] for i in range(0, len(rows), 2)):
                raise ValueError('Query-blind control changed class for identical history')
    return record


def screen_scores(rows):
    if (not isinstance(rows, list) or len(rows) != 9
            or {(r.get('seed'), r.get('arm')) for r in rows} != {(s, a) for s in SEEDS for a in ARMS}):
        raise ValueError('Nine fixed paired arm scores required')
    by_seed, output = {}, []
    for row in rows:
        if (type(row['seed']) is not int or type(row.get('individual')) is not int
                or not 0 <= row['individual'] <= 672 or type(row.get('quartets')) is not int
                or not 0 <= row['quartets'] <= 168): raise ValueError('Valid count scores required')
        by_seed.setdefault(row['seed'], {})[row['arm']] = row
    for seed in SEEDS:
        trained, blind, initial = (by_seed[seed][a] for a in ARMS)
        if blind['quartets'] != 0: raise ValueError('Query-blind negative-control failure')
        output.append(dict(seed=seed, trained_signal=trained['quartets'] >= 17 and trained['individual'] >= 168,
            trained_feature_advantage=trained['quartets'] - initial['quartets'] >= 17,
            trained_reliability=trained['quartets'] >= 135 and trained['individual'] >= 605,
            trained_minus_initial_quartets=trained['quartets'] - initial['quartets']))
    return dict(status='complete', paired_seeds=output, all_trained_signal=all(r['trained_signal'] for r in output),
        all_trained_feature_advantage=all(r['trained_feature_advantage'] for r in output),
        all_trained_reliability=all(r['trained_reliability'] for r in output), candidate_for_revia=False,
        interpretation='Pragmatic descriptive screens; related groups/seed-runs dependent. Supplied query selection is not learned routing. No causal diagnosis.')


def validate_artifacts(cell, directory):
    records = cell.get('snapshots')
    if (not isinstance(records, list) or len(records) != 5
            or {r.get('name') for r in records} != set(SNAPSHOTS)):
        raise ValueError('Exactly five retained snapshots per seed required')
    for record in records: verify_snapshot_file(directory, record)
    expected = {'initial-encoder.f32': cell['encoder_replay']['initial_sha256'],
                'trained-encoder.f32': cell['encoder_replay']['trained_sha256'],
                **{a['name'] + '-reader.f32': a['final_reader_sha256'] for a in cell['arms']}}
    if any(r['parameter_sha256'] != expected[r['name']] for r in records):
        raise ValueError('Retained snapshot differs from evaluated model weights')
    if {p.name for p in Path(directory).iterdir()} != set(SNAPSHOTS):
        raise ValueError('Unexpected checkpoint artifacts')


def validate_cell(cell, config):
    validate_config(config)
    if (not isinstance(cell, dict) or cell.get('schema') != SCHEMA or cell.get('status') != 'complete'
            or cell.get('model') != 'gru' or type(cell.get('seed')) is not int or cell['seed'] not in SEEDS
            or cell.get('native_executed') is not True or cell.get('cpu_only') is not True
            or cell.get('torch_version') != config['torch_version'] or cell.get('encoder_capacity') != previous.capacity('gru')
            or cell.get('data_identity') != config['set_fingerprints']): raise ValueError('Complete fixed native seed record required')
    seed, enc = cell['seed'], cell['encoder_replay']
    refs = config['reference_weights'][str(seed)]
    if (enc.get('updates') != 1152 or enc.get('train_sha256') != config['reference_train'][str(seed)]
            or enc.get('initial_sha256') != refs['0'] or enc.get('trained_sha256') != refs['1152']
            or enc.get('frozen_preserved') is not True or len(enc.get('weight_checks', [])) != 8):
        raise ValueError('Exact frozen original encoder replay required')
    for row, update in zip(enc['weight_checks'], CHECKPOINTS):
        if row != dict(updates=update, expected=refs[str(update)], observed=refs[str(update)], matched=True):
            raise ValueError('Exact original trajectory hash required')
    arms = cell.get('arms')
    if not isinstance(arms, list) or [a.get('name') for a in arms] != list(ARMS): raise ValueError('Three fixed reader arms required')
    diagnostic, final = checked_split(config, data.DIAGNOSTIC_NAME), checked_split(config, data.FINAL_NAME)
    training_probe = checked_split(config, data.TRAIN_NAMES[0])
    initial_readers = set()
    for arm in arms:
        name = arm['name']; blind = name == 'trained_blind'
        if (arm.get('updates') != 1008 or arm.get('parameters') != 1056 or arm.get('main_forward_macs') != 1024
                or arm.get('frozen_features_preserved') is not True or arm.get('evaluation_preserved') is not True
                or not replay._sha(arm.get('initial_reader_sha256')) or not replay._sha(arm.get('final_reader_sha256'))
                or arm.get('selection') != ('mean_query_blind' if blind else 'supplied_observable_key')):
            raise ValueError('Complete fixed readout fit required')
        initial_readers.add(arm['initial_reader_sha256'])
        curves = arm.get('curves')
        if not isinstance(curves, list) or [r.get('updates') for r in curves] != list(CURVES):
            raise ValueError('All fixed learning-curve records required')
        for curve in curves:
            validate_evaluation(curve['training_probe'], training_probe, retain_rows=False, blind=blind)
            validate_evaluation(curve['diagnostic'], diagnostic, retain_rows=curve['updates'] == 1008, blind=blind)
        holdout = arm.get('final_holdout')
        if holdout.get('evaluations') != 1 or holdout.get('evaluated_after_updates') != 1008:
            raise ValueError('Final evaluation exactly once at fixed final update required')
        validate_evaluation(holdout['evaluation'], final, retain_rows=True, blind=blind)
    if len(initial_readers) != 1: raise ValueError('All three readers must start from identical tensors')
    return cell


def assess(cells, config):
    if not isinstance(cells, list) or len(cells) != 3 or {c.get('seed') for c in cells} != set(SEEDS):
        raise ValueError('Three complete paired seed workers required')
    rows = []
    for cell in cells:
        validate_cell(cell, config)
        for arm in cell['arms']:
            metrics = arm['final_holdout']['evaluation']['metrics']
            rows.append(dict(seed=cell['seed'], arm=arm['name'],
                             individual=metrics['individual']['correct'], quartets=metrics['quartets']['correct']))
    return screen_scores(rows)


def validation_report(config):
    validate_config(config)
    splits = [checked_split(config, name) for name in data.NAMES]
    references = []
    for seed in SEEDS:
        pool = original.training_pool(seed)
        if original.episode_digest(pool) != config['reference_train'][str(seed)]: raise ValueError('Original training identity mismatch')
        references.extend(pool)
    before = replay.read_config(Path(__file__).parent / 'STATE-008-config.json')
    references.extend(e for name in prior_data.NAMES for e in prior_data.make_split(name))
    with (Path(__file__).parent / 'STATE-007-config.json').open(encoding='utf-8') as stream:
        cfg7 = original.validate_config(json.load(stream))
    references.extend(original.diagnostic_split(cfg7))
    references.extend(original.final_holdout(cfg7, 1152))
    disjoint = data.validate_disjoint(splits, iter(references))
    return dict(schema='STATE-009-preparation-v1', status='prepared_native_unverified', native_executed=False,
        trained=False, data={name: data.validate_split(e) for name, e in zip(data.NAMES, splits)},
        disjoint=disjoint, original_weight_hashes_future=24, head_fits_future=9,
        retained_prediction_rows_future=12096, snapshot_parameter_bytes_future=157248,
        native_validation='Not executed; exact guarded approval required')


def _model_tensors(model):
    tensors = {}
    for name, tensor in model.state_dict().items():
        if str(tensor.dtype) != 'torch.float32' or tensor.device.type != 'cpu':
            raise RuntimeError('CPUFP32 checkpoint tensors required')
        tensors[name] = (list(tensor.shape), tensor.detach().reshape(-1).tolist())
    return tensors


def _save_model(model, directory, name, kind):
    raw, inventory = encode_snapshot(_model_tensors(model), kind)
    return write_snapshot_bytes(directory, name, raw, inventory, kind)


def _freeze(model):
    model.eval()
    for parameter in model.parameters(): parameter.requires_grad_(False); parameter.grad = None


def _frozen(torch, model, expected):
    if (model.training or previous._weight_digest(model) != expected
            or any(p.requires_grad or p.grad is not None for p in model.parameters())):
        raise RuntimeError('Frozen encoder weights/mode/gradients changed')


def _cache(torch, model, episodes):
    """One recurrent evaluation per unique history, zero state every history."""
    expected, rng = previous._weight_digest(model), torch.get_rng_state().clone()
    _frozen(torch, model, expected)
    unique, indices = history_index(episodes)
    parts = []
    with torch.no_grad():
        for first in range(0, len(unique), 16):
            encoded = [history_rows({'events': [*history, ('QUERY', 0, None)]}) for history in unique[first:first + 16]]
            inputs = torch.tensor(encoded, dtype=torch.float32, device='cpu')
            zero = inputs.new_zeros((1, inputs.shape[0], 32))
            _, state = model.recurrent(inputs, zero)
            if state.shape != (1, len(encoded), 32) or not bool(torch.isfinite(state).all()):
                raise RuntimeError('Finite exact pre-QUERY feature shape required')
            parts.append(state[0].detach().clone())
        features = torch.cat(parts, dim=0)[torch.tensor(indices, dtype=torch.long)].detach()
    if features.requires_grad or features.grad_fn is not None: raise RuntimeError('Detached features required')
    if not all(bool(torch.equal(features[i], features[i + 1])) for i in range(0, len(episodes), 2)):
        raise RuntimeError('Query variants did not reuse identical history features')
    _frozen(torch, model, expected)
    if not bool(torch.equal(rng, torch.get_rng_state())): raise RuntimeError('Feature cache changed CPU RNG')
    return features


def _feature_hash(features):
    values = features.detach().reshape(-1).tolist()
    return hashlib.sha256(struct.pack('<' + str(len(values)) + 'f', *values)).hexdigest()


def _head_logits(reader, features, keys, blind):
    logits = reader.heads(features).reshape(-1, 4, 8)
    return logits.mean(dim=1) if blind else logits.gather(1, keys.reshape(-1, 1, 1).expand(-1, 1, 8)).squeeze(1)


def _evaluate(torch, reader, features, episodes, blind, retain_rows):
    tick = time.perf_counter()
    training, weights, rng = reader.training, previous._weight_digest(reader), torch.get_rng_state().clone()
    predictions, loss_sum = [], 0.
    try:
        reader.eval()
        with torch.no_grad():
            for first in range(0, len(episodes), 16):
                batch = episodes[first:first + 16]
                keys = torch.tensor([e['events'][-1][1] for e in batch], dtype=torch.long)
                targets = torch.tensor([e['target'] for e in batch], dtype=torch.long)
                logits = _head_logits(reader, features[first:first + 16], keys, blind)
                loss = torch.nn.functional.cross_entropy(logits, targets, reduction='sum')
                if not bool(torch.isfinite(logits).all()) or not bool(torch.isfinite(loss)):
                    raise RuntimeError('Finite reader evaluation required')
                if blind and not all(bool(torch.equal(logits[i], logits[i + 1])) for i in range(0, len(batch), 2)):
                    raise RuntimeError('Query-blind logits changed for identical features')
                predictions.extend(logits.argmax(dim=1).tolist()); loss_sum += loss.item()
    finally: reader.train(training)
    if previous._weight_digest(reader) != weights or not bool(torch.equal(rng, torch.get_rng_state())):
        raise RuntimeError('Reader evaluation changed weights or RNG')
    metrics = data.prediction_metrics(episodes, predictions)
    if not retain_rows: del metrics['rows']
    record = dict(loss=loss_sum / len(episodes), seconds=time.perf_counter() - tick, preserved=True, metrics=metrics)
    validate_evaluation(record, episodes, retain_rows=retain_rows, blind=blind)
    return record


def _new_reader(torch, seed):
    rng = torch.get_rng_state().clone()
    try:
        torch.default_generator.manual_seed(900900 + seed)
        class Reader(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.heads = torch.nn.Linear(32, 32, device='cpu', dtype=torch.float32)
        return Reader()
    finally: torch.set_rng_state(rng)


def run_worker(model_name, seed, config, directory, report):
    validate_config(config)
    if model_name != 'gru' or type(seed) is not int or seed not in SEEDS: raise ValueError('Fixed seed/model required')
    directory = _plain_directory(directory)
    if any(directory.iterdir()): raise ValueError('Fresh empty checkpoint directory required')
    started = time.perf_counter()
    report.update(encoder_replay=dict(updates=0, weight_checks=[]), arms=[], snapshots=[], data_identity={}, timings={})
    pool = original.training_pool(seed)
    if original.episode_digest(pool) != config['reference_train'][str(seed)]: raise ValueError('Original training pool mismatch')
    train_blocks = [checked_split(config, name) for name in data.TRAIN_NAMES]
    diagnostic = checked_split(config, data.DIAGNOSTIC_NAME)
    report['data_identity'].update({name: config['set_fingerprints'][name] for name in (*data.TRAIN_NAMES, data.DIAGNOSTIC_NAME)})
    report['native_executed'] = True
    torch, model = previous._native_setup('gru', seed)
    if torch.__version__ != config['torch_version']: raise RuntimeError('Exact existing CPU framework required')
    initial = deepcopy(model)
    refs = config['reference_weights'][str(seed)]
    report.update(cpu_only=True, torch_version=torch.__version__, encoder_capacity=previous.capacity('gru'))
    enc = report['encoder_replay']; enc['train_sha256'] = original.episode_digest(pool)
    def checkpoint(update):
        observed = previous._weight_digest(model); expected = refs[str(update)]
        enc['weight_checks'].append(dict(updates=update, expected=expected, observed=observed, matched=observed == expected))
        if observed != expected: raise RuntimeError('Original GRU trajectory mismatch at update' + str(update))
    checkpoint(0); enc['initial_sha256'] = previous._weight_digest(initial)
    optimizer = torch.optim.Adam(model.parameters(), lr=.003, weight_decay=0)
    for update, first, last in original.batch_schedule():
        model.train(); inputs, targets = previous._batch(torch, pool[first:last])
        optimizer.zero_grad(set_to_none=True)
        logits = model(inputs); loss = torch.nn.functional.cross_entropy(logits, targets)
        if not bool(torch.isfinite(logits).all()) or not bool(torch.isfinite(loss)): raise RuntimeError('Nonfinite replay')
        loss.backward(); torch.nn.utils.clip_grad_norm_(model.parameters(), 1., error_if_nonfinite=True)
        optimizer.step(); enc['updates'] = update
        if update in CHECKPOINTS: checkpoint(update)
    enc['trained_sha256'] = previous._weight_digest(model)
    del optimizer
    _freeze(model); _freeze(initial)
    report['snapshots'].extend([_save_model(initial, directory, 'initial-encoder.f32', 'encoder'),
                                _save_model(model, directory, 'trained-encoder.f32', 'encoder')])
    report['timings']['encoder_replay_and_setup_seconds'] = time.perf_counter() - started
    training = [e for block in train_blocks for e in block]
    feature_sets = {'trained': {}, 'initial': {}}
    encoders = {'trained': model, 'initial': initial}
    for which, encoder in encoders.items():
        feature_sets[which]['train'] = _cache(torch, encoder, training)
        feature_sets[which]['diagnostic'] = _cache(torch, encoder, diagnostic)
    report['timings']['pre_final_feature_cache_seconds'] = time.perf_counter() - started - report['timings']['encoder_replay_and_setup_seconds']
    template = _new_reader(torch, seed)
    initial_reader_hash = previous._weight_digest(template)
    final = None
    for name in ARMS:
        arm_started = time.perf_counter(); which = 'initial' if name == 'initial_selected' else 'trained'
        blind = name == 'trained_blind'; encoder = encoders[which]; features = feature_sets[which]
        weights_before = previous._weight_digest(encoder)
        rng_before = torch.get_rng_state().clone()
        hashes_before = {key: _feature_hash(value) for key, value in features.items()}
        reader = deepcopy(template)
        if previous._weight_digest(reader) != initial_reader_hash: raise RuntimeError('Unequal initial readers')
        if sum(p.numel() for p in reader.parameters()) != 1056: raise RuntimeError('Reader capacity mismatch')
        # The scalar oracle exercises supplied-key selection/mean before fitting.
        with torch.no_grad():
            for key in range(4):
                native = _head_logits(reader, features['train'][:1], torch.tensor([key]), blind)[0].tolist()
                reference = readout_reference(reader.heads.weight.tolist(), reader.heads.bias.tolist(), features['train'][0].tolist(), key, blind)
                if any(abs(a - b) > 1e-5 for a, b in zip(native, reference)):
                    raise RuntimeError('Native reader differs from scalar selection/mean oracle')
        arm = dict(name=name, updates=0, parameters=1056, main_forward_macs=1024,
            selection='mean_query_blind' if blind else 'supplied_observable_key',
            encoder=which, initial_reader_sha256=initial_reader_hash, curves=[], final_holdout=None)
        report['arms'].append(arm)
        opt = torch.optim.Adam(reader.parameters(), lr=.003, weight_decay=0)
        def curve(update):
            arm['curves'].append(dict(updates=update,
                training_probe=_evaluate(torch, reader, features['train'][:672], train_blocks[0], blind, False),
                diagnostic=_evaluate(torch, reader, features['diagnostic'], diagnostic, blind, update == 1008)))
        curve(0); losses, accuracies = [], []
        for update, first, last in reader_schedule():
            reader.train(); opt.zero_grad(set_to_none=True)
            batch = training[first:last]
            keys = torch.tensor([e['events'][-1][1] for e in batch], dtype=torch.long)
            targets = torch.tensor([e['target'] for e in batch], dtype=torch.long)
            logits = _head_logits(reader, features['train'][first:last], keys, blind)
            loss = torch.nn.functional.cross_entropy(logits, targets)
            if not bool(torch.isfinite(logits).all()) or not bool(torch.isfinite(loss)): raise RuntimeError('Nonfinite reader fit')
            losses.append(loss.item()); accuracies.append(int((logits.argmax(dim=1) == targets).sum().item()) / 16)
            loss.backward(); torch.nn.utils.clip_grad_norm_(reader.parameters(), 1., error_if_nonfinite=True); opt.step()
            arm['updates'] = update
            if update in CURVES: curve(update)
        if final is None:
            final = checked_split(config, data.FINAL_NAME)
            report['data_identity'][data.FINAL_NAME] = data.fingerprint(final)
        if 'final' not in features: features['final'] = _cache(torch, encoder, final)
        arm['final_holdout'] = dict(evaluations=1, evaluated_after_updates=1008,
            evaluation=_evaluate(torch, reader, features['final'], final, blind, True))
        arm['online_summary'] = dict(count=len(losses), loss_mean=sum(losses) / len(losses),
            loss_min=min(losses), loss_max=max(losses), accuracy_mean=sum(accuracies) / len(accuracies))
        arm['final_reader_sha256'] = previous._weight_digest(reader)
        report['snapshots'].append(_save_model(reader, directory, name + '-reader.f32', 'reader'))
        _frozen(torch, encoder, weights_before)
        if any(_feature_hash(features[key]) != value for key, value in hashes_before.items()):
            raise RuntimeError('Frozen feature cache changed while fitting reader')
        if not bool(torch.equal(rng_before, torch.get_rng_state())):
            raise RuntimeError('Reader fitting changed the preserved CPU RNG stream')
        arm.update(frozen_features_preserved=True, evaluation_preserved=True, seconds=time.perf_counter() - arm_started,
                   feature_sha256={key: _feature_hash(value) for key, value in features.items()})
        del opt, reader
    _frozen(torch, model, refs['1152']); _frozen(torch, initial, refs['0'])
    enc['frozen_preserved'] = True
    report['timings']['total_seconds'] = time.perf_counter() - started


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--worker', action='store_true'); mode.add_argument('--validate-only', action='store_true')
    parser.add_argument('--config'); parser.add_argument('--model', choices=MODELS); parser.add_argument('--seed', type=int)
    parser.add_argument('--checkpoint-dir')
    args = parser.parse_args(argv)
    if not args.worker and not args.validate_only:
        print('{"status":"prepared_unexecuted","native_executed":false,"trained":false}')
        return 0
    config = read_config(args.config)
    if args.validate_only:
        print(json.dumps(validation_report(config), sort_keys=True, allow_nan=False)); return 0
    report = dict(schema=SCHEMA, status='incomplete', model=args.model, seed=args.seed, native_executed=False, cpu_only=False)
    try:
        run_worker(args.model, args.seed, config, args.checkpoint_dir, report)
        report['status'] = 'complete'; validate_cell(report, config); validate_artifacts(report, args.checkpoint_dir)
    except Exception as exc:
        report.update(status='incomplete', failure=dict(type=type(exc).__name__, message=str(exc)[:2000]))
    print(json.dumps(report, sort_keys=True, allow_nan=False))
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
