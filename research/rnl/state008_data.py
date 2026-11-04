"""STATE-008 counterfactual data and prediction accounting; stdlib only.

No generation occurs on import. Each four-example group changes only its final
QUERY key and the values of two earlier SETs. Metadata never enters the encoder.
"""
from collections import Counter
import hashlib
from itertools import combinations
import json
import random
import re

import state006_data as original

VERSION = 'STATE-008-quartet-v1'
GROUPS, COUNT, SEED = 168, 672, 8008
NAMES = ('state008_diagnostic', 'state008_final')
RULES = ('constant0', 'query_key', 'last_set', 'dictionary')


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def fingerprint(episodes):
    digest = hashlib.sha256()
    for episode in episodes:
        digest.update(json.dumps(episode, sort_keys=True, separators=(',', ':')).encode() + b'\n')
    return digest.hexdigest()


def make_split(name, seed=SEED):
    if name not in NAMES or type(seed) is not int or seed != SEED:
        raise ValueError('Frozen STATE-008 split name and seed required')
    pairs = [(keys, values) for keys in combinations(range(4), 2) for values in combinations(range(8), 2)]
    random.Random(_digest([VERSION, name, seed, 'order'])).shuffle(pairs)
    lags = [pair if orientation == 0 else pair[::-1]
            for pair in combinations(range(2, 9), 2) for orientation in (0, 1) for _ in range(4)]
    random.Random(_digest([VERSION, name, seed, 'delays'])).shuffle(lags)
    episodes = []
    for index, ((keys, values), delays) in enumerate(zip(pairs, lags)):
        gid = _digest([VERSION, name, seed, keys, values])
        rng = random.Random(_digest([gid, 'history']))
        positions = [63 - lag for lag in delays]
        third = rng.choice([key for key in range(4) if key not in keys])
        latest = rng.choice([value for value in range(8) if value not in values])
        base = [('SET', key, rng.randrange(8)) for key in range(4)]
        for i in range(4, 62):
            if i in positions:
                k = positions.index(i)
                base.append(('SET', keys[k], values[k]))
            elif rng.random() < .7:
                allowed = [key for key in range(4) if key not in keys or i < positions[keys.index(key)]]
                base.append(('SET', rng.choice(allowed), rng.randrange(8)))
            else:
                base.append(('NOOP', None, None))
        base.append(('SET', third, latest))
        for assignment in (0, 1):
            assigned = values if assignment == 0 else values[::-1]
            for query in (0, 1):
                events = list(base)
                for k in (0, 1):
                    events[positions[k]] = ('SET', keys[k], assigned[k])
                events.append(('QUERY', keys[query], None))
                episodes.append(dict(events=events, seed=_digest([VERSION, gid, assignment, query]),
                                     group_id=gid, group_index=index, assignment=assignment,
                                     query_variant=query, query_key=keys[query], lag=delays[query],
                                     target=assigned[query], keys=list(keys), values=list(values),
                                     designated_positions=positions.copy()))
    return episodes


def _integer(value, lo, hi):
    return type(value) is int and lo <= value <= hi


def _history(episode):
    return tuple(tuple(event) for event in episode['events'][:-1])


def _checked_episode(episode):
    if not isinstance(episode, dict):
        raise ValueError('Episode object required')
    events = episode.get('events')
    if not isinstance(events, (list, tuple)) or len(events) != 64:
        raise ValueError('Exactly 64 events required')
    if any(not isinstance(e, (list, tuple)) or len(e) != 3 for e in events):
        raise ValueError('Three-field events required')
    if any(tuple(event[:2]) != ('SET', key) for key, event in enumerate(events[:4])):
        raise ValueError('Four initialized keys required')
    target = original.oracle(events)
    memory, last = {}, None
    for kind, key, value in events:
        if kind == 'SET': memory[key], last = value, value
    key = events[-1][1]
    if target != memory[key]:
        raise ValueError('Independent dictionary/original oracle mismatch')
    keys, values, positions = (episode.get(field) for field in ('keys', 'values', 'designated_positions'))
    for pair, lo, hi in ((keys, 0, 3), (values, 0, 7), (positions, 55, 61)):
        if not isinstance(pair, (list, tuple)) or len(pair) != 2 or not all(_integer(x, lo, hi) for x in pair) or pair[0] == pair[1]:
            raise ValueError('Distinct valid key/value/position pairs required')
    if list(keys) != sorted(keys) or list(values) != sorted(values):
        raise ValueError('Canonical unordered key/value pairs required')
    assignment, query = episode.get('assignment'), episode.get('query_variant')
    if not _integer(assignment, 0, 1) or not _integer(query, 0, 1) or not _integer(episode.get('group_index'), 0, GROUPS - 1):
        raise ValueError('Valid group/variant identity required')
    gid = episode.get('group_id')
    if not isinstance(gid, str) or re.fullmatch('[0-9a-f]{64}', gid) is None:
        raise ValueError('Group digest required')
    if episode.get('seed') != _digest([VERSION, gid, assignment, query]):
        raise ValueError('Variant identity mismatch')
    assigned = values if assignment == 0 else values[::-1]
    for k in (0, 1):
        if tuple(events[positions[k]]) != ('SET', keys[k], assigned[k]):
            raise ValueError('Designated SET mismatch')
    last_key_write = max(i for i, event in enumerate(events[:-1]) if tuple(event[:2]) == ('SET', key))
    if (not _integer(episode.get('query_key'), 0, 3) or episode['query_key'] != key or key != keys[query]
            or not _integer(episode.get('lag'), 2, 8) or episode['lag'] != 63 - last_key_write
            or last_key_write != positions[query] or not _integer(episode.get('target'), 0, 7)
            or episode['target'] != target or target != assigned[query]):
        raise ValueError('Observable query/lag/target mismatch')
    latest = tuple(events[62])
    if latest[0] != 'SET' or latest[1] in keys or latest[2] in values:
        raise ValueError('Fixed other-key latest SET must differ from both answers')
    return dict(constant0=0, query_key=key, last_set=last, dictionary=target)


