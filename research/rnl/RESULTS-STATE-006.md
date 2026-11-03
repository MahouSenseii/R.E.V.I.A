# STATE-006: setup stopped before the trainable-state test

One approved attempt downloaded and verified all ten CPU wheels, passed archive
safety checks and created an isolated virtual environment. It then stopped at
pip bootstrap: the Windows venv launcher exited101 while trying to create its
delegated base-interpreter process. PyTorch installation, both native probes and
all six neural training cells were unrun. Learning/state/gating/forgetting metrics
remain null. This is an environment-entry failure, not a measured neural result.

## What completed and what stopped

Frozen preparation is commit `de3dbdb50fb0993889e12bc581aa8fdbc732123c`,
equivalent to local `d15aedd6ecb51cb22abacba59ff798a391916c27`, tree
`fbc034c36e42a0da7aecb2af6c553db5d8d9e40a`. The [protocol](STATE-006.md)
and source remain the unchanged pre-attempt snapshot, including its status text.
No runtime fix, parameter adjustment, cap increase or second invocation followed.

The exact ten wheel files total136,954,361B. Streaming download length/SHA256
checks passed and were independently repeated after stopping. ZIP checks passed;
central-directory expanded payload totals524,894,199B. This archive sum is not
installed footprint, allocated memory or a training measurement. The selected
torch2.15.0+cpu artifact and all dependencies are pinned in the
[runtime metadata](STATE-006-runtime.json) and separate
[download](state006-download.lock)/[offline-install](state006-offline.lock) locks.
No GPU/model dataset or pretrained weights were downloaded.

The first owned process created the venv with `--without-pip` and exited0.
The second invoked its Python redirector for the locally bundled pip bootstrap;
it exited101 after reporting that its delegated process could not be created.
It did not reach pip installation. Static installed-package metadata is empty;
no complete environment census, probe output or study directory exists.

## Cause and uncertainty

The leading explanation is that the Windows venv redirector needs another
process while its owned job allows only one. The matching
[CPython launcher implementation](https://raw.githubusercontent.com/python/cpython/v3.12.10/PC/launcher.c)
emits this diagnostic on process-creation failure; Microsoft's
[job-limit documentation](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information)
describes the active-process restriction. The exact Windows error was not
retained, so this attribution remains strongly supported inference. No control
was bypassed to prove it and no launcher repair or retry was attempted.

## Costs, controls and cleanup

The outer controller exited1 after5.511 seconds including downloads, hashes,
archive checks, both launches and cleanup. This is not model loading, latency
or training throughput. The600s setup ceiling was not approached.

Both owned jobs verified one logical CPU affinity, active-process limit1 and
2GiB commit/hard-working-set limits. Maximum observed worker working set was
14,512,128B and commit9,629,696B, during setup only. Full PyTorch/model memory
remains unmeasured; the controller is outside the worker cap. The observed job
disk peak was137,507,871B and final owned disk use137,510,224B, beneath the
monitored2GiB ceiling. Disk polling is not a filesystem quota.

Both exact owned PIDs were independently absent after confirmed exits; no cleanup
errors occurred. There were no API credentials. The partial environment, verified
wheels and six raw setup files totaling4,886B remain private. The
[sanitized summary](results/STATE-006/summary.json) retains safe per-job records,
artifact hashes, null/unrun fields and the distinction between observation and
inference. No wheel binary, private path, PID or raw command is published.

Before this attempt,44 offline checks passed:21 STATE-006 controls plus23 legacy
PREFIX controls. Independent reviews caught and corrected a synthetic-data
shortcut and isolated the new resource policy so the old guard's exact fingerprint
remains intact. Those checks did not establish native Windows/PyTorch readiness.
The neural design itself remains untested in this attempt.

## Next decision and retained evidence

Review a Windows interpreter entry point that preserves the one-process limit,
then seek approval for any changed attempt. The retained wheels need no automatic
redownload, but reuse, changed code/paths and another execution must first be
reviewed. No repair, new run, training or integration is included in this outcome.

The synthetic SET/QUERY task, GRU/RNN budget comparison, three seeds and learning/
retention controls remain proposed. No language quality, general memory, adaptive
growth, speed or RAM benefit follows from the setup failure. Candidate for Revia
and Integrated remain None. Earlier positive RNL-003 toy results and PREFIX-004
real-model reuse remain intact; this environment result does not replace them.
No GPU work, private conversations, global-Python installation, security-setting
change or installed/application Revia change occurred.
