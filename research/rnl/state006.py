"""STATE-006 preparation and explicitly gated future CPU worker.

Default/--validate-only is stdlib and offline. --worker and --native-check are
future operations requiring separate approval and the external resource guard.
This module itself does not enforce wall-time or OS memory limits. No worker
network, process launch, filesystem writes, pickle, or GPU queries are used.
"""
import argparse
from copy import deepcopy
import hashlib
import json
import math
import struct
import time

import state006_data as data

SCHEMA = 'STATE-006-cell-v1'
METRICS = ('checkpoint_A_A', 'final_A', 'final_B', 'final_OOD', 'untouched',
           'overwrite', 'state_reset', 'state_exchange', 'clean_reset')


def capacity(model):
    if model not in ('gru', 'rnn'):
        raise ValueError('model must be gru or rnn')
    hidden, gates = (32, 3) if model == 'gru' else (59, 1)
    return dict(parameters=gates * hidden * (15 + hidden + 2) + 8 * (hidden + 1),
                dense_macs=64 * gates * hidden * (15 + hidden) + 8 * hidden,
                state_bytes=4 * hidden)


def assess(cells):
    """Aggregate six immutable cells. Ratios are fractions, never percentages.

    Both models need complete finite metrics. Absolute quality/state thresholds
    apply to GRU; RNN is the paired baseline. Every seed must pass independently.
    Untouched accuracy is diagnostic and cannot promote an otherwise failed run.
    Malformed, duplicate, missing or failed cells produce incomplete, never GO.
    """
    expected = {(m, s) for m in ('gru', 'rnn') for s in data.SEEDS}
    valid, failures = {}, []
    malformed = not isinstance(cells, list)
    for cell in cells if isinstance(cells, list) else []:
        if not isinstance(cell, dict):
            malformed = True
            failures.append('malformed cell')
            continue
        key = (cell.get('model'), cell.get('seed'))
        if (not isinstance(key[0], str) or type(key[1]) is not int or key not in expected or
                cell.get('schema') != SCHEMA or cell.get('status') != 'complete'):
            malformed = True
            failures.append('invalid identity/schema/status')
            continue
        if key in valid:
            malformed = True
            failures.append(f'duplicate {key[0]}/{key[1]}')
            continue
        metrics = cell.get('metrics')
        if not isinstance(metrics, dict) or any(
                type(metrics.get(k)) not in (int, float) or not 0 <= metrics[k] <= 1 or
                not math.isfinite(metrics[k]) for k in METRICS):
            malformed = True
            failures.append(f'invalid metrics {key[0]}/{key[1]}')
            continue
        valid[key] = metrics
    missing = [f'{m}/{s}' for m, s in sorted(expected - valid.keys())]
    pairs = []
    for seed in data.SEEDS:
        if ('gru', seed) not in valid or ('rnn', seed) not in valid:
            continue
        g, r = valid['gru', seed], valid['rnn', seed]
        gap = g['final_OOD'] - r['final_OOD']
        loss = g['checkpoint_A_A'] - g['final_A']
        checks = {k: g[k] >= threshold for k, threshold in
                  [('checkpoint_A_A', .95), ('final_A', .95), ('final_B', .95),
                   ('final_OOD', .90), ('overwrite', .95)]}
        checks.update(ood_gap=gap >= .10 - 1e-12, old_A_loss=loss <= .02 + 1e-12,
                      clean_reset=g['clean_reset'] == 1.)
        for mode in ('state_reset', 'state_exchange'):
            checks[mode] = g[mode] <= .25 and g['final_OOD'] - g[mode] >= .50 - 1e-12
        failed = [name for name, passed in checks.items() if not passed]
        failures.extend(f'gru/{seed}:{name}' for name in failed)
        pairs.append(dict(seed=seed, ood_gap=gap, old_A_loss=loss, checks=checks,
                          gru=g, rnn=r, passed=not failed))
    status = 'incomplete' if malformed or missing else 'no_go' if failures else 'go'
    return dict(status=status, go=status == 'go', missing=missing, failures=failures, pairs=pairs)


def validation_report():
    return dict(schema='STATE-006-preparation-v1', status='prepared_native_unverified',
                native_executed=False, live_metrics=None, dataset=data.dataset_manifest(),
                capacity={m: capacity(m) for m in ('gru', 'rnn')},
                planned=dict(seeds=list(data.SEEDS), updates_A=192, updates_B=64,
                             batch=16, sequence=64, optimizer='Adam', learning_rate=.003,
                             weight_decay=0, clip_norm=1, dtype='float32', device='cpu',
                             threads=1, query_only=True),
                limits='External controller required: 180s/cell, 1200s total, 2GiB/worker; feasibility unknown')