def _grouped(episodes):
    if not isinstance(episodes, (list, tuple)) or not episodes:
        raise ValueError('Nonempty complete quartet sequence required')
    groups, indices, variants, histories, observed = {}, {}, set(), {}, []
    for i, episode in enumerate(episodes):
        observed.append(_checked_episode(episode))
        gid, index = episode['group_id'], episode['group_index']
        if index in indices and indices[index] != gid:
            raise ValueError('Group index collision')
        indices[index] = gid
        slot = (episode['assignment'], episode['query_variant'])
        group = groups.setdefault(gid, {})
        if slot in group or episode['seed'] in variants:
            raise ValueError('Duplicate quartet variant')
        group[slot] = i
        variants.add(episode['seed'])
        history = _digest(_history(episode))
        if history in histories and histories[history] != gid:
            raise ValueError('Observable history shared across unrelated groups')
        histories[history] = gid
    expected = {(a, q) for a in (0, 1) for q in (0, 1)}
    for group in groups.values():
        if set(group) != expected:
            raise ValueError('All four variants required')
        reference = episodes[group[0, 0]]
        for i in group.values():
            if any(episodes[i][field] != reference[field] for field in
                   ('group_index', 'keys', 'values', 'designated_positions')):
                raise ValueError('Quartet metadata mismatch')
        for assignment in (0, 1):
            if _history(episodes[group[assignment, 0]]) != _history(episodes[group[assignment, 1]]):
                raise ValueError('Query variant changed history')
        original_history, swapped_history = _history(reference), _history(episodes[group[1, 0]])
        changed = {i for i, pair in enumerate(zip(original_history, swapped_history)) if pair[0] != pair[1]}
        if changed != set(reference['designated_positions']):
            raise ValueError('Swap changed more than the two designated SET values')
    return groups, observed


def _measure(episodes, groups, correct):
    def record(hits, count):
        return dict(correct=hits, count=count, accuracy=hits / count)
    lags = {}
    for e, hit in zip(episodes, correct):
        row = lags.setdefault(str(e['lag']), [0, 0]); row[0] += int(hit); row[1] += 1
    return dict(individual=record(sum(correct), len(correct)),
                by_lag={lag: record(*value) for lag, value in sorted(lags.items())},
                query_pairs=record(sum(all(correct[group[a, q]] for q in (0, 1))
                                       for group in groups.values() for a in (0, 1)), 2 * len(groups)),
                value_pairs=record(sum(all(correct[group[a, q]] for a in (0, 1))
                                       for group in groups.values() for q in (0, 1)), 2 * len(groups)),
                quartets=record(sum(all(correct[i] for i in group.values()) for group in groups.values()), len(groups)))


def prediction_metrics(episodes, predictions):
    groups, observed = _grouped(episodes)
    if not isinstance(predictions, (list, tuple)) or len(predictions) != len(episodes) or not all(_integer(p, 0, 7) for p in predictions):
        raise ValueError('One integer class prediction per episode required')
    correct = [p == e['target'] for p, e in zip(predictions, episodes)]
    result = _measure(episodes, groups, correct)
    result.update(count=len(episodes), groups=len(groups), baselines={}, baseline_agreement={},
                  rows=[[e['group_index'], e['assignment'], e['query_variant'], e['query_key'],
                         e['lag'], e['target'], baseline['last_set'], p]
                        for e, baseline, p in zip(episodes, observed, predictions)])
    for name in RULES:
        base_correct = [r[name] == e['target'] for r, e in zip(observed, episodes)]
        result['baselines'][name] = _measure(episodes, groups, base_correct)
        result['baseline_agreement'][name] = dict(
            count=len(episodes), agree=sum(p == r[name] for p, r in zip(predictions, observed)),
            both_correct=sum(a and b for a, b in zip(correct, base_correct)),
            model_only_correct=sum(a and not b for a, b in zip(correct, base_correct)),
            baseline_only_correct=sum(not a and b for a, b in zip(correct, base_correct)),
            both_wrong=sum(not a and not b for a, b in zip(correct, base_correct)))
    return result


