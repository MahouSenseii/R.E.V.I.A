"""STATE-006 synthetic event data; standard library only.

Events are (kind, key, value) triples. Episodes contain events, a hexadecimal
group seed, actual lag and query_key; the answer is computed separately by the
dictionary oracle. encode returns (input_rows, target). Only SET has value bits.
Split identity includes its name, so equal root seeds cannot share groups.
"""
import hashlib
import json
import random

VERSION = 'STATE-006-data-v2'
STEPS, INPUTS, KEYS, VALUES = 64, 15, 4, 8
SEEDS = (11, 23, 37)


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def oracle(events):
    """Independent dictionary last-write oracle; reject ambiguous queries."""
    memory = {}
    answer = None
    for i, event in enumerate(events):
        if len(event) != 3:
            raise ValueError('event must have three fields')
        kind, key, value = event
        if kind == 'SET':
            if type(key) is not int or not 0 <= key < KEYS or type(value) is not int or not 0 <= value < VALUES:
                raise ValueError('invalid SET')
            memory[key] = value
        elif kind == 'QUERY':
            if i != len(events) - 1 or type(key) is not int or key not in memory or value is not None:
                raise ValueError('one final initialized QUERY is required')
            answer = memory[key]
        elif kind != 'NOOP' or key is not None or value is not None:
            raise ValueError('invalid event')
    if answer is None:
        raise ValueError('missing QUERY')
    return answer


def encode(events):
    target = oracle(events)
    rows = []
    for kind, key, value in events:
        row = [0.] * INPUTS
        row[('SET', 'NOOP', 'QUERY').index(kind)] = 1.
        if key is not None:
            row[3 + key] = 1.
        if kind == 'SET':
            row[7 + value] = 1.
        rows.append(row)
    return rows, target


def _episode(seed, lag, untouched, key=None, target=None):
    if type(lag) is not int or not 1 <= lag <= 32:
        raise ValueError('lag must be 1..32')
    rng = random.Random(str(seed))
    key = rng.randrange(KEYS) if key is None else key
    target = rng.randrange(VALUES) if target is None else target
    events = [('SET', k, target if untouched and k == key else
               rng.randrange(VALUES)) for k in range(KEYS)]
    last = 63 - lag
    for i in range(4, 63):
        if not untouched and i == last:
            events.append(('SET', key, target))
        elif rng.random() < .7:
            choices = [k for k in range(KEYS) if k != key] if untouched or i > last else list(range(KEYS))
            k = rng.choice(choices)
            events.append(('SET', k, rng.randrange(VALUES)))
        else:
            events.append(('NOOP', None, None))
    if not untouched:
        # Older random history is independent of the answer. Only the immediate
        # predecessor must differ, to make the designated SET an actual change.
        previous = max(i for i, event in enumerate(events[:last]) if event[:2] == ('SET', key))
        old = rng.randrange(VALUES - 1)
        events[previous] = ('SET', key, old + (old >= target))
    events.append(('QUERY', key, None))
    return dict(events=events, seed=str(seed), lag=63 - key if untouched else lag, query_key=key)


def make_episode(seed, lag, untouched=False):
    return _episode(seed, lag, untouched)


def iter_split(name, count, lag_range, seed, *, untouched=False):
    """Balanced 32 key/value combinations, independently shuffled balanced lags.

    A group is one complete episode; no time slices cross partitions. A/B/OOD
    evaluation and training names are distinct namespaces. Training never reads
    an evaluation group. Counts must be multiples of32 and lag-range width.
    """
    lo, hi = lag_range
    if not isinstance(name, str) or not name or type(count) is not int or count <= 0 or count % 32:
        raise ValueError('named split needs a positive multiple of32')
    if not (type(lo) is int and type(hi) is int and 1 <= lo <= hi <= 32) or count % (hi - lo + 1):
        raise ValueError('invalid or unbalanced lag range')
    identity = [VERSION, name, seed]
    rng = random.Random(_digest(identity))
    assignments = [(k, v) for _ in range(count // 32) for k in range(KEYS) for v in range(VALUES)]
    lags = [lag for _ in range(count // (hi - lo + 1)) for lag in range(lo, hi + 1)]
    rng.shuffle(assignments)
    rng.shuffle(lags)
    for i, ((key, target), lag) in enumerate(zip(assignments, lags)):
        group = _digest([*identity, i])
        yield _episode(group, lag, untouched, key, target)


def make_split(name, count, lag_range, seed, *, untouched=False):
    return list(iter_split(name, count, lag_range, seed, untouched=untouched))


def split_specs():
    specs = {f'train_{phase}_{s}': dict(count=count, lag_range=lags, seed=s, untouched=False)
             for s in SEEDS for phase, count, lags in [('A', 3072, (1, 8)), ('B', 1024, (9, 16))]}
    specs.update({f'eval_{phase}': dict(count=512, lag_range=lags, seed=6006, untouched=False)
                  for phase, lags in [('A', (1, 8)), ('B', (9, 16)), ('OOD', (17, 32))]})
    specs['untouched'] = dict(count=128, lag_range=(1, 1), seed=6006, untouched=True)
    specs['overwrite'] = dict(count=128, lag_range=(17, 32), seed=6006, untouched=False)
    return specs


def dataset_manifest():
    """Stream canonical episode hashes; no trace files or giant JSON payloads."""
    splits = {}
    for name, spec in split_specs().items():
        hasher = hashlib.sha256()
        for episode in iter_split(name, **spec):
            hasher.update(json.dumps(episode, sort_keys=True, separators=(',', ':')).encode() + b'\n')
        splits[name] = {**spec, 'lag_range': list(spec['lag_range']), 'sha256': hasher.hexdigest()}
    result = dict(version=VERSION, encoding='SET/NOOP/QUERY+key4+SET-only-value8',
                  steps=STEPS, inputs=INPUTS, classes=VALUES, splits=splits)
    return {**result, 'sha256': _digest(result)}
