"""STATE-008 fixed-training replay and counterfactual evaluation.

Import/default CLI is stdlib-only and does not generate data. Only the explicit
future --worker path imports the existing CPU framework; external resource
supervision and separate execution approval are required. No network, process
launch, direct writes, model-weight saving or GPU queries are used here.
"""
import argparse
import json
import math
import re
import time

import state006 as previous
import state006_data as old_data
import state007 as original
import state008_data as data

SCHEMA = 'STATE-008-cell-v1'
MODELS, SEEDS = ('rnn', 'gru'), (11, 23, 37)
CHECKPOINTS = original.CHECKPOINTS
DIAGNOSTIC_AT = (0, 192, 1152)


def _number(value, hi=None):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0 and (hi is None or value <= hi)


def _sha(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def validate_config(config):
    if not isinstance(config, dict):
        raise ValueError('Configuration object required')
    fixed = dict(schema='STATE-008-config-v1',models=list(MODELS),seeds=list(SEEDS),
        checkpoints=list(CHECKPOINTS),diagnostic_predictions_at=list(DIAGNOSTIC_AT),
        updates=1152,batch=16,train_count=3072,epochs=6,groups=168,count=672,
        evaluation_seed=8008,diagnostic_name='state008_diagnostic',final_name='state008_final',
        torch_version='2.15.0+cpu',material_diagnostic_quartet_gain=17,
        material_final_quartets=17,material_final_individual=168,
        reliability_final_quartets=135,reliability_final_individual=605)
    if any(type(config.get(k)) is not type(v) or config[k] != v for k,v in fixed.items()):
        raise ValueError('Frozen scientific configuration mismatch')
    if any(type(x) is not int for key in ('seeds','checkpoints','diagnostic_predictions_at') for x in config[key]):
        raise ValueError('Integer scientific sequences required')
    references = config.get('reference_weights')
    if (not isinstance(references, dict) or set(references) != {f'{m}/{s}' for m in MODELS for s in SEEDS}
            or any(not isinstance(row,dict) or set(row) != set(map(str,CHECKPOINTS))
                   or not all(_sha(v) for v in row.values()) for row in references.values())):
        raise ValueError('All48 original weight hashes required')
    for key, expected in [('reference_train',set(map(str,SEEDS))),('set_fingerprints',set(data.NAMES))]:
        value = config.get(key)
        if not isinstance(value,dict) or set(value) != expected or not all(_sha(v) for v in value.values()):
            raise ValueError('Frozen data identities required: '+key)
    return config


def read_config(path):
    with open(path,encoding='utf-8') as stream:
        def pairs(items):
            result = {}
            for key,value in items:
                if key in result: raise ValueError('Duplicate configuration key')
                result[key] = value
            return result
        value = json.load(stream,object_pairs_hook=pairs,
            parse_constant=lambda _:(_ for _ in ()).throw(ValueError('Nonfinite configuration')))
    return validate_config(value)


def checked_split(config, name):
    episodes = data.make_split(name,config['evaluation_seed'])
    if data.fingerprint(episodes) != config['set_fingerprints'][name]:
        raise ValueError('Frozen counterfactual dataset mismatch')
    data.validate_split(episodes)
    return episodes


def final_split(config, updates):
    if type(updates) is not int or updates != 1152:
        raise RuntimeError('Final evaluation is unavailable before exact final update')
    return checked_split(config,config['final_name'])


def _same_typed(actual, expected):
    if type(actual) is not type(expected): return False
    if isinstance(expected,dict):
        return actual.keys()==expected.keys() and all(_same_typed(actual[k],v) for k,v in expected.items())
    if isinstance(expected,list):
        return len(actual)==len(expected) and all(_same_typed(a,b) for a,b in zip(actual,expected))
    return actual==expected and (not isinstance(actual,float) or math.isfinite(actual))


def validate_evaluation(record, episodes):
    if (not isinstance(record,dict) or record.get('preserved') is not True
            or not _number(record.get('loss')) or not _number(record.get('seconds'))):
        raise ValueError('Finite noninvasive evaluation required')
    metrics = record.get('metrics')
    rows = metrics.get('rows') if isinstance(metrics,dict) else None
    if (not isinstance(rows,list) or len(rows) != len(episodes)
            or any(not isinstance(row,list) or len(row) != 8 or any(type(x) is not int for x in row) for row in rows)):
        raise ValueError('Exact compact integer prediction rows required')
    recomputed = data.prediction_metrics(episodes,[row[-1] for row in rows])
    if not _same_typed(metrics,recomputed):
        raise ValueError('Predictions, identities and aggregates disagree')
    return record


def validate_cell(cell, config):
    validate_config(config)
    if not isinstance(cell,dict): raise ValueError('Cell object required')
    model,seed = cell.get('model'),cell.get('seed')
    if (model not in MODELS or type(seed) is not int or seed not in SEEDS
            or cell.get('schema') != SCHEMA or cell.get('status') != 'complete'
            or cell.get('native_executed') is not True or cell.get('cpu_only') is not True
            or cell.get('torch_version') != config['torch_version']
            or cell.get('capacity') != previous.capacity(model)
            or cell.get('evaluation_preserved_weights_and_rng') is not True
            or cell.get('training') != dict(updates=1152,passes=6,unique_training_episodes=3072)):
        raise ValueError('Complete fixed CPU cell required')
    references = config['reference_weights'][f'{model}/{seed}']
    checks = cell.get('weight_checks')
    if (not isinstance(checks,list) or len(checks) != 8
            or any(not isinstance(row,dict) or type(row.get('updates')) is not int or row['updates'] != n
                   or row.get('observed') != references[str(n)] or row.get('expected') != references[str(n)]
                   or row.get('matched') is not True for row,n in zip(checks,CHECKPOINTS))):
        raise ValueError('Exact original trajectory hash checks required')
    identity = dict(train=config['reference_train'][str(seed)],
        diagnostic=config['set_fingerprints'][config['diagnostic_name']],
        final=config['set_fingerprints'][config['final_name']])
    if cell.get('data_identity') != identity: raise ValueError('Data identity mismatch')
    probes = cell.get('original_training_probe')
    if not isinstance(probes,list) or len(probes) != 8: raise ValueError('Eight fixed probe records required')
    for row,n in zip(probes,CHECKPOINTS):
        if (not isinstance(row,dict) or type(row.get('updates')) is not int or row['updates'] != n
                or type(row.get('correct')) is not int or not 0 <= row['correct'] <= 128
                or type(row.get('count')) is not int or row['count'] != 128
                or not _number(row.get('accuracy'),1) or row['accuracy'] != row['correct']/128 or not _number(row.get('loss'))):
            raise ValueError('Invalid training-pool probe record')
    diagnostic = checked_split(config,config['diagnostic_name'])
    records = cell.get('diagnostic')
    if not isinstance(records,list) or len(records) != 3: raise ValueError('Three diagnostic records required')
    for row,n in zip(records,DIAGNOSTIC_AT):
        if not isinstance(row,dict) or type(row.get('updates')) is not int or row['updates'] != n:
            raise ValueError('Fixed diagnostic checkpoint identity required')
        validate_evaluation(row.get('evaluation'),diagnostic)
    final = cell.get('final_holdout')
    if (not isinstance(final,dict) or type(final.get('evaluations')) is not int or final['evaluations'] != 1
            or type(final.get('evaluated_after_updates')) is not int or final['evaluated_after_updates'] != 1152):
        raise ValueError('Final once at fixed endpoint required')
    validate_evaluation(final.get('evaluation'),final_split(config,1152))
    online = cell.get('online_summary')
    if (not isinstance(online,dict) or type(online.get('count')) is not int or online['count'] != 1152
            or any(not _number(online.get(k)) for k in ('loss_mean','loss_min','loss_max'))
            or not online['loss_min'] <= online['loss_mean'] <= online['loss_max']
            or not _number(online.get('accuracy_mean'),1)):
        raise ValueError('Finite online summaries required')
    timings = cell.get('timings')
    if not isinstance(timings,dict) or any(not _number(timings.get(k)) for k in ('setup_seconds','training_seconds','total_seconds')):
        raise ValueError('Finite operational timings required')
    return cell


def screen(config, initial_quartets, diagnostic_quartets, final_correct, final_quartets):
    values = (initial_quartets,diagnostic_quartets,final_correct,final_quartets)
    if any(type(v) is not int or not 0 <= v <= limit for v,limit in zip(values,(168,168,672,168))):
        raise ValueError('Integer screening counts required')
    if final_quartets*4 > final_correct: raise ValueError('Inconsistent quartet/individual counts')
    return dict(material_signal=(diagnostic_quartets-initial_quartets >= config['material_diagnostic_quartet_gain']
        and final_quartets >= config['material_final_quartets'] and final_correct >= config['material_final_individual']),
        reliability=(final_quartets >= config['reliability_final_quartets']
                     and final_correct >= config['reliability_final_individual']))


def assess(cells, config):
    validate_config(config)
    found,failures = {},[]
    for cell in cells if isinstance(cells,list) else []:
        try:
            validate_cell(cell,config)
            key = (cell['model'],cell['seed'])
            if key in found: raise ValueError('Duplicate cell identity')
            found[key] = cell
        except Exception as exc:
            failures.append(str(exc))
    expected = {(m,s) for m in MODELS for s in SEEDS}
    missing = [f'{m}/{s}' for m,s in sorted(expected-found.keys())]
    if not isinstance(cells,list) or missing or failures:
        return dict(status='incomplete',missing=missing,failures=failures,models={})
    models = {}
    for model in MODELS:
        rows = []
        for seed in SEEDS:
            cell = found[model,seed]
            first,middle,last = [r['evaluation']['metrics'] for r in cell['diagnostic']]
            final = cell['final_holdout']['evaluation']['metrics']
            gain = last['quartets']['correct']-first['quartets']['correct']
            screens = screen(config,first['quartets']['correct'],last['quartets']['correct'],
                             final['individual']['correct'],final['quartets']['correct'])
            rows.append(dict(seed=seed,initial_diagnostic_quartets=first['quartets']['correct'],
                diagnostic192_quartets=middle['quartets']['correct'],final_diagnostic_quartets=last['quartets']['correct'],
                diagnostic_gain_over_initial=gain,diagnostic_gain_over192=last['quartets']['correct']-middle['quartets']['correct'],
                final_individual=final['individual']['correct'],final_quartets=final['quartets']['correct'],
                **screens))
        models[model] = dict(seeds=rows,material_signal_passed=all(r['material_signal'] for r in rows),
                             reliability_passed=all(r['reliability'] for r in rows))
    return dict(status='complete',models=models,candidate_for_revia=False,integrated_into_revia=False,
        interpretation='Research screens only; report every seed and all misses. Quartets are dependent groups, not four independent observations.')


def validation_report(config):
    validate_config(config)
    diagnostic = checked_split(config,config['diagnostic_name'])
    final = checked_split(config,config['final_name'])
    def training():
        for seed in SEEDS:
            name = f'train_A_{seed}'
            yield from old_data.iter_split(name,**old_data.split_specs()[name])
    disjoint = data.validate_disjoint([diagnostic,final],training())
    for seed in SEEDS:
        pool = original.training_pool(seed)
        if original.episode_digest(pool) != config['reference_train'][str(seed)]:
            raise ValueError('Original training pool identity mismatch')
    return dict(schema='STATE-008-preparation-v1',native_executed=False,trained=False,
        dataset={name:data.validate_split(group) for name,group in zip(data.NAMES,(diagnostic,final))},
        disjoint=disjoint,counts=dict(groups_per_set=168,episodes_per_set=672,prediction_rows_future=16128),
        native_validation='unrun; exact future guarded execution approval required')


def _evaluate(torch, model, episodes):
    started = time.perf_counter()
    training,weights,rng = model.training,previous._weight_digest(model),torch.get_rng_state().clone()
    predicted,loss_sum = [],0.
    try:
        model.eval()
        with torch.no_grad():
            for offset in range(0,len(episodes),16):
                x,y = previous._batch(torch,episodes[offset:offset+16])
                logits = model(x)
                loss = torch.nn.functional.cross_entropy(logits,y,reduction='sum')
                if not bool(torch.isfinite(logits).all()) or not bool(torch.isfinite(loss)):
                    raise RuntimeError('Nonfinite counterfactual evaluation')
                predicted.extend(logits.argmax(dim=1).tolist())
                loss_sum += loss.item()
    finally:
        model.train(training)
    if (weights != previous._weight_digest(model) or not bool(torch.equal(rng,torch.get_rng_state()))
            or model.training != training):
        raise RuntimeError('Evaluation changed weights, RNG or mode')
    return dict(metrics=data.prediction_metrics(episodes,predicted),loss=loss_sum/len(episodes),
                seconds=time.perf_counter()-started,preserved=True)


def run_worker(model_name, seed, config, report):
    validate_config(config)
    if model_name not in MODELS or type(seed) is not int or seed not in SEEDS:
        raise ValueError('Frozen model/seed required')
    started = time.perf_counter()
    report.update(training=dict(updates=0,passes=6,unique_training_episodes=3072),weight_checks=[],
        original_training_probe=[],diagnostic=[],final_holdout=None,
        evaluation_preserved_weights_and_rng=False,data_identity=dict(train=None,diagnostic=None,final=None))
    pool = original.training_pool(seed)
    if original.episode_digest(pool) != config['reference_train'][str(seed)]:
        raise ValueError('Original training pool mismatch')
    probe = original.select_probe(pool)
    diagnostic = checked_split(config,config['diagnostic_name'])
    report['data_identity'].update(train=original.episode_digest(pool),diagnostic=data.fingerprint(diagnostic))
    report['native_executed'] = True  # Records native path entry even if setup later fails.
    torch,model = previous._native_setup(model_name,seed)
    if torch.__version__ != config['torch_version']: raise RuntimeError('Exact CPU framework required')
    report.update(cpu_only=True,torch_version=torch.__version__,capacity=previous.capacity(model_name))
    optimizer = torch.optim.Adam(model.parameters(),lr=.003,weight_decay=0)
    report['timings'] = dict(setup_seconds=time.perf_counter()-started)
    def checkpoint(updates):
        observed = previous._weight_digest(model)
        expected = config['reference_weights'][f'{model_name}/{seed}'][str(updates)]
        matched = observed == expected
        report['weight_checks'].append(dict(updates=updates,observed=observed,expected=expected,matched=matched))
        if not matched: raise RuntimeError('Original training trajectory mismatch at update'+str(updates))
        report['original_training_probe'].append(dict(updates=updates,**original._evaluate(torch,model,probe)))
        if updates in DIAGNOSTIC_AT:
            report['diagnostic'].append(dict(updates=updates,evaluation=_evaluate(torch,model,diagnostic)))
    checkpoint(0)
    training_started = time.perf_counter()
    losses,total_accuracy = [],0.
    for update,first,last in original.batch_schedule():
        model.train()
        x,y = previous._batch(torch,pool[first:last])
        optimizer.zero_grad(set_to_none=True)
        logits = model(x)
        loss = torch.nn.functional.cross_entropy(logits,y)
        if not bool(torch.isfinite(loss)) or not bool(torch.isfinite(logits).all()):
            raise RuntimeError('Nonfinite training loss/logits')
        losses.append(loss.item())
        total_accuracy += int((logits.argmax(dim=1)==y).sum().item())/16
        loss.backward()
        norm = torch.nn.utils.clip_grad_norm_(model.parameters(),1.,error_if_nonfinite=True)
        if not bool(torch.isfinite(norm)): raise RuntimeError('Nonfinite gradient norm')
        optimizer.step()
        report['training']['updates'] = update
        if update in CHECKPOINTS: checkpoint(update)
    report['timings']['training_seconds'] = time.perf_counter()-training_started
    report['timings']['training_includes_checkpoint_evaluation'] = True
    holdout = final_split(config,report['training']['updates'])
    report['data_identity']['final'] = data.fingerprint(holdout)
    report['final_holdout'] = dict(evaluations=1,evaluated_after_updates=1152,evaluation=_evaluate(torch,model,holdout))
    report['evaluation_preserved_weights_and_rng'] = True
    report['online_summary'] = dict(count=len(losses),loss_mean=sum(losses)/len(losses),loss_min=min(losses),
        loss_max=max(losses),accuracy_mean=total_accuracy/len(losses))
    report['timings']['total_seconds'] = time.perf_counter()-started


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--worker',action='store_true')
    mode.add_argument('--validate-only',action='store_true')
    parser.add_argument('--config')
    parser.add_argument('--model',choices=MODELS)
    parser.add_argument('--seed',type=int)
    args = parser.parse_args(argv)
    if not args.worker and not args.validate_only:
        print('{"status":"prepared_unexecuted","native_executed":false,"trained":false}')
        return 0
    config = read_config(args.config)
    if args.validate_only:
        print(json.dumps(validation_report(config),sort_keys=True,allow_nan=False))
        return 0
    report = dict(schema=SCHEMA,status='incomplete',model=args.model,seed=args.seed,native_executed=False,cpu_only=False)
    try:
        run_worker(args.model,args.seed,config,report)
        report['status'] = 'complete'
        validate_cell(report,config)
    except Exception as exc:
        report.update(status='incomplete',failure=dict(type=type(exc).__name__,message=str(exc)[:2000]))
    print(json.dumps(report,sort_keys=True,allow_nan=False))
    return 0 if report['status']=='complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
