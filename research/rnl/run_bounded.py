"""Windows-only bounded launcher for one reviewed, task-owned worker.

This is resource supervision, not a filesystem/network/GPU security sandbox.
Workers must be reviewed to avoid those operations and direct file writes.
Never attaches existing processes. Uses only stdlib and Windows APIs.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

MEMORY_CAP = 256 * 1024 * 1024
OUTPUT_CAP = 10 * 1024 * 1024
RESERVE = 16384
SIZE = C.c_size_t
U64 = C.c_ulonglong


class Security(C.Structure):
    _fields_ = [('length', W.DWORD), ('descriptor', W.LPVOID), ('inherit', W.BOOL)]


class Startup(C.Structure):
    _fields_ = [('cb', W.DWORD), ('reserved', W.LPWSTR), ('desktop', W.LPWSTR),
                ('title', W.LPWSTR), ('x', W.DWORD), ('y', W.DWORD),
                ('xsize', W.DWORD), ('ysize', W.DWORD), ('xchars', W.DWORD),
                ('ychars', W.DWORD), ('fill', W.DWORD), ('flags', W.DWORD),
                ('show', W.WORD), ('reserved_size', W.WORD), ('reserved_ptr', W.LPVOID),
                ('stdin', W.HANDLE), ('stdout', W.HANDLE), ('stderr', W.HANDLE)]


class ProcessInfo(C.Structure):
    _fields_ = [('process', W.HANDLE), ('thread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]


class BasicLimits(C.Structure):
    _fields_ = [('process_time', C.c_longlong), ('job_time', C.c_longlong),
                ('flags', W.DWORD), ('min_ws', SIZE), ('max_ws', SIZE),
                ('processes', W.DWORD), ('affinity', SIZE),
                ('priority', W.DWORD), ('scheduling', W.DWORD)]


class IOCounters(C.Structure):
    _fields_ = [(name, U64) for name in ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]


class ExtendedLimits(C.Structure):
    _fields_ = [('basic', BasicLimits), ('io', IOCounters), ('process_memory', SIZE),
                ('job_memory', SIZE), ('peak_process_memory', SIZE), ('peak_job_memory', SIZE)]


class MemoryCounters(C.Structure):
    _fields_ = [('cb', W.DWORD), ('faults', W.DWORD)] + [(name, SIZE) for name in
                ('peak_ws', 'ws', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged',
                 'commit', 'peak_commit', 'private')]


def _apis():
    kernel = C.WinDLL('kernel32', use_last_error=True)
    memory = C.WinDLL('psapi', use_last_error=True)
    signatures = {
        'CreatePipe': ([C.POINTER(W.HANDLE), C.POINTER(W.HANDLE), C.POINTER(Security), W.DWORD], W.BOOL),
        'SetHandleInformation': ([W.HANDLE, W.DWORD, W.DWORD], W.BOOL),
        'CreateFileW': ([W.LPCWSTR, W.DWORD, W.DWORD, C.POINTER(Security), W.DWORD, W.DWORD, W.HANDLE], W.HANDLE),
        'CreateProcessW': ([W.LPCWSTR, W.LPWSTR, W.LPVOID, W.LPVOID, W.BOOL, W.DWORD, W.LPVOID, W.LPCWSTR, C.POINTER(Startup), C.POINTER(ProcessInfo)], W.BOOL),
        'CreateJobObjectW': ([W.LPVOID, W.LPCWSTR], W.HANDLE),
        'SetInformationJobObject': ([W.HANDLE, C.c_int, W.LPVOID, W.DWORD], W.BOOL),
        'QueryInformationJobObject': ([W.HANDLE, C.c_int, W.LPVOID, W.DWORD, C.POINTER(W.DWORD)], W.BOOL),
        'AssignProcessToJobObject': ([W.HANDLE, W.HANDLE], W.BOOL),
        'TerminateJobObject': ([W.HANDLE, W.UINT], W.BOOL),
        'TerminateProcess': ([W.HANDLE, W.UINT], W.BOOL),
        'GetCurrentProcess': ([], W.HANDLE),
        'GetProcessAffinityMask': ([W.HANDLE, C.POINTER(SIZE), C.POINTER(SIZE)], W.BOOL),
        'SetProcessAffinityMask': ([W.HANDLE, SIZE], W.BOOL),
        'GetProcessWorkingSetSizeEx': ([W.HANDLE, C.POINTER(SIZE), C.POINTER(SIZE), C.POINTER(W.DWORD)], W.BOOL),
        'SetProcessWorkingSetSizeEx': ([W.HANDLE, SIZE, SIZE, W.DWORD], W.BOOL),
        'ResumeThread': ([W.HANDLE], W.DWORD),
        'WaitForSingleObject': ([W.HANDLE, W.DWORD], W.DWORD),
        'GetExitCodeProcess': ([W.HANDLE, C.POINTER(W.DWORD)], W.BOOL),
        'PeekNamedPipe': ([W.HANDLE, W.LPVOID, W.DWORD, C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.POINTER(W.DWORD)], W.BOOL),
        'ReadFile': ([W.HANDLE, W.LPVOID, W.DWORD, C.POINTER(W.DWORD), W.LPVOID], W.BOOL),
        'CloseHandle': ([W.HANDLE], W.BOOL),
    }
    for name, (args, result) in signatures.items():
        func = getattr(kernel, name)
        func.argtypes, func.restype = args, result
    memory.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(MemoryCounters), W.DWORD]
    memory.GetProcessMemoryInfo.restype = W.BOOL
    return kernel, memory


def _ok(value, operation):
    if not value:
        raise OSError(C.get_last_error(), operation)
    return value


def run_bounded(command, output_dir, *, wall_seconds=580, total_output_limit=OUTPUT_CAP):
    """Return and retain one result; never reuse/overwrite an evidence directory.

    Limits apply to worker; supervisor memory is separately recorded. The worker
    must use stdout/stderr only for results. OS scheduling can delay cleanup;
    an overrun is reported as incomplete, never a compliant success.
    """
    if os.name != 'nt':
        raise RuntimeError('Windows APIs required; no weaker fallback')
    if not command or not Path(command[0]).is_absolute() or not all(isinstance(x, str) for x in command):
        raise ValueError('Pass a reviewed argv with an absolute executable; no shell')
    if len(subprocess.list2cmdline(command)) > 8192:
        raise ValueError('Command too long for bounded evidence')
    if not 0.1 <= wall_seconds <= 590 or not RESERVE + 1024 <= total_output_limit <= OUTPUT_CAP:
        raise ValueError('Caps must fit inside 600 seconds and 10 MiB')
    began = time.monotonic()
    destination = Path(output_dir).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    result = {
        'status': 'control_failed', 'command': command,
        'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'pid': None, 'exit_code': None, 'limits_verified': False,
        'wall_limit_seconds': wall_seconds, 'memory_cap_bytes': MEMORY_CAP,
        'total_output_limit_bytes_exclusive': total_output_limit,
        'metadata_reserve_bytes': RESERVE, 'captured_output_bytes': 0,
        'output_truncated': False, 'terminated_owned_job': False,
        'cleanup_exit_confirmed': None, 'direct_file_writes_capped': False,
        'peak_working_set_bytes': 0, 'peak_committed_bytes': 0,
        'affinity_mask': None, 'errors': [],
        'limitations': ['Resource supervisor, not network/filesystem/GPU security sandbox',
                        'Output cap covers captured stream plus this result only; reviewed direct writes need separate preflight/aggregate accounting',
                        'Single logical CPU affinity does not prove a single OS thread',
                        'Hard working-set limits may affect timing',
                        'OS scheduling may delay termination; overruns are incomplete'],
        'supervisor_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'python_version': sys.version.split()[0],
    }
    # JSON escaping can greatly exceed argv character count. Refuse before any
    # child exists and retain the refusal within the same metadata reservation.
    if len(json.dumps(result).encode('utf-8')) > RESERVE - 4096:
        result['command_sha256'] = hashlib.sha256(json.dumps(command).encode('utf-8')).hexdigest()
        result['command'] = None
        result['errors'].append('Command metadata exceeds pre-launch reservation')
        (destination / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        return result
    try:
        kernel, memory = _apis()
    except Exception as exc:
        result['errors'].append(f'API loading failed: {type(exc).__name__}: {str(exc)[:512]}')
        (destination / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        return result
    handles = []
    info = ProcessInfo()
    job = None
    assigned = False
    stream = None

    def close(handle):
        if handle:
            kernel.CloseHandle(handle)

    def sample(handle, own_worker=True):
        counters = MemoryCounters()
        counters.cb = C.sizeof(counters)
        _ok(memory.GetProcessMemoryInfo(handle, C.byref(counters), C.sizeof(counters)), 'GetProcessMemoryInfo')
        if own_worker:
            result['peak_working_set_bytes'] = max(result['peak_working_set_bytes'], counters.peak_ws)
            result['peak_committed_bytes'] = max(result['peak_committed_bytes'], counters.peak_commit)
            result['last_working_set_bytes'] = counters.ws
            if counters.peak_ws > MEMORY_CAP or counters.peak_commit > MEMORY_CAP:
                raise RuntimeError('Worker exceeded configured memory cap')
        else:
            result['supervisor_peak_working_set_bytes'] = counters.peak_ws

    def terminate():
        if not info.process:
            return
        if assigned:
            _ok(kernel.TerminateJobObject(job, 0xE001), 'TerminateJobObject(owned job)')
            result['terminated_owned_job'] = True
        else:
            _ok(kernel.TerminateProcess(info.process, 0xE002), 'TerminateProcess(own suspended child)')
            result['terminated_suspended_child'] = True
        wait = kernel.WaitForSingleObject(info.process, 2000)
        result['cleanup_exit_confirmed'] = wait == 0
        if wait != 0:
            result['errors'].append('Owned-child cleanup wait did not confirm exit')

    try:
        sample(kernel.GetCurrentProcess(), False)
        process_mask, system_mask = SIZE(), SIZE()
        _ok(kernel.GetProcessAffinityMask(kernel.GetCurrentProcess(), C.byref(process_mask), C.byref(system_mask)), 'GetProcessAffinityMask(supervisor)')
        one_cpu = process_mask.value & -process_mask.value
        if not one_cpu:
            raise RuntimeError('No allowed logical CPU')
        result['affinity_mask'] = one_cpu
        job = _ok(kernel.CreateJobObjectW(None, None), 'CreateJobObjectW')
        handles.append(job)
        limits = ExtendedLimits()
        # Active process = 1, one CPU, no breakaway, 256 MiB private commit,
        # and close-last-job-handle kills only this owned job.
        limits.basic.flags = 0x8 | 0x10 | 0x100 | 0x2000
        limits.basic.processes = 1
        limits.basic.affinity = one_cpu
        limits.process_memory = MEMORY_CAP
        _ok(kernel.SetInformationJobObject(job, 9, C.byref(limits), C.sizeof(limits)), 'SetInformationJobObject')
        readback = ExtendedLimits()
        _ok(kernel.QueryInformationJobObject(job, 9, C.byref(readback), C.sizeof(readback), None), 'QueryInformationJobObject')
        if (readback.basic.flags & limits.basic.flags) != limits.basic.flags or readback.basic.processes != 1 or readback.basic.affinity != one_cpu or readback.process_memory != MEMORY_CAP or readback.basic.flags & (0x800 | 0x1000):
            raise RuntimeError('Job limit readback mismatch')
        result['job_limit_flags'] = readback.basic.flags
        result['job_process_memory_limit_bytes'] = readback.process_memory
        sa = Security(C.sizeof(Security), None, True)
        read_handle, write_handle = W.HANDLE(), W.HANDLE()
        _ok(kernel.CreatePipe(C.byref(read_handle), C.byref(write_handle), C.byref(sa), 4096), 'CreatePipe')
        handles.extend([read_handle, write_handle])
        _ok(kernel.SetHandleInformation(read_handle, 1, 0), 'SetHandleInformation(pipe)')
        null_input = kernel.CreateFileW('NUL', 0x80000000, 3, C.byref(sa), 3, 0, None)
        if null_input == C.c_void_p(-1).value:
            raise OSError(C.get_last_error(), 'CreateFileW(NUL)')
        handles.append(null_input)
        startup = Startup()
        startup.cb = C.sizeof(startup)
        startup.flags = 0x100 | 1
        startup.stdin, startup.stdout, startup.stderr = null_input, write_handle, write_handle
        startup.show = 0
        environment = {key: os.environ[key] for key in ('SystemRoot', 'WINDIR') if key in os.environ}
        environment.update(TEMP=str(destination), TMP=str(destination), OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1')
        env_buffer = C.create_unicode_buffer('\0'.join(f'{k}={v}' for k, v in sorted(environment.items(), key=lambda pair: pair[0].upper())) + '\0\0')
        commandline = C.create_unicode_buffer(subprocess.list2cmdline(command))
        _ok(kernel.CreateProcessW(command[0], commandline, None, None, True,
                                  0x4 | 0x400 | 0x08000000, env_buffer,
                                  str(destination), C.byref(startup), C.byref(info)), 'CreateProcessW(own suspended child)')
        handles.extend([info.process, info.thread])
        result['pid'] = info.pid
        close(write_handle)
        handles.remove(write_handle)
        _ok(kernel.AssignProcessToJobObject(job, info.process), 'AssignProcessToJobObject')
        assigned = True
        _ok(kernel.SetProcessAffinityMask(info.process, one_cpu), 'SetProcessAffinityMask(own child)')
        child_mask, child_system = SIZE(), SIZE()
        _ok(kernel.GetProcessAffinityMask(info.process, C.byref(child_mask), C.byref(child_system)), 'GetProcessAffinityMask(own child)')
        if child_mask.value != one_cpu:
            raise RuntimeError('Worker affinity readback mismatch')
        minimum, maximum, flags = SIZE(), SIZE(), W.DWORD()
        _ok(kernel.GetProcessWorkingSetSizeEx(info.process, C.byref(minimum), C.byref(maximum), C.byref(flags)), 'GetProcessWorkingSetSizeEx')
        if minimum.value > MEMORY_CAP:
            raise RuntimeError('Existing worker minimum working set exceeds cap')
        _ok(kernel.SetProcessWorkingSetSizeEx(info.process, minimum.value, MEMORY_CAP, 0x4 | 0x2), 'SetProcessWorkingSetSizeEx(hard max)')
        _ok(kernel.GetProcessWorkingSetSizeEx(info.process, C.byref(minimum), C.byref(maximum), C.byref(flags)), 'GetProcessWorkingSetSizeEx(readback)')
        if maximum.value > MEMORY_CAP or not flags.value & 0x4:
            raise RuntimeError('Hard working-set limit readback mismatch')
        result['working_set_max_bytes'] = maximum.value
        result['working_set_flags'] = flags.value
        sample(info.process)
        result['limits_verified'] = True
        stream = (destination / 'combined-output.bin').open('xb')
        margin = min(2.0, wall_seconds * 0.2)
        worker_deadline = began + wall_seconds - margin
        result['termination_margin_seconds'] = margin
        if time.monotonic() >= worker_deadline:
            result['status'] = 'timeout'
            terminate()
        else:
            if kernel.ResumeThread(info.thread) == 0xFFFFFFFF:
                raise OSError(C.get_last_error(), 'ResumeThread')
            result['status'] = 'running'
            result['resumed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            log_limit = total_output_limit - RESERVE - 1
            last_sample = 0.0
            while True:
                now = time.monotonic()
                if now >= worker_deadline:
                    result['status'] = 'timeout'
                    terminate()
                    break
                if now - last_sample >= 0.01:
                    sample(info.process)
                    last_sample = now
                available = W.DWORD()
                peeked = kernel.PeekNamedPipe(read_handle, None, 0, None, C.byref(available), None)
                if not peeked and C.get_last_error() != 109:
                    raise OSError(C.get_last_error(), 'PeekNamedPipe')
                if available.value:
                    remaining = log_limit - result['captured_output_bytes']
                    amount = min(16384, available.value, remaining)
                    if amount:
                        buffer = C.create_string_buffer(amount)
                        read_count = W.DWORD()
                        _ok(kernel.ReadFile(read_handle, buffer, amount, C.byref(read_count), None), 'ReadFile(owned pipe)')
                        stream.write(buffer.raw[:read_count.value])
                        result['captured_output_bytes'] += read_count.value
                    if available.value > remaining:
                        result['status'] = 'output_limit'
                        result['output_truncated'] = True
                        terminate()
                        break
                else:
                    waited = kernel.WaitForSingleObject(info.process, 0)
                    if waited == 0:
                        code = W.DWORD()
                        _ok(kernel.GetExitCodeProcess(info.process, C.byref(code)), 'GetExitCodeProcess')
                        result['exit_code'] = code.value
                        result['cleanup_exit_confirmed'] = True
                        result['status'] = 'completed' if code.value == 0 else 'worker_failed'
                        break
                    if waited != 258:
                        raise OSError(C.get_last_error(), 'WaitForSingleObject')
                    time.sleep(0.005)
    except (Exception, KeyboardInterrupt) as exc:
        result['errors'].append(f'{type(exc).__name__}: {exc}')
        result['status'] = 'control_failed'
        try:
            terminate()
        except Exception as cleanup:
            result['errors'].append(f'Cleanup: {cleanup}')
    finally:
        if info.process:
            try:
                sample(info.process)
            except Exception as exc:
                result['errors'].append(f'Final memory sample: {exc}')
                if result['status'] == 'completed':
                    result['status'] = 'incomplete_measurement'
            code = W.DWORD()
            if kernel.GetExitCodeProcess(info.process, C.byref(code)):
                result['exit_code'] = code.value
                if code.value == 259:
                    result['status'] = 'cleanup_unconfirmed'
            else:
                result['errors'].append('GetExitCodeProcess final failed')
        if stream:
            stream.close()
        for handle in reversed(handles):
            close(handle)
        try:
            sample(kernel.GetCurrentProcess(), False)
        except Exception as exc:
            result['errors'].append(f'Final supervisor sample: {exc}')
            if result['status'] == 'completed':
                result['status'] = 'incomplete_measurement'
    result['elapsed_seconds'] = time.monotonic() - began
    result['wall_budget_compliant'] = result['elapsed_seconds'] <= wall_seconds
    if not result['wall_budget_compliant'] and result['status'] == 'completed':
        result['status'] = 'wall_overrun'
    result['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    result_bytes = (json.dumps(result, indent=2) + '\n').encode('utf-8')
    if len(result_bytes) > RESERVE:
        result['command_sha256'] = hashlib.sha256(json.dumps(command).encode('utf-8')).hexdigest()
        result['command'] = None
        result['errors'] = [str(error)[:256] for error in result['errors'][:8]]
        result['errors'].append('Metadata abbreviated to preserve reserved evidence space')
        if result['status'] == 'completed':
            result['status'] = 'incomplete_metadata'
        result_bytes = (json.dumps(result, indent=2) + '\n').encode('utf-8')
        if len(result_bytes) > RESERVE:
            raise RuntimeError('Minimal metadata unexpectedly exceeds reserve; partial logs retained')
    (destination / 'result.json').write_bytes(result_bytes)
    # This supervisor cannot impose a filesystem quota. The separately reviewed
    # harness must bound direct results before writing and account for them with
    # these retained stdout/stderr artifacts under the experiment-wide budget.
    captured = destination / 'combined-output.bin'
    if (captured.stat().st_size if captured.exists() else 0) + len(result_bytes) >= total_output_limit:
        raise RuntimeError('Captured stream plus result exceeded budget')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--wall-seconds', type=float, default=580)
    parser.add_argument('--total-output-limit', type=int, default=OUTPUT_CAP)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    argv = args.command[1:] if args.command[:1] == ['--'] else args.command
    outcome = run_bounded(argv, args.output_dir, wall_seconds=args.wall_seconds, total_output_limit=args.total_output_limit)
    print(json.dumps(outcome, indent=2))
    raise SystemExit(0 if outcome['status'] == 'completed' else 1)
