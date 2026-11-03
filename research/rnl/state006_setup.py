"""Offline preparation by default; future explicitly approved bounded setup only.

No third-party import. --execute is an action selector, never evidence of consent.
The guard is resource supervision, not a network/filesystem security sandbox.
"""
import argparse
import email.parser
import hashlib
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import time
import urllib.parse
import zipfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
GIB2 = 2 * 1024**3
EVIDENCE = 10 * 1024**2
SETUP_SECONDS = 600
SOURCES = ('state006.py', 'state006_data.py', 'state006_setup.py',
           'state006_launch.py', 'run_bounded.py', 'state006_guard.py')
SHA = re.compile(r'^[0-9a-f]{64}$')


def digest(path, tick=lambda: None):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while block := stream.read(65536):
            value.update(block); tick()
    tick()
    return value.hexdigest()


def read_json(path, limit=1024**2):
    with Path(path).open('rb') as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError('JSON exceeds bound')
    return json.loads(raw)


def write_json(path, value, limit=1024**2):
    raw = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()
    if len(raw) > limit:
        raise ValueError('JSON exceeds reserved evidence bound')
    with Path(path).open('xb') as stream:
        stream.write(raw)


def plain_path(path):
    path = Path(path)
    # Refuse existing symlinks/junctions in every component; do not follow owned
    # trees into an unrelated location. On Windows junctions are reparse points.
    for item in (path, *path.parents):
        if item.exists() and (item.is_symlink() or getattr(item.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('Reparse path refused')
    return path


def require_fresh(paths):
    for path in paths:
        path = plain_path(path)
        if path.exists():
            raise ValueError(f'Fresh target already exists: {path}')


def runtime_path(runtime, key):
    path = Path(runtime['future_paths'][key])
    if path.is_absolute() or '..' in path.parts:
        raise ValueError('Runtime paths must be task-relative')
    return plain_path(ROOT / path)


def owned_bytes(paths):
    total = 0
    for path in paths:
        path = plain_path(path)
        if not path.exists():
            continue
        items = path.rglob('*') if path.is_dir() else [path]
        for item in items:
            plain_path(item)
            if item.is_file():
                total += item.stat().st_size
                if total > GIB2:
                    raise ValueError('Owned disk cap exceeded')
    return total


def check_budget(deadline, paths, cap=GIB2, now=time.monotonic):
    if now() >= deadline:
        raise ValueError('Aggregate deadline reached')
    if owned_bytes(paths) >= cap:
        raise ValueError('Owned disk cap reached')
    if now() >= deadline:
        raise ValueError('Aggregate deadline reached after IO')
    return deadline - now()


def budget_monitor(deadline, paths, cap=GIB2):
    next_disk = 0
    def tick():
        nonlocal next_disk
        now = time.monotonic()
        if now >= deadline: raise ValueError('Aggregate deadline reached')
        if now >= next_disk:
            check_budget(deadline, paths, cap)
            next_disk = time.monotonic() + .25
        return deadline - time.monotonic()
    return tick


def require_space(path, amount):
    usage = shutil.disk_usage(path)
    free = usage.free if hasattr(usage, 'free') else usage[2]
    if free < amount + 64 * 1024**2:
        raise ValueError('Conservative available disk check failed')


def validate_config(config_path):
    config_path = Path(config_path).resolve()
    before = digest(config_path)
    config = read_json(config_path)
    if config.get('schema') != 'STATE-006-control-config-v1':
        raise ValueError('Control config schema required')
    for key in ('runtime', 'offline_lock', 'output_dir', 'census', 'manifest'):
        path = Path(config[key])
        if not path.is_absolute():
            raise ValueError('Absolute scoped config paths required')
        plain_path(path)
        if not path.resolve().is_relative_to(ROOT):
            raise ValueError('Config path outside task root')
    mode = 'study' if config.get('probe_receipt') else 'probe'
    if Path(config['output_dir']).resolve() != (ROOT / f'.verification/state006-{mode}-01').resolve():
        raise ValueError('Exact fresh probe/study output path required')
    if mode == 'study' and Path(config['probe_receipt']).resolve() != (ROOT / '.verification/state006-probe-01/controller.json').resolve():
        raise ValueError('Exact preceding probe receipt path required')
    for key in ('runtime', 'offline_lock'):
        if digest(config[key]) != config.get(key + '_sha256'):
            raise ValueError(f'{key} pin mismatch')
    pins = config.get('source_sha256', {})
    if set(pins) != set(SOURCES):
        raise ValueError('Exact controller/worker/guard source pins required')
    for name in SOURCES:
        loaded = sys.modules.get(Path(name).stem)
        if loaded is not None and Path(getattr(loaded, '__file__', '')).resolve() != (HERE / name).resolve():
            raise ValueError('Loaded module origin mismatch')
        if not SHA.fullmatch(str(pins[name])) or digest(HERE / name) != pins[name]:
            raise ValueError(f'Source pin mismatch: {name}')
    runtime = read_json(config['runtime'])
    wheels = runtime.get('wheels', [])
    if len(wheels) != 10 or sum(w['size_bytes'] for w in wheels) != 136954361:
        raise ValueError('Exact ten-wheel reviewed runtime required')
    names, files, urls = set(), set(), set()
    for wheel in wheels:
        url = urllib.parse.urlsplit(wheel['url'])
        if (url.scheme != 'https' or url.hostname not in ('files.pythonhosted.org', 'download-r2.pytorch.org')
                or url.username or url.password or url.query or url.fragment or url.port not in (None, 443)):
            raise ValueError('HTTPS wheel whitelist only')
        if (Path(wheel['filename']).name != wheel['filename'] or '/' in wheel['filename'] or '\\' in wheel['filename']
                or not wheel['filename'].endswith('.whl') or not SHA.fullmatch(wheel['sha256'])
                or type(wheel['size_bytes']) is not int or wheel['size_bytes'] <= 0):
            raise ValueError('Invalid bounded wheel metadata')
        if urllib.parse.unquote(url.path.rsplit('/', 1)[-1]) != wheel['filename']:
            raise ValueError('Wheel URL filename mismatch')
        names.add(wheel['name'].lower()); files.add(wheel['filename']); urls.add(wheel['url'])
    if len(names) != 10 or len(files) != 10 or len(urls) != 10:
        raise ValueError('Duplicate wheel identity')
    if Path(config['offline_lock']).read_text(encoding='utf-8') != runtime['requirements_offline_version_hash_lock']:
        raise ValueError('Offline lock differs from reviewed runtime')
    if '@' in runtime['requirements_offline_version_hash_lock'] or '://' in runtime['requirements_offline_version_hash_lock']:
        raise ValueError('Offline lock cannot contain URLs')
    expected = {'venv': ROOT / '.verification/state006-env',
                'wheelhouse': ROOT / '.verification/state006-wheelhouse-01',
                'setup': ROOT / '.verification/state006-setup-01',
                'future_census': Path(config['census'])}
    for key, path in expected.items():
        if runtime_path(runtime, key).resolve() != path.resolve():
            raise ValueError('Unexpected owned setup path')
    for key in ('runtime', 'offline_lock'):
        if digest(config[key]) != config[key + '_sha256']:
            raise ValueError('Runtime/lock changed during validation')
    if digest(config_path) != before:
        raise ValueError('Config changed during validation')
    config['_config_path'] = str(config_path)
    config['_config_sha256'] = before
    config['_runtime'] = runtime
    config['_mode'] = mode
    return config


class HTTPSBody:
    """One TLS request, no proxy/auth/redirect/retry; adjustable read deadline."""
    def __init__(self, url, timeout):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme != 'https' or parsed.username or parsed.password:
            raise ValueError('Authenticated/non-TLS fetch refused')
        self.url = url
        self.connection = http.client.HTTPSConnection(parsed.hostname, parsed.port or 443, timeout=timeout)
        try:
            self.connection.request('GET', parsed.path, headers={'Accept-Encoding': 'identity'})
            self.socket = self.connection.sock
            self.response = self.connection.getresponse()
            self.status, self.headers = self.response.status, self.response.headers
        except Exception:
            self.connection.close(); raise
    def geturl(self): return self.url
    def set_read_timeout(self, seconds): self.socket.settimeout(seconds)
    def read(self, amount): return self.response.read1(amount)
    def __enter__(self): return self
    def __exit__(self, *args):
        self.response.close(); self.connection.close()


def open_https(url, timeout=30):
    return HTTPSBody(url, timeout)


def fetch_wheel(wheel, target, tick):
    value, count = hashlib.sha256(), 0
    remaining = tick()
    with open_https(wheel['url'], timeout=min(30, remaining) if remaining is not None else 30) as response:
        if response.status != 200 or response.geturl() != wheel['url']:
            raise ValueError('Unexpected HTTP status or URL')
        if response.headers.get('Content-Length') != str(wheel['size_bytes']):
            raise ValueError('Exact Content-Length required')
        if response.headers.get('Content-Encoding', 'identity') != 'identity':
            raise ValueError('Encoded HTTP body refused')
        with Path(target).open('xb') as output:
            while True:
                remaining = tick()
                if hasattr(response, 'set_read_timeout'):
                    response.set_read_timeout(max(.001, min(30, remaining)))
                block = response.read(min(65536, wheel['size_bytes'] - count + 1))
                tick()
                if not block:
                    break
                count += len(block)
                if count > wheel['size_bytes']:
                    raise ValueError('Wheel exceeds approved byte count')
                output.write(block); value.update(block)
    tick()
    if count != wheel['size_bytes'] or value.hexdigest() != wheel['sha256']:
        raise ValueError('Wheel length/hash mismatch')


def wheel_payload(path):
    total, seen = 0, set()
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) > 100000:
            raise ValueError('Excessive ZIP entry count')
        for item in archive.infolist():
            name = PurePosixPath(item.filename)
            key = str(name).casefold()
            reserved = {'con', 'prn', 'aux', 'nul'} | {f'{s}{i}' for s in ('com', 'lpt') for i in range(1, 10)}
            mode = stat.S_IFMT(item.external_attr >> 16)
            if (name.is_absolute() or '..' in name.parts or '\\' in item.filename or ':' in item.filename
                    or not name.parts or any(part.rstrip(' .') != part or part.split('.')[0].lower() in reserved for part in name.parts)
                    or key in seen or item.flag_bits & 1 or mode not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError('Unsafe wheel path/link/encryption/duplicate')
            seen.add(key)
            total += item.file_size
            if total >= GIB2:
                raise ValueError('Wheel expanded payload exceeds cap')
    return total


def file_census(env, tick):
    records = []
    for path in sorted(Path(env).rglob('*')):
        plain_path(path); tick()
        if path.is_file():
            records.append([path.relative_to(env).as_posix(), path.stat().st_size, digest(path, tick)])
        if len(records) > 100000:
            raise ValueError('Environment census entry bound exceeded')
    tick()
    return records


def package_versions(env):
    versions = {}
    for path in (Path(env) / 'Lib/site-packages').glob('*.dist-info/METADATA'):
        with path.open('rb') as stream:
            raw = stream.read(1024**2 + 1)
        if len(raw) > 1024**2:
            raise ValueError('Package METADATA exceeds bound')
        metadata = email.parser.BytesParser().parsebytes(raw, headersonly=True)
        key = re.sub('[-_.]+', '-', metadata['Name']).lower()
        if key in versions:
            raise ValueError('Duplicate installed distribution')
        versions[key] = metadata['Version']
    return versions


def guarded(command, dest, deadline, owned, guard=None):
    if guard is None:
        from state006_guard import run_bounded
        guard = run_bounded
    check_budget(deadline, owned)
    # Guard has a 590s ceiling; deadline includes download/hash/IO/cleanup.
    remaining = min(590, deadline - time.monotonic() - 3)
    if remaining < .1:
        raise ValueError('No bounded command time remains')
    result = guard(command, dest, wall_seconds=remaining, total_output_limit=1024**2,
                   memory_cap_bytes=GIB2, owned_paths=owned, disk_limit_bytes=GIB2 - EVIDENCE)
    check_budget(deadline, owned)
    if (result.get('status') != 'completed' or not result.get('limits_verified')
            or result.get('cleanup_exit_confirmed') is not True or not result.get('wall_budget_compliant')):
        raise ValueError('Setup owned process failed; no retry')
    return result


def execute(config):
    started = time.monotonic()
    started_unix = time.time()
    deadline = started + SETUP_SECONDS
    checked = validate_config(config['_config_path'])
    if checked['_config_sha256'] != config['_config_sha256']:
        raise ValueError('Setup config changed')
    runtime = config['_runtime']
    env, wheels, setup = (runtime_path(runtime, key) for key in ('venv', 'wheelhouse', 'setup'))
    census = Path(config['census'])
    owned = [env, wheels, setup, census]
    if sys.version.split()[0] != runtime['environment']['python_version'] or digest(sys.executable) != runtime['environment']['base_python_sha256']:
        raise ValueError('Use exact reviewed existing Python interpreter')
    require_fresh(owned)
    require_space(ROOT, GIB2)
    # Locate bundled wheel without importing pip or executing ensurepip.
    relative = Path(runtime['environment']['bundled_pip_relative_path'])
    expected_relative = Path('Lib/ensurepip/_bundled') / ('pip-' + runtime['environment']['ensurepip_bundled_pip'] + '-py3-none-any.whl')
    if relative != expected_relative: raise ValueError('Unexpected bundled pip path')
    bundled = Path(sys.base_prefix) / relative
    if bundled.stat().st_size != runtime['environment']['ensurepip_bundled_wheel_bytes']:
        raise ValueError('Bundled pip identity differs')
    bootstrap_sha = digest(bundled)
    if bootstrap_sha != runtime['environment']['bundled_pip_sha256']:
        raise ValueError('Bundled pip hash mismatch')
    for path in (wheels, setup): path.mkdir()
    (setup / 'tmp').mkdir()
    tick = budget_monitor(deadline, owned, GIB2 - EVIDENCE)
    # Fresh paths remain as partial evidence on any failure, never silently reused.
    for wheel in runtime['wheels']:
        fetch_wheel(wheel, wheels / wheel['filename'], tick)
    payload = sum(wheel_payload(wheels / wheel['filename']) for wheel in runtime['wheels'])
    tick()
    # ZIP sums are conservative input accounting, not an installed-size guarantee.
    if owned_bytes(owned) + 2 * payload + 128 * 1024**2 + EVIDENCE >= GIB2:
        raise ValueError('Expanded wheels cannot fit conservative setup disk budget')
    require_space(ROOT, 2 * payload + 128 * 1024**2)
    base = str(Path(sys.executable).resolve())
    guarded([base, '-I', '-B', '-m', 'venv', '--without-pip', str(env)], setup / 'venv', deadline, owned)
    python = str(env / 'Scripts/python.exe')
    code = 'import sys,runpy;sys.path.insert(0,' + repr(str(bundled)) + ');runpy.run_module("pip",run_name="__main__")'
    bootstrap = [python, '-I', '-B', '-c', code, '--isolated', '--disable-pip-version-check', 'install',
                 '--no-index', '--no-deps', '--no-cache-dir', '--no-compile', str(bundled)]
    if digest(bundled, tick) != bootstrap_sha: raise ValueError('Bundled pip changed')
    guarded(bootstrap, setup / 'pip-bootstrap', deadline, owned)
    pip = [python, '-I', '-B', '-m', 'pip', '--isolated', '--disable-pip-version-check']
    guarded(pip + ['install', '--no-index', '--find-links', str(wheels), '--no-deps', '--require-hashes',
                   '--only-binary=:all:', '--no-cache-dir', '--no-compile', '-r', config['offline_lock']],
            setup / 'install', deadline, owned)
    guarded(pip + ['check'], setup / 'check', deadline, owned)
    expected = {re.sub('[-_.]+', '-', w['name']).lower(): w['version'] for w in runtime['wheels']}
    expected['pip'] = runtime['environment']['ensurepip_bundled_pip']
    versions = package_versions(env)
    if versions != expected: raise ValueError('Installed package set/version mismatch')
    files = file_census(env, tick)
    if validate_config(config['_config_path'])['_config_sha256'] != config['_config_sha256']:
        raise ValueError('Setup source/config changed during IO')
    tick()
    write_json(census, dict(schema='STATE-006-environment-v1', complete=True,
                           config_sha256=config['_config_sha256'], runtime_sha256=config['runtime_sha256'],
                           source_sha256=config['source_sha256'],
                           python=python, base_python=base, base_python_sha256=digest(base, tick),
                           python_version=sys.version.split()[0], packages=versions,
                           bundled_pip_sha256=bootstrap_sha, files=files,
                           installed_bytes=sum(f[1] for f in files), native_imported=False,
                           setup_started_unix=started_unix, setup_elapsed_seconds=time.monotonic() - started), 16 * 1024**2)
    try:
        check_budget(deadline, owned, GIB2 - EVIDENCE)
    except Exception:
        write_json(setup / 'incomplete.json', dict(status='incomplete', error='Final setup census IO exceeded budget'), 1024)
        raise
    return dict(status='setup_complete_native_unverified', census=str(census), elapsed_seconds=SETUP_SECONDS - (deadline - time.monotonic()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--config-sha256', help='externally reviewed config digest; required for --execute')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--validate-only', action='store_true')
    group.add_argument('--execute', action='store_true')
    args = parser.parse_args(argv)
    try:
        config = validate_config(args.config)
        if args.execute and args.config_sha256 != config['_config_sha256']:
            raise ValueError('Separately reviewed setup config SHA256 required')
        result = execute(config) if args.execute else dict(status='prepared_setup_unexecuted', native_imported=False)
        print(json.dumps(result, sort_keys=True)); return 0
    except Exception as exc:
        print(json.dumps(dict(status='failed', error=f'{type(exc).__name__}: {str(exc)[:1000]}'))); return 1


if __name__ == '__main__': raise SystemExit(main())