def validate_split(episodes):
    if not isinstance(episodes, (list, tuple)) or len(episodes) != COUNT:
        raise ValueError('Exactly 672 examples required')
    groups, observed = _grouped(episodes)
    if (len(groups) != GROUPS or [e['group_index'] for e in episodes] != [i for i in range(GROUPS) for _ in range(4)]
            or [(e['assignment'], e['query_variant']) for e in episodes] != [(a, q) for _ in range(GROUPS) for a in (0, 1) for q in (0, 1)]):
        raise ValueError('Canonical 168-group quartet ordering required')
    key_values = Counter((e['query_key'], e['target']) for e in episodes)
    lags = Counter(e['lag'] for e in episodes)
    if key_values != Counter({(k, v): 21 for k in range(4) for v in range(8)}) or lags != Counter({lag: 96 for lag in range(2, 9)}):
        raise ValueError('Key/value/delay balance mismatch')
    representatives = [episodes[group[0, 0]] for group in groups.values()]
    if Counter((tuple(e['keys']), tuple(e['values'])) for e in representatives) != Counter(
            {(keys, values): 1 for keys in combinations(range(4), 2) for values in combinations(range(8), 2)}):
        raise ValueError('Full key-pair/value-pair coverage required')
    if Counter(tuple(63 - p for p in e['designated_positions']) for e in representatives) != Counter(
            {(a, b): 4 for a in range(2, 9) for b in range(2, 9) if a != b}):
        raise ValueError('Balanced ordered delay pairs required')
    metrics = prediction_metrics(episodes, [e['target'] for e in episodes])
    for name, count, quartets in [('constant0', 84, 0), ('query_key', 84, 0), ('last_set', 0, 0), ('dictionary', COUNT, GROUPS)]:
        if metrics['baselines'][name]['individual']['correct'] != count or metrics['baselines'][name]['quartets']['correct'] != quartets:
            raise ValueError('Non-neural control failed')
    return dict(status='passed', count=COUNT, groups=GROUPS, fingerprint=fingerprint(episodes),
                key_value_count=21, per_lag_count={str(k): v for k, v in sorted(lags.items())},
                baselines=metrics['baselines'])


def validate_disjoint(newgroups, originaltraininghistoryiterator):
    splits = list(newgroups.values()) if isinstance(newgroups, dict) else list(newgroups)
    if not splits:
        raise ValueError('New evaluation groups required')
    group_ids, variant_ids, histories = set(), set(), {}
    for split_index, episodes in enumerate(splits):
        groups, _ = _grouped(episodes)
        if group_ids.intersection(groups):
            raise ValueError('Group identity overlap between splits')
        group_ids.update(groups)
        for e in episodes:
            if e['seed'] in variant_ids:
                raise ValueError('Variant identity overlap between splits')
            variant_ids.add(e['seed'])
            history = _digest(_history(e)); owner = (split_index, e['group_id'])
            if history in histories and histories[history] != owner:
                raise ValueError('Observable history overlap across groups/splits')
            histories[history] = owner
    original_count = 0
    for e in originaltraininghistoryiterator:
        if isinstance(e, dict):
            events, identity = e['events'], e.get('seed')
        else:
            events, identity = e, None
        if not isinstance(events, (list, tuple)) or len(events) not in (63, 64):
            raise ValueError('Original episode or 63-event observable history required')
        history = events[:-1] if len(events) == 64 and events[-1][0] == 'QUERY' else events
        if len(history) != 63:
            raise ValueError('Original observable history length mismatch')
        # Validate the supplied reference, rather than treating malformed input
        # as evidence of separation. Original pools initialize all four keys.
        if any(tuple(event[:2]) != ('SET', key) for key, event in enumerate(history[:4])):
            raise ValueError('Original history must initialize four keys')
        original.oracle(events if len(events) == 64 else [*history, ('QUERY', 0, None)])
        if identity in group_ids or identity in variant_ids or _digest(history) in histories:
            raise ValueError('New/original training history overlap')
        original_count += 1
    if original_count == 0:
        raise ValueError('Original training history evidence required')
    return dict(status='passed', new_splits=len(splits), new_groups=len(group_ids),
                new_unique_histories=len(histories), original_episodes=original_count)
