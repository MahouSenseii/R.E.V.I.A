"""STATE-020: reads every run's JSON in a folder and prints the comparison (and writes summary.json).

  python summarize.py runs/main
"""
import json, math, sys
from collections import defaultdict
from pathlib import Path


def label(r):
    c = r['config']
    if r['arm'] == 'dense':
        return f"dense, {c['experts']} experts' worth"
    if r['arm'] == 'slow':
        return f"slow, route held {c['hold']} tokens" + ('' if c['active'] == 2 else f", {c['active']} active")
    frozen = ', fixed random routes' if c.get('fixed_routes') else ', router never trained' if c.get('freeze_router') else ''
    return {'fast': 'fast, route chosen every token', 'turn': 'route planned once per turn', 'leaky': 'route from a fading summary',
            'oracle': 'route fixed by topic (not learned)'}[r['arm']] + frozen + ('' if c['active'] == 2 else f", {c['active']} active") + ('' if c['experts'] == 16 else f", {c['experts']} experts")


def spread(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return dict(mean=sum(values) / len(values), low=min(values), high=max(values), runs=len(values))


def show(s, scale=100.0, digits=1):
    if s is None:
        return '-'
    if s['runs'] == 1:
        return f"{scale * s['mean']:.{digits}f}"
    return f"{scale * s['mean']:.{digits}f} ({scale * s['low']:.{digits}f}-{scale * s['high']:.{digits}f})"


def main():
    folder = Path(sys.argv[1])
    groups = defaultdict(list)
    for path in sorted(folder.glob('*.json')):
        if path.name == 'summary.json':
            continue
        r = json.loads(path.read_text())
        groups[label(r)].append(r)
    order = sorted(groups, key=lambda k: (groups[k][0]['arm'] != 'dense', groups[k][0]['parameters']['loaded_per_token'], k))
    summary = {}
    print(f"{'arm':44s} {'runs':>4s} {'loaded/token':>12s} {'one topic':>18s} {'one switch':>18s} {'taking turns':>18s}")
    for name in order:
        runs = groups[name]
        p = runs[0]['parameters']
        row = dict(runs=len(runs), parameters=p, share_loaded_per_token=p['loaded_per_token'] / p['total'], seconds=spread([r['seconds'] for r in runs]))
        for regime in ('one_topic', 'one_switch', 'taking_turns'):
            get = lambda key: spread([r['regimes'][regime].get(key) for r in runs])
            row[regime] = {key: get(key) for key in ('accuracy', 'universal', 'topical', 'first_segment', 'later_segments', 'experts_touched_per_block',
                                                     'share_of_parameters_loaded', 'route_changes_per_token', 'route_follows_topic', 'route_follows_token_kind')}
            row[regime]['after_a_change'] = [spread([r['regimes'][regime].get(f'later_q{q}') for r in runs]) for q in range(8)]
            fetched = defaultdict(list)
            for r in runs:
                for key, value in (r['regimes'][regime].get('experts_fetched_per_token') or {}).items():
                    fetched[key].append(value)
            row[regime]['experts_fetched_per_token'] = {key: spread(v) for key, v in fetched.items()}
        summary[name] = row
        print(f"{name:44s} {len(runs):4d} {100 * row['share_loaded_per_token']:11.1f}% {show(row['one_topic']['accuracy']):>18s} {show(row['one_switch']['accuracy']):>18s} {show(row['taking_turns']['accuracy']):>18s}")

    print('\nWhat a conversation needs loaded, and how the routes behave (one topic | one switch | taking turns):')
    for name in order:
        row = summary[name]
        if row['one_topic']['experts_touched_per_block'] is None:
            continue
        cells = []
        for regime in ('one_topic', 'one_switch', 'taking_turns'):
            x = row[regime]
            cells.append(f"{show(x['experts_touched_per_block'], 1, 2)} experts/block, {show(x['share_of_parameters_loaded'])}% loaded")
        print(f"{name:44s} " + ' | '.join(cells))
    print('\nRoute changes per token, and what the routes follow (one-topic conversations):')
    for name in order:
        x = summary[name]['one_topic']
        if x['route_changes_per_token'] is None:
            continue
        print(f"{name:44s} changes/token {show(x['route_changes_per_token'], 1, 3)} | follows topic {show(x['route_follows_topic'], 1, 2)} | follows kind of token {show(x['route_follows_token_kind'], 1, 2)}")
    print('\nExperts fetched per token when only a few stay loaded (summed over blocks):')
    for name in order:
        for regime in ('one_topic', 'one_switch', 'taking_turns'):
            f = summary[name][regime]['experts_fetched_per_token']
            if f:
                print(f"{name:44s} {regime:13s} " + ', '.join(f"{key.replace('keeping_', 'keep ').replace('_per_block', '')}: {show(v, 1, 3)}" for key, v in f.items()))
    print('\nAccuracy on each question after the topic changes (one-switch conversations, questions 1 to 8 of the new topic):')
    for name in order:
        cells = summary[name]['one_switch']['after_a_change']
        print(f"{name:44s} " + ' '.join(f"{100 * c['mean']:5.1f}" if c else '    -' for c in cells))

    # Where each routed arm sits on the dense size curve: the dense size that gives the same one-topic accuracy.
    dense = sorted((summary[n]['parameters']['total'], summary[n]['one_topic']['accuracy']['mean']) for n in order if groups[n][0]['arm'] == 'dense')
    if len(dense) >= 2:
        print('\nDense size with the same one-topic accuracy (interpolated on the dense runs, in log size):')
        for name in order:
            if groups[name][0]['arm'] == 'dense':
                continue
            acc = summary[name]['one_topic']['accuracy']['mean']
            loaded = summary[name]['one_topic']['share_of_parameters_loaded']['mean'] * summary[name]['parameters']['total']
            equal = None
            if acc <= dense[0][1]:
                equal, note = dense[0][0], 'at or below the smallest dense run'
            elif acc >= dense[-1][1]:
                equal, note = dense[-1][0], 'at or above the largest dense run'
            else:
                for (p0, a0), (p1, a1) in zip(dense, dense[1:]):
                    if a0 <= acc <= a1 and a1 > a0:
                        equal, note = math.exp(math.log(p0) + (math.log(p1) - math.log(p0)) * (acc - a0) / (a1 - a0)), ''
                        break
            if equal:
                summary[name]['equal_dense_parameters'] = equal
                summary[name]['loaded_in_a_one_topic_conversation'] = loaded
                print(f"{name:44s} loads {loaded / 1e6:.2f}M in a one-topic conversation; a dense model needs {equal / 1e6:.2f}M for the same accuracy ({equal / loaded:.1f}x) {note}")
    (folder / 'summary.json').write_text(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