def _native_setup(model_name, seed):
    # The only native import is in this future, explicit action path.
    import torch
    if not torch.__version__.endswith('+cpu') or torch.version.cuda is not None:
        raise RuntimeError('exact approved CPU build required')
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    torch.use_deterministic_algorithms(True)
    torch.default_generator.manual_seed(seed)
    hidden = 32 if model_name == 'gru' else 59

    class QueryModel(torch.nn.Module):
        def __init__(self):
            super().__init__()
            cls = torch.nn.GRU if model_name == 'gru' else torch.nn.RNN
            self.recurrent = cls(15, hidden, batch_first=True, device='cpu', dtype=torch.float32)
            self.head = torch.nn.Linear(hidden, 8, device='cpu', dtype=torch.float32)
            for name, parameter in self.recurrent.named_parameters():
                if name.startswith('weight_ih'):
                    torch.nn.init.xavier_uniform_(parameter)
                elif name.startswith('weight_hh'):
                    for block in parameter.chunk(3 if model_name == 'gru' else 1, dim=0):
                        torch.nn.init.orthogonal_(block)
                else:
                    torch.nn.init.zeros_(parameter)
            torch.nn.init.xavier_uniform_(self.head.weight)
            torch.nn.init.zeros_(self.head.bias)

        def forward(self, inputs, intervention=None):
            zero = inputs.new_zeros((1, inputs.shape[0], hidden))
            if intervention is None:
                outputs, _ = self.recurrent(inputs, zero)
            else:
                _, state = self.recurrent(inputs[:, :-1], zero)
                if intervention == 'reset':
                    state = torch.zeros_like(state)
                elif intervention == 'exchange':
                    state = state.roll(1, dims=1)
                else:
                    raise ValueError('unknown intervention')
                outputs, _ = self.recurrent(inputs[:, -1:], state)
            return self.head(outputs[:, -1])  # head/loss only at final QUERY

    model = QueryModel()
    observed = sum(p.numel() for p in model.parameters())
    if observed != capacity(model_name)['parameters'] or any(p.device.type != 'cpu' for p in model.parameters()):
        raise RuntimeError('native parameter/device mismatch')
    return torch, model


def _weight_digest(model):
    digest = hashlib.sha256()
    for name, tensor in sorted(model.state_dict().items()):
        digest.update(name.encode() + b'\0')
        values = tensor.detach().cpu().reshape(-1).tolist()
        digest.update(struct.pack('<' + 'f' * len(values), *values))
    return digest.hexdigest()


def _batch(torch, episodes):
    rows, targets = zip(*(data.encode(e['events']) for e in episodes))
    return (torch.tensor(rows, dtype=torch.float32, device='cpu'),
            torch.tensor(targets, dtype=torch.long, device='cpu'))


def native_check(model_name, seed):
    """Future guarded forward/backward probe; no optimizer or weight update."""
    torch, model = _native_setup(model_name, seed)
    before = _weight_digest(model)
    x, y = _batch(torch, [data.make_episode(7000 + i, 1 + i) for i in range(2)])
    logits = model(x)
    if tuple(logits.shape) != (2, 8) or not bool(torch.isfinite(logits).all()):
        raise RuntimeError('invalid native logits')
    loss = torch.nn.functional.cross_entropy(logits, y)
    loss.backward()
    if not all(p.grad is not None and bool(torch.isfinite(p.grad).all()) for p in model.parameters()):
        raise RuntimeError('invalid native gradient')
    after = _weight_digest(model)
    if before != after:
        raise RuntimeError('probe changed weights')
    return dict(schema='STATE-006-native-check-v1', status='complete', model=model_name, seed=seed,
                native_executed=True, torch_version=torch.__version__, cpu_only=True,
                parameters=capacity(model_name)['parameters'], shape=[2, 8], finite_gradients=True,
                optimizer_steps=0, before_sha256=before, after_sha256=after)


