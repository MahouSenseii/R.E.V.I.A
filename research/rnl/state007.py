"""STATE-007 fixed-data learning curves; preparation is stdlib-only.

Only --worker imports the reviewed STATE-006 native setup. It needs separate
execution approval and an external resource supervisor. This worker does not
launch processes, access a network, write weights, or enforce OS resource caps.
"""
import argparse
import hashlib
import json
import math
import re
import time

import state006 as previous
import state006_data as data

SCHEMA = 'STATE-007-cell-v1'
MODELS, SEEDS = ('rnn', 'gru'), (11, 23, 37)
CHECKPOINTS = (0, 32, 64, 128, 192, 384, 768, 1152)
UPDATES, BATCH, POOL_COUNT, PROBE_COUNT, EVAL_COUNT = 1152, 16, 3072, 128, 512


def _number(value, lo=0., hi=None):
    return (type(value) in (int, float) and math.isfinite(value) and value >= lo
            and (hi is None or value <= hi))


def _sha(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def validate_config(config):
    if not isinstance(config, dict):
        raise ValueError('Configuration object required')
    fixed = dict(schema='STATE-007-config-v1', models=list(MODELS), seeds=list(SEEDS),
                 checkpoints=list(CHECKPOINTS), updates=UPDATES, batch=BATCH,
                 train_count=POOL_COUNT, epochs=6, probe_count=PROBE_COUNT,
                 diagnostic_count=EVAL_COUNT, final_count=EVAL_COUNT, lag_range=[1, 8],
                 torch_version='2.15.0+cpu', diagnostic_name='state007_diagnostic_A',
                 final_name='state007_final_A', evaluation_seed=7007,
                 improvement_threshold=.10, train_threshold=.95, final_threshold=.90)
    if any(type(config.get(key)) is not type(value) or config[key] != value
           for key, value in fixed.items()):
        raise ValueError('Frozen scientific configuration mismatch')
    if any(type(item) is not int for field in ('seeds', 'checkpoints', 'lag_range') for item in config[field]):
        raise ValueError('Integer scientific sequences required')
    references = config.get('reference_A_sha256')
    keys = {f'{model}/{seed}' for model in MODELS for seed in SEEDS}
    if not isinstance(references, dict) or set(references) != keys or not all(map(_sha, references.values())):
        raise ValueError('Six original checkpoint-A hashes required')
    return config


def read_config(path):
    with open(path, encoding='utf-8') as stream:
        value = json.load(stream, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite configuration')))
    return validate_config(value)


def batch_schedule():
    """Six identical full passes; preserve the original first 192 batches."""
    for update in range(1, UPDATES + 1):
        offset = ((update - 1) % (POOL_COUNT // BATCH)) * BATCH
        yield update, offset, offset + BATCH


def training_pool(seed):
    if type(seed) is not int or seed not in SEEDS:
        raise ValueError('Frozen seed required')
    name = f'train_A_{seed}'
    return data.make_split(name, **data.split_specs()[name])


def select_probe(pool):
    """First four examples per key/value, retaining original pool order."""
    selected, counts = [], {(key, value): 0 for key in range(4) for value in range(8)}
    for episode in pool:
        pair = episode['query_key'], data.oracle(episode['events'])
        if counts[pair] < 4:
            selected.append(episode)
            counts[pair] += 1
    if len(selected) != PROBE_COUNT or set(counts.values()) != {4}:
        raise ValueError('Balanced 128-example training-pool probe unavailable')
    return selected


def episode_digest(episodes):
    digest = hashlib.sha256()
    for episode in episodes:
        digest.update(json.dumps(episode, sort_keys=True, separators=(',', ':')).encode() + b'\n')
    return digest.hexdigest()


def diagnostic_split(config):
    return data.make_split(config['diagnostic_name'], EVAL_COUNT, (1, 8), config['evaluation_seed'])


def final_holdout(config, updates):
    if type(updates) is not int or updates != UPDATES:
        raise RuntimeError('Final holdout is unavailable before the fixed final update')
    return data.make_split(config['final_name'], EVAL_COUNT, (1, 8), config['evaluation_seed'])


def require_disjoint(*groups):
    seen = set()
    for group in groups:
        identities = {episode['seed'] for episode in group}
        if len(identities) != len(group) or not seen.isdisjoint(identities):
            raise ValueError('Training/evaluation group overlap or duplicate')
        seen.update(identities)


def baseline_records(episodes):
    """Non-neural controls: query-key rule and independent event replay."""
    correct, oracle_correct = 0, 0
    for episode in episodes:
        memory = {}
        for kind, key, value in episode['events']:
            if kind == 'SET':
                memory[key] = value
            elif kind == 'QUERY':
                predicted = memory[key]
                query_key = key
        target = data.oracle(episode['events'])
        correct += query_key == target  # Deterministic key -> same-number value.
        oracle_correct += predicted == target
    count = len(episodes)
    if count <= 0 or correct * 8 != count or oracle_correct != count:
        raise ValueError('Balanced query-key/oracle controls failed')
    return dict(count=count, uniform_expected_accuracy=.125,
                key_only_correct=correct, key_only_accuracy=correct / count,
                dictionary_oracle_correct=oracle_correct, dictionary_oracle_accuracy=oracle_correct / count)


def _evaluate(torch, model, episodes):
    training, weights, rng = model.training, previous._weight_digest(model), torch.get_rng_state().clone()
    correct, loss_sum = 0, 0.
    try:
        model.eval()
        with torch.no_grad():
            for offset in range(0, len(episodes), BATCH):
                x, y = previous._batch(torch, episodes[offset:offset + BATCH])
                logits = model(x)
                if not bool(torch.isfinite(logits).all()):
                    raise RuntimeError('Nonfinite diagnostic logits')
                loss = torch.nn.functional.cross_entropy(logits, y, reduction='sum')
                if not bool(torch.isfinite(loss)):
                    raise RuntimeError('Nonfinite diagnostic loss')
                loss_sum += loss.item()
                correct += int((logits.argmax(dim=1) == y).sum().item())
    finally:
        model.train(training)
    if weights != previous._weight_digest(model) or not bool(torch.equal(rng, torch.get_rng_state())):
        raise RuntimeError('Evaluation changed weights or CPU RNG')
    if model.training != training or not _number(loss_sum):
        raise RuntimeError('Evaluation changed mode or produced invalid loss')
    return dict(correct=correct, count=len(episodes), accuracy=correct / len(episodes), loss=loss_sum / len(episodes))


def run_worker(model_name, seed, config, report):
    validate_config(config)
    if model_name not in MODELS or type(seed) is not int or seed not in SEEDS:
        raise ValueError('Frozen model/seed required')
    started = time.perf_counter()
    report.update(training=dict(updates=0, unique_training_episodes=POOL_COUNT, passes=6), curves=[],
                  online=dict(loss=[], accuracy=[], update_seconds=[]), final_holdout=None,
                  reference_A_matches=False, reference_A_sha256=None,
                  evaluation_preserved_weights_and_rng=False)
    torch, model = previous._native_setup(model_name, seed)
    report.update(native_executed=True, cpu_only=True, torch_version=torch.__version__,
                  capacity=previous.capacity(model_name))
    if torch.__version__ != config['torch_version']:
        raise RuntimeError('Exact pinned CPU framework required')
    optimizer = torch.optim.Adam(model.parameters(), lr=.003, weight_decay=0)
    pool = training_pool(seed)
    probe, diagnostic = select_probe(pool), diagnostic_split(config)
    require_disjoint(pool, diagnostic)
    identity = dict(train=episode_digest(pool), probe=episode_digest(probe), diagnostic=episode_digest(diagnostic), final=None)
    report['data_identity'] = identity
    report['baselines'] = dict(training_probe=baseline_records(probe),
                               diagnostic=baseline_records(diagnostic), final_holdout=None)
    report['timings'] = dict(setup_seconds=time.perf_counter() - started)

    def checkpoint(updates):
        evaluation_started = time.perf_counter()
        weights = previous._weight_digest(model)
        if updates == 192:
            report['reference_A_sha256'] = weights
            if weights != config['reference_A_sha256'][f'{model_name}/{seed}']:
                raise RuntimeError('Original checkpoint-A hash mismatch; no extension')
            report['reference_A_matches'] = True
        row = dict(updates=updates, weight_sha256=weights,
                   training_probe=_evaluate(torch, model, probe),
                   diagnostic=_evaluate(torch, model, diagnostic),
                   evaluation_seconds=time.perf_counter() - evaluation_started)
        report['curves'].append(row)

    checkpoint(0)
    optimization_started = time.perf_counter()
    evaluation_seconds = report['curves'][0]['evaluation_seconds']
    for update, first, last in batch_schedule():
        tick = time.perf_counter()
        model.train()
        x, y = previous._batch(torch, pool[first:last])
        optimizer.zero_grad(set_to_none=True)
        logits = model(x)
        loss = torch.nn.functional.cross_entropy(logits, y)
        if not bool(torch.isfinite(loss)) or not bool(torch.isfinite(logits).all()):
            raise RuntimeError('Nonfinite training loss/logits')
        online_loss = loss.item()
        online_accuracy = int((logits.argmax(dim=1) == y).sum().item()) / BATCH
        loss.backward()
        norm = torch.nn.utils.clip_grad_norm_(model.parameters(), 1., error_if_nonfinite=True)
        if not bool(torch.isfinite(norm)):
            raise RuntimeError('Nonfinite gradient norm')
        optimizer.step()
        report['training']['updates'] = update
        report['online']['loss'].append(online_loss)
        report['online']['accuracy'].append(online_accuracy)
        report['online']['update_seconds'].append(time.perf_counter() - tick)
        if update in CHECKPOINTS:
            checkpoint(update)
            evaluation_seconds += report['curves'][-1]['evaluation_seconds']
    report['timings']['training_and_curve_seconds'] = time.perf_counter() - optimization_started
    report['timings']['curve_evaluation_seconds'] = evaluation_seconds
    if report['training']['updates'] != UPDATES or not report['reference_A_matches']:
        raise RuntimeError('Fixed update/reference control incomplete')
    holdout = final_holdout(config, report['training']['updates'])
    require_disjoint(pool, diagnostic, holdout)
    identity['final'] = episode_digest(holdout)
    report['baselines']['final_holdout'] = baseline_records(holdout)
    report['final_holdout'] = dict(_evaluate(torch, model, holdout), evaluated_after_updates=UPDATES, evaluations=1)
    if identity['train'] != episode_digest(pool) or identity['probe'] != episode_digest(probe):
        raise RuntimeError('Training pool/probe changed')
    report['evaluation_preserved_weights_and_rng'] = True
    report['timings']['total_seconds'] = time.perf_counter() - started
    report['status'] = 'complete'
    validate_cell(report, config)
    return report


def _validate_metric(value, count):
    if (not isinstance(value, dict) or type(value.get('count')) is not int or value['count'] != count
            or type(value.get('correct')) is not int or not 0 <= value['correct'] <= count
            or not _number(value.get('accuracy'), 0., 1.) or not _number(value.get('loss'))
            or value['accuracy'] != value['correct'] / count):
        raise ValueError('Finite reconciled accuracy/loss record required')


def _validate_baseline(value, count):
    expected = dict(count=count, uniform_expected_accuracy=.125, key_only_correct=count // 8,
                    key_only_accuracy=.125, dictionary_oracle_correct=count, dictionary_oracle_accuracy=1.)
    if (not isinstance(value, dict) or value != expected
            or any(type(value.get(key)) is not int for key in ('count', 'key_only_correct', 'dictionary_oracle_correct'))
            or not all(_number(value.get(key), 0., 1.) for key in
                       ('uniform_expected_accuracy', 'key_only_accuracy', 'dictionary_oracle_accuracy'))):
        raise ValueError('Exact query-key/oracle baseline controls required')


def validate_cell(cell, config):
    validate_config(config)
    if not isinstance(cell, dict):
        raise ValueError('Cell object required')
    model, seed = cell.get('model'), cell.get('seed')
    if (model not in MODELS or type(seed) is not int or seed not in SEEDS
            or cell.get('schema') != SCHEMA or cell.get('status') != 'complete'
            or cell.get('native_executed') is not True or cell.get('cpu_only') is not True
            or cell.get('torch_version') != config['torch_version']
            or cell.get('capacity') != previous.capacity(model)
            or cell.get('training') != dict(updates=UPDATES, unique_training_episodes=POOL_COUNT, passes=6)
            or cell.get('reference_A_matches') is not True
            or cell.get('reference_A_sha256') != config['reference_A_sha256'][f'{model}/{seed}']
            or cell.get('evaluation_preserved_weights_and_rng') is not True):
        raise ValueError('Cell identity/fixed-work/reference control mismatch')
    curves = cell.get('curves')
    if (not isinstance(curves, list) or len(curves) != len(CHECKPOINTS)
            or not all(isinstance(row, dict) and type(row.get('updates')) is int for row in curves)
            or [row['updates'] for row in curves] != list(CHECKPOINTS)):
        raise ValueError('Exact ordered checkpoint curve required')
    for row in curves:
        if not _sha(row.get('weight_sha256')) or not _number(row.get('evaluation_seconds')):
            raise ValueError('Checkpoint identity/timing invalid')
        _validate_metric(row.get('training_probe'), PROBE_COUNT)
        _validate_metric(row.get('diagnostic'), EVAL_COUNT)
        if row['updates'] == 192 and row['weight_sha256'] != cell['reference_A_sha256']:
            raise ValueError('Checkpoint-A curve hash mismatch')
    online = cell.get('online')
    if not isinstance(online, dict):
        raise ValueError('Online curve object required')
    for field, maximum in [('loss', None), ('accuracy', 1.), ('update_seconds', None)]:
        values = online.get(field)
        if not isinstance(values, list) or len(values) != UPDATES or not all(_number(v, 0., maximum) for v in values):
            raise ValueError('Exact finite per-update record required')
    final = cell.get('final_holdout')
    _validate_metric(final, EVAL_COUNT)
    if type(final.get('evaluations')) is not int or final['evaluations'] != 1 or type(final.get('evaluated_after_updates')) is not int or final['evaluated_after_updates'] != UPDATES:
        raise ValueError('Final holdout gate mismatch')
    baselines = cell.get('baselines')
    if not isinstance(baselines, dict):
        raise ValueError('Baseline control records required')
    for label, count in [('training_probe', PROBE_COUNT), ('diagnostic', EVAL_COUNT), ('final_holdout', EVAL_COUNT)]:
        _validate_baseline(baselines.get(label), count)
    identity = cell.get('data_identity')
    if not isinstance(identity, dict) or set(identity) != {'train', 'probe', 'diagnostic', 'final'} or not all(map(_sha, identity.values())):
        raise ValueError('Complete data identities required')
    timing = cell.get('timings')
    if not isinstance(timing, dict) or not all(_number(timing.get(key)) for key in
            ('setup_seconds', 'training_and_curve_seconds', 'curve_evaluation_seconds', 'total_seconds')):
        raise ValueError('Finite cell timing fields required')


def assess(cells, config):
    """Separate progress/learnability screens, never application promotion."""
    validate_config(config)
    valid, failures = {}, []
    for cell in cells if isinstance(cells, list) else []:
        try:
            validate_cell(cell, config)
            key = cell['model'], cell['seed']
            if key in valid:
                raise ValueError('Duplicate cell')
            valid[key] = cell
        except (ValueError, TypeError, KeyError) as error:
            failures.append(str(error))
    expected = {(model, seed) for model in MODELS for seed in SEEDS}
    missing = [f'{model}/{seed}' for model, seed in sorted(expected - valid.keys())]
    if not isinstance(cells, list):
        failures.append('Cell list required')
    if valid:
        for label in ('diagnostic', 'final'):
            if len({cell['data_identity'][label] for cell in valid.values()}) != 1:
                failures.append('Shared evaluation identity mismatch')
        for seed in SEEDS:
            pair = [valid[model, seed] for model in MODELS if (model, seed) in valid]
            for label in ('train', 'probe'):
                if len({cell['data_identity'][label] for cell in pair}) > 1:
                    failures.append('Paired training/probe identity mismatch')
    model_results = {}
    for model in MODELS:
        rows = []
        for seed in SEEDS:
            if (model, seed) not in valid:
                continue
            cell = valid[model, seed]
            baseline = next(row for row in cell['curves'] if row['updates'] == 192)
            last = cell['curves'][-1]
            delta = last['diagnostic']['correct'] - baseline['diagnostic']['correct']
            rows.append(dict(seed=seed, diagnostic_correct_at_192=baseline['diagnostic']['correct'],
                             diagnostic_correct_final=last['diagnostic']['correct'], additional_correct=delta,
                             accuracy_gain=delta / EVAL_COUNT, observed_increase=delta > 0,
                             material_gain=delta >= 52, train_probe_correct=last['training_probe']['correct'],
                             final_holdout_correct=cell['final_holdout']['correct'],
                             basic_learnability=last['training_probe']['correct'] >= 122 and cell['final_holdout']['correct'] >= 461))
        complete = len(rows) == len(SEEDS)
        model_results[model] = dict(seeds=rows,
                                   observed_all_seed_increase=complete and all(row['observed_increase'] for row in rows),
                                   material_gain_screen=complete and all(row['material_gain'] for row in rows),
                                   basic_learnability_screen=complete and all(row['basic_learnability'] for row in rows))
    return dict(schema='STATE-007-assessment-v1', status='incomplete' if failures or missing else 'complete',
                failures=failures, missing=missing, models=model_results, candidate_for_revia=False,
                baselines=dict(uniform_query=.125, deterministic_key_only=.125, dictionary_oracle=1.),
                interpretation='Fixed-data repeated-update screen; no significance, superiority, B/OOD or integration claim')


def validation_report(config=None):
    if config is not None:
        validate_config(config)
    return dict(schema='STATE-007-preparation-v1', status='prepared_native_unverified', native_executed=False,
                configuration_supplied=config is not None, checkpoints=list(CHECKPOINTS),
                updates=UPDATES, batch=BATCH, unique_training_episodes=POOL_COUNT, passes=6,
                models=list(MODELS), seeds=list(SEEDS), single_query_chance=.125,
                fixed_training_probe=PROBE_COUNT, diagnostic_count=EVAL_COUNT, final_holdout_count=EVAL_COUNT,
                no_final_holdout_created=True,
                limits='Requires separately approved external one-deadline CPU/RAM/disk/output supervisor')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--validate-only', action='store_true')
    action.add_argument('--worker', action='store_true')
    parser.add_argument('--config')
    parser.add_argument('--model', choices=MODELS, default='gru')
    parser.add_argument('--seed', type=int, choices=SEEDS, default=11)
    args = parser.parse_args(argv)
    report = dict(schema=SCHEMA, status='failed', model=args.model, seed=args.seed, native_executed=False)
    try:
        config = read_config(args.config) if args.config else None
        if args.worker:
            if config is None:
                raise ValueError('Worker requires the frozen configuration')
            report.update(native_attempted=True, native_executed=None)
            run_worker(args.model, args.seed, config, report)
        else:
            report = validation_report(config)
    except Exception as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error)[:1000])
    print(json.dumps(report, sort_keys=True, allow_nan=False))
    return 1 if report['status'] == 'failed' else 0


if __name__ == '__main__':
    raise SystemExit(main())
