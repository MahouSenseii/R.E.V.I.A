"""Thin future STATE-006 controller. Default validation is offline and read-only.

--seal writes a post-setup provenance manifest; it grants no approval. Future
--execute requires the separately reviewed manifest's exact SHA256. No retry.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

from state006_setup import (ROOT, HERE, GIB2, EVIDENCE, SOURCES, SHA, check_budget, budget_monitor,
                            digest, file_census, owned_bytes, package_versions,
                            plain_path, read_json, require_fresh, runtime_path,
                            validate_config, write_json)

PROBE = [('gru', 11), ('rnn', 11)]
STUDY = [('rnn', 11), ('gru', 11), ('gru', 23), ('rnn', 23), ('rnn', 37), ('gru', 37)]
CELL_CAP = 1024**2
FINAL_RESERVE = 256 * 1024
CLEANUP_RESERVE = 3


def accounting_roots(config, mode):
    """Deduplicate the exact retained scope, including earlier probe evidence."""
    evidence = [Path(config['output_dir'])]
    if mode == 'study': evidence.append(ROOT / '.verification/state006-probe-01')
    owned = evidence + [ROOT / '.verification' / name for name in
                        ('state006-env', 'state006-wheelhouse-01', 'state006-setup-01',
                         'state006-environment-01.json')]
    def deduplicate(paths):
        roots = []
        for path in sorted({plain_path(p).resolve() for p in paths}, key=lambda p: len(p.parts)):
            if not any(path.is_relative_to(parent) for parent in roots): roots.append(path)
        return roots
    return deduplicate(owned), deduplicate(evidence)


def check_final_budget(deadline, owned, evidence, reserve=FINAL_RESERVE):
    if owned_bytes(owned) + reserve >= GIB2:
        raise ValueError('Aggregate owned disk reservation exhausted')
    if owned_bytes(evidence) + reserve >= EVIDENCE:
        raise ValueError('Aggregate retained evidence reservation exhausted')
    if time.monotonic() >= deadline:
        raise ValueError('Aggregate deadline reached after accounting IO')


def admit_cell(deadline, cell_deadline, nominal_seconds, *, now):
    if deadline - now < nominal_seconds + CLEANUP_RESERVE:
        raise ValueError('Full nominal cell budget plus cleanup reserve unavailable after IO')
    remaining = cell_deadline - now - 1
    if remaining < .1: raise ValueError('Cell deadline reached after IO')
    return remaining


def _json_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def verify_environment(config, tick=lambda: None):
    runtime = config['_runtime']
    env = runtime_path(runtime, 'venv')
    census_sha = digest(config['census'], tick)
    census = read_json(config['census'], 16 * 1024**2)
    if ((runtime_path(runtime, 'setup') / 'incomplete.json').exists()
            or not 0 <= census.get('setup_elapsed_seconds', 601) < 600
            or Path(config['census']).stat().st_mtime > census.get('setup_started_unix', 0) + 600):
        raise ValueError('Setup census final IO/time budget invalid')
    if (census.get('schema') != 'STATE-006-environment-v1' or census.get('complete') is not True
            or census.get('native_imported') is not False or census.get('runtime_sha256') != config['runtime_sha256']
            or census.get('source_sha256') != config['source_sha256']
            or census.get('python_version') != runtime['environment']['python_version']
            or Path(census['python']).resolve() != (env / 'Scripts/python.exe').resolve()
            or census.get('base_python_sha256') != runtime['environment']['base_python_sha256']
            or digest(census['base_python'], tick) != census['base_python_sha256']):
        raise ValueError('Environment provenance mismatch')
    expected = {re.sub('[-_.]+', '-', w['name']).lower(): w['version'] for w in runtime['wheels']}
    expected['pip'] = runtime['environment']['ensurepip_bundled_pip']
    if census.get('packages') != expected or package_versions(env) != expected:
        raise ValueError('Installed package versions differ')
    if census.get('files') != file_census(env, tick):
        raise ValueError('Installed file identity changed')
    tick()
    if digest(config['census'], tick) != census_sha:
        raise ValueError('Census changed during verification')
    identity = _json_sha(dict(census_sha256=census_sha,
                              runtime_sha256=config['runtime_sha256'], source_sha256=config['source_sha256']))
    return dict(identity=identity, python=census['python'], torch_version=runtime['selection']['torch_version'])


def seal(config):
    require_fresh([config['manifest'], config['output_dir']])
    identity = verify_environment(config)
    receipt = config.get('probe_receipt')
    receipt_sha = None
    if receipt:
        path = plain_path(receipt)
        if not path.is_absolute() or not path.resolve().is_relative_to(ROOT):
            raise ValueError('Probe receipt outside task root')
        receipt_sha = digest(path)
    if validate_config(config['_config_path'])['_config_sha256'] != config['_config_sha256']:
        raise ValueError('Config/source changed before sealing')
    manifest = dict(schema='STATE-006-review-manifest-v1', config_sha256=config['_config_sha256'],
                    environment_identity=identity['identity'], source_sha256=config['source_sha256'],
                    probe_receipt_sha256=receipt_sha, approval_claimed=False)
    write_json(config['manifest'], manifest)
    return dict(status='sealed_pending_separate_review', manifest_sha256=digest(config['manifest']))


def preflight(config, manifest_sha256, deadline):
    if not SHA.fullmatch(manifest_sha256 or ''):
        raise ValueError('Separately reviewed manifest SHA256 required')
    # Recheck file/module/config pins after every potentially long IO phase.
    checked = validate_config(config['_config_path'])
    if checked['_config_sha256'] != config['_config_sha256']:
        raise ValueError('Config changed')
    owned, evidence = accounting_roots(config, config['_mode'])
    tick = budget_monitor(deadline, owned, GIB2 - FINAL_RESERVE)
    tick()
    if digest(config['manifest'], tick) != manifest_sha256:
        raise ValueError('Review manifest pin mismatch')
    manifest = read_json(config['manifest'])
    if (manifest.get('schema') != 'STATE-006-review-manifest-v1'
            or manifest.get('config_sha256') != config['_config_sha256']
            or manifest.get('source_sha256') != config['source_sha256']):
        raise ValueError('Review manifest identity mismatch')
    identity = verify_environment(config, tick)
    if identity['identity'] != manifest.get('environment_identity'):
        raise ValueError('Environment differs from sealed review manifest')
    receipt = config.get('probe_receipt')
    if (digest(receipt, tick) if receipt else None) != manifest.get('probe_receipt_sha256'):
        raise ValueError('Probe receipt differs from reviewed manifest')
    # Hash sources again after census IO, then deadline AFTER all reads.
    for name in SOURCES:
        if digest(HERE / name, tick) != config['source_sha256'][name]:
            raise ValueError('Source changed during preflight')
    check_final_budget(deadline, owned, evidence)
    return identity


def parse_worker(raw):
    if len(raw) >= CELL_CAP:
        raise ValueError('Worker output exceeds cell bound')
    lines = raw.decode('utf-8', errors='strict').splitlines()
    while lines and not lines[-1].strip(): lines.pop()
    if not lines:
        raise ValueError('Worker produced no completed JSON')
    # CPU wheels can warn about absent optional NumPy. Permit only that known
    # warning form and its source-context continuation; unknown errors are fatal.
    prefix = lines[:-1]
    if prefix:
        warning = re.compile(r'^(?:.*:\d+: )?UserWarning: Failed to initialize NumPy: .+$')
        if not warning.fullmatch(prefix[0]) or any(
                line != '  cpu = _conversion_method_template(device=torch.device("cpu"))' for line in prefix[1:]):
            raise ValueError('Unexpected worker output before JSON')
    value = json.loads(lines[-1], parse_constant=lambda value: (_ for _ in ()).throw(ValueError('Nonfinite JSON')))
    if not isinstance(value, dict): raise ValueError('Worker JSON object required')
    return value


def check_probe(cell, model, torch_version):
    required = dict(schema='STATE-006-native-check-v1', status='complete', model=model, seed=11,
                    torch_version=torch_version, cpu_only=True, parameters=4968 if model == 'gru' else 4964,
                    shape=[2, 8], finite_gradients=True, optimizer_steps=0, native_executed=True)
    if any(type(cell.get(k)) is not type(v) or cell[k] != v for k, v in required.items()):
        raise ValueError('CPU no-update probe receipt mismatch')
    if (not SHA.fullmatch(str(cell.get('before_sha256', '')))
            or cell['before_sha256'] != cell.get('after_sha256')):
        raise ValueError('Probe changed weights or missing weight identity')


def validate_completion(path, *, controller_exit_code, controller_wall_seconds, candidate_sha256):
    """Accept only with caller-observed zero exit, whole wall time and stdout SHA.

    These arguments must come from the trusted external controller observer,
    never be inferred from a candidate file. Validation IO counts against the
    same aggregate budget. This read-only function is shared by probe and study.
    """
    began = time.monotonic()
    if (type(controller_exit_code) is not int or controller_exit_code != 0
            or type(controller_wall_seconds) not in (int, float)
            or not 0 <= controller_wall_seconds < 1200
            or not SHA.fullmatch(str(candidate_sha256))):
        raise ValueError('Trusted external zero-exit/wall-time/stdout-SHA confirmation required')
    if digest(path) != candidate_sha256: raise ValueError('Candidate differs from controller stdout SHA')
    receipt = read_json(path)
    mode = receipt.get('mode')
    if mode not in ('probe', 'study'): raise ValueError('Unknown completion mode')
    limit = 75 if mode == 'probe' else 1200
    if Path(path).resolve() != (ROOT / f'.verification/state006-{mode}-01/controller.json').resolve():
        raise ValueError('Exact canonical candidate path required')
    if (controller_wall_seconds >= limit or receipt.get('schema') != 'STATE-006-controller-v2'
            or receipt.get('status') != 'pending_confirmation' or receipt.get('operational_complete') is not True
            or receipt.get('eligible') is not False or receipt.get('error') is not None
            or receipt.get('assessment', {}).get('go') is not False
            or type(receipt.get('elapsed_seconds')) not in (int, float)
            or not 0 <= receipt['elapsed_seconds'] <= controller_wall_seconds):
        raise ValueError('Candidate is incomplete or lacks valid finalization')
    if mode == 'study':
        import state006
        assessment = state006.assess(receipt.get('cells'))
        if assessment['status'] == 'incomplete' or assessment != receipt.get('diagnostic_assessment'):
            raise ValueError('Study diagnostic assessment incomplete or changed')
    else:
        if len(receipt.get('cells', [])) != 2: raise ValueError('Both probe cells required')
        for cell, (model, seed) in zip(receipt['cells'], PROBE):
            check_probe(cell, model, '2.15.0+cpu')
        assessment = dict(status='not_applicable', go=False)
    owned, evidence = accounting_roots(dict(output_dir=str(Path(path).parent)), mode)
    if digest(path) != candidate_sha256: raise ValueError('Candidate changed during validation IO')
    check_final_budget(began + limit - controller_wall_seconds, owned, evidence, reserve=0)
    total = controller_wall_seconds + time.monotonic() - began
    if total >= limit: raise ValueError('Completion validation exceeded whole aggregate budget')
    return dict(receipt, status='complete', eligible=True, assessment=assessment,
                native_readiness='probes_complete' if mode == 'probe' else 'study_complete',
                validated_total_seconds=total)


def validate_probe_receipt(config, identity):
    confirmation = config.get('probe_confirmation')
    if not isinstance(confirmation, dict) or set(confirmation) != {'controller_exit_code', 'controller_wall_seconds', 'candidate_sha256'}:
        raise ValueError('External probe completion confirmation required')
    receipt = validate_completion(config['probe_receipt'], **confirmation)
    if (receipt.get('mode') != 'probe' or receipt.get('identity') != identity['identity']):
        raise ValueError('Both same-environment/source CPU probes required before study')
    for cell, (model, seed) in zip(receipt['cells'], PROBE):
        check_probe(cell, model, identity['torch_version'])


def execute(config, mode, manifest_sha256, guard=None):
    if mode not in ('probe', 'study'): raise ValueError('Explicit bounded mode required')
    if config.get('_mode', mode) != mode: raise ValueError('Config mode/output does not match requested action')
    if guard is None:
        from state006_guard import run_bounded
        guard = run_bounded
    started = time.monotonic()
    deadline = started + (75 if mode == 'probe' else 1200)
    output = Path(config['output_dir'])
    require_fresh([output])
    identity = preflight(config, manifest_sha256, deadline)
    if mode == 'study': validate_probe_receipt(config, identity)
    if time.monotonic() >= deadline - 3: raise ValueError('Deadline reached after preflight IO')
    output.mkdir()
    report = dict(schema='STATE-006-controller-v2', status='incomplete', mode=mode,
                  identity=identity['identity'], cells=[], assessment=None, error=None,
                  manifest_sha256=manifest_sha256, native_readiness='pending', elapsed_seconds=None,
                  eligible=False, operational_complete=False, diagnostic_assessment=None,
                  started_unix=time.time() - (time.monotonic() - started))
    owned, evidence = accounting_roots(config, mode)
    try:
        for index, (model, seed) in enumerate(PROBE if mode == 'probe' else STUDY):
            cell_started = time.monotonic()
            nominal = 30 if mode == 'probe' else 180
            cell_deadline = cell_started + nominal
            current = preflight(config, manifest_sha256, min(deadline, cell_deadline))
            if current != identity: raise ValueError('Identity changed between cells')
            if mode == 'study': validate_probe_receipt(config, identity)
            check_budget(deadline, owned)
            check_final_budget(deadline, owned, evidence, reserve=CELL_CAP + FINAL_RESERVE)
            remaining = admit_cell(deadline, cell_deadline, nominal, now=time.monotonic())
            cell = dict(schema='STATE-006-cell-v1', status='incomplete', model=model, seed=seed, metrics=None)
            report['cells'].append(cell)
            argv = ['--native-check' if mode == 'probe' else '--worker', '--model', model, '--seed', str(seed)]
            code = 'import sys;sys.path.insert(0,' + repr(str(HERE)) + ');import state006;raise SystemExit(state006.main(' + repr(argv) + '))'
            dest = output / f'{index:02d}-{model}-{seed}'
            result = guard([identity['python'], '-I', '-B', '-c', code], dest,
                           wall_seconds=remaining, total_output_limit=CELL_CAP,
                           memory_cap_bytes=GIB2, owned_paths=owned, disk_limit_bytes=GIB2 - FINAL_RESERVE,
                           direct_output_limit_bytes=CELL_CAP)
            if (result.get('status') != 'completed' or result.get('limits_verified') is not True
                    or result.get('cleanup_exit_confirmed') is not True or result.get('wall_budget_compliant') is not True):
                raise ValueError('Owned cell guard failed; terminal partial result; no retry')
            check_budget(deadline, owned)
            if time.monotonic() > cell_deadline: raise ValueError('Cell budget exceeded including setup/IO')
            with (dest / 'combined-output.bin').open('rb') as stream: raw = stream.read(CELL_CAP + 1)
            value = parse_worker(raw)
            if mode == 'probe': check_probe(value, model, identity['torch_version'])
            else:
                if (value.get('schema') != 'STATE-006-cell-v1' or value.get('status') != 'complete'
                        or value.get('model') != model or type(value.get('seed')) is not int or value['seed'] != seed
                        or value.get('torch_version') != identity['torch_version'] or value.get('native_executed') is not True):
                    raise ValueError('Study cell receipt mismatch')
                import state006
                if state006.assess([value])['failures'] or value.get('training', {}).get('updates') != 256:
                    raise ValueError('Malformed/incomplete study metrics or update count')
            if time.monotonic() > cell_deadline: raise ValueError('Cell result IO exceeded deadline')
            report['cells'][-1] = value
        if mode == 'study':
            import state006
            if state006.assess(report['cells'])['status'] == 'incomplete': raise ValueError('Assessment incomplete')
        report.update(status='pending_confirmation', operational_complete=True)
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {str(exc)[:1000]}'
    if mode == 'study':
        import state006
        report['diagnostic_assessment'] = state006.assess(report['cells'])
    report['elapsed_seconds'] = time.monotonic() - started
    if report['elapsed_seconds'] >= (75 if mode == 'probe' else 1200):
        report.update(status='incomplete', operational_complete=False, error='Aggregate deadline exceeded including IO/cleanup')
    report['assessment'] = dict(status=report['status'], go=False,
                                reason='Requires trusted external zero-exit and post-IO completion validation')
    check_final_budget(deadline, owned, evidence)
    candidate = output / 'controller.json'
    write_json(candidate, report, FINAL_RESERVE)
    # The canonical candidate NEVER confers eligibility or GO, even if this read
    # or the last budget check blocks/fails and no failure marker can be written.
    candidate_sha256 = digest(candidate)
    check_final_budget(deadline, owned, evidence, reserve=0)
    report.update(candidate_sha256=candidate_sha256, _deadline=deadline)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--validate-only', action='store_true')
    action.add_argument('--seal', action='store_true')
    action.add_argument('--execute', action='store_true')
    parser.add_argument('--mode', choices=('probe', 'study'))
    parser.add_argument('--manifest-sha256')
    args = parser.parse_args(argv)
    try:
        config = validate_config(args.config)
        if args.seal: result = seal(config)
        elif args.execute: result = execute(config, args.mode, args.manifest_sha256)
        else: result = dict(status='prepared_native_unverified', native_executed=False)
        if args.execute:
            confirmation = dict(schema='STATE-006-candidate-receipt-v1', status=result['status'],
                                eligible=False, candidate_sha256=result['candidate_sha256'])
            print(json.dumps(confirmation, sort_keys=True), flush=True)
            # The external observer must confirm this exit and WHOLE invocation
            # wall time. A candidate or stdout line alone never proves success.
            return 0 if result['operational_complete'] and time.monotonic() < result['_deadline'] else 1
        print(json.dumps(result, sort_keys=True)); return 0
    except Exception as exc:
        print(json.dumps(dict(status='failed', error=f'{type(exc).__name__}: {str(exc)[:1000]}'))); return 1


if __name__ == '__main__': raise SystemExit(main())