def run_worker(model_name, seed, report):
    """Future bounded cell. Paired fixed streams, 256 updates, no retries/HPO."""
    started = time.perf_counter()
    timing = dict(update_seconds=[])
    report['timings'] = timing
    torch, model = _native_setup(model_name, seed)
    report.update(native_executed=True, torch_version=torch.__version__,
                  capacity=capacity(model_name), metrics=None, training=dict(updates=0))
    optimizer = torch.optim.Adam(model.parameters(), lr=.003, weight_decay=0)
    specs = data.split_specs()
    timing['setup_seconds'] = time.perf_counter() - started
    first_eight_started = None
    checkpoint_A = None
    for phase in ('A', 'B'):
        phase_started = time.perf_counter()
        name = f'train_{phase}_{seed}'
        episodes = data.make_split(name, **specs[name])
        model.train()
        for offset in range(0, len(episodes), 16):
            update_started = time.perf_counter()
            if first_eight_started is None:
                first_eight_started = time.perf_counter()
            x, y = _batch(torch, episodes[offset:offset + 16])
            optimizer.zero_grad(set_to_none=True)
            loss = torch.nn.functional.cross_entropy(model(x), y)
            if not bool(torch.isfinite(loss)):
                raise RuntimeError('nonfinite training loss')
            loss.backward()
            norm = torch.nn.utils.clip_grad_norm_(model.parameters(), 1., error_if_nonfinite=True)
            if not bool(torch.isfinite(norm)):
                raise RuntimeError('nonfinite gradient')
            optimizer.step()
            timing['update_seconds'].append(time.perf_counter() - update_started)
            report['training']['updates'] += 1
            if report['training']['updates'] == 8:
                timing['first_8_updates_seconds'] = time.perf_counter() - first_eight_started
        timing[f'train_{phase}_seconds'] = time.perf_counter() - phase_started
        if phase == 'A':
            checkpoint_A = deepcopy(model.state_dict())
    if report['training']['updates'] != 256:
        raise RuntimeError('fixed update count mismatch')
    final = deepcopy(model.state_dict())
    final_digest = _weight_digest(model)
    # All held-out evaluation begins after both phases; no selection from tests.
    evaluation_started = time.perf_counter()

    def predictions(episodes, intervention=None):
        result = []
        model.eval()
        with torch.no_grad():
            for offset in range(0, len(episodes), 16):
                x, _ = _batch(torch, episodes[offset:offset + 16])
                logits = model(x, intervention)
                if not bool(torch.isfinite(logits).all()):
                    raise RuntimeError('nonfinite held-out logits')
                result.extend(logits.argmax(dim=1).tolist())
        return result

    def accuracy(episodes, intervention=None):
        predicted = predictions(episodes, intervention)
        return sum(p == data.oracle(e['events']) for p, e in zip(predicted, episodes)) / len(episodes)

    heldout = {name: data.make_split(name, **specs[name]) for name in
               ('eval_A', 'eval_B', 'eval_OOD', 'untouched', 'overwrite')}
    model.load_state_dict(checkpoint_A)
    checkpoint_digest = _weight_digest(model)
    metrics = dict(checkpoint_A_A=accuracy(heldout['eval_A']))
    model.load_state_dict(final)
    for key, name in [('final_A', 'eval_A'), ('final_B', 'eval_B'),
                      ('final_OOD', 'eval_OOD'), ('untouched', 'untouched')]:
        metrics[key] = accuracy(heldout[name])
    # A pair differs in exactly the last query-key SET value. Pair succeeds only
    # if BOTH answers are correct, preventing a one-sided overwriting shortcut.
    originals, alternatives = heldout['overwrite'], []
    for episode in originals:
        altered = deepcopy(episode)
        key = episode['query_key']
        i = max(i for i, event in enumerate(episode['events']) if event[:2] == ('SET', key))
        kind, key, value = episode['events'][i]
        altered['events'][i] = (kind, key, (value + 1) % 8)
        alternatives.append(altered)
    left, right = predictions(originals), predictions(alternatives)
    metrics['overwrite'] = sum(a == data.oracle(e['events']) and b == data.oracle(f['events'])
                               for a, b, e, f in zip(left, right, originals, alternatives)) / len(originals)
    for key, mode in [('state_reset', 'reset'), ('state_exchange', 'exchange')]:
        metrics[key] = accuracy(heldout['eval_OOD'], mode)
    clean = predictions(heldout['eval_OOD'])
    predictions(heldout['eval_B'])  # intervening unrelated episodes must leave no carryover
    repeated = predictions(heldout['eval_OOD'])
    metrics['clean_reset'] = sum(a == b for a, b in zip(clean, repeated)) / len(clean)
    if _weight_digest(model) != final_digest:
        raise RuntimeError('held-out evaluation changed final weights')
    timing['evaluation_seconds'] = time.perf_counter() - evaluation_started
    dataset_sha256 = data.dataset_manifest()['sha256']
    timing['total_seconds'] = time.perf_counter() - started
    report.update(status='complete', metrics=metrics, timings=timing,
                  checkpoints=dict(A_sha256=checkpoint_digest, final_sha256=final_digest,
                                   storage='RAM only; frozen after fixed update phases'),
                  dataset_sha256=dataset_sha256,
                  counts=dict(checkpoint_A_A=512, final_A=512, final_B=512, final_OOD=512,
                              untouched=128, overwrite=128, state_reset=512,
                              state_exchange=512, clean_reset=512))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--validate-only', action='store_true', help='offline stdlib preparation (default)')
    action.add_argument('--worker', action='store_true', help='future explicitly approved bounded CPU training')
    action.add_argument('--native-check', action='store_true', help='future approved CPU forward/backward probe; no update')
    parser.add_argument('--model', choices=('gru', 'rnn'), default='gru')
    parser.add_argument('--seed', type=int, choices=data.SEEDS, default=11)
    args = parser.parse_args(argv)
    report = dict(schema=SCHEMA, status='failed', model=args.model, seed=args.seed,
                  native_executed=False, metrics=None)
    try:
        if args.worker:
            report.update(native_attempted=True, native_executed=None)
            report = run_worker(args.model, args.seed, report)
        elif args.native_check:
            report.update(native_attempted=True, native_executed=None)
            report = native_check(args.model, args.seed)
        else:
            report = validation_report()
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {str(error)[:1000]}')
    print(json.dumps(report, sort_keys=True, allow_nan=False))
    return 1 if report['status'] == 'failed' else 0


if __name__ == '__main__':
    raise SystemExit(main())
