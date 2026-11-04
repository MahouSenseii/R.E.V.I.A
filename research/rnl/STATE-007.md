# STATE-007 fixed-data learning curves implementation plan

> Historical pre-execution protocol, retained to show the planned question and controls. STATE-007 has completed; see [RESULTS-STATE-007.md](RESULTS-STATE-007.md) for the current result. This public copy omits machine-specific output paths; the original local protocol SHA-256 was 5e35fccb107c36cef4a8783468183eef659b45dc3452f21fc9240a157e9a07a6. Scientific worker, configuration and test files retain their executed/preparation bytes. The machine-specific execution controller stays private by recorded hash.

> For agentic workers: use subagent-driven-development or executing-plans task by task. Preparation is authorized; training and publication require their own applicable approval.

Goal: determine whether more optimizer updates on the same short-delay examples improve fixed-set training fit and held-out recall, while preserving STATE-006's published no-go result.

Architecture: reuse the original CPU GRU/RNN, encoding, optimizer and exact A-training prefix. Add a small curve-recording worker and a thin controller around the existing tested native resource guard. No new framework, model, dependency, architecture, telemetry or product integration.

Tech stack: existing Python3.12.10, stdlib unittest and the already-installed isolated PyTorch2.15.0+cpu. Native framework code is entered only by the explicit future worker action.

Spec: the completed STATE-006 report, raw-result SHA2564d10f420ded93e3f019a4bda8065644d3ce8050a243646c2552220de6c1327a1, and STATE-007-config.json. This is a revised proposal, not a reinterpretation of the original result.

## Why this comparison

STATE-006 finished all256 updates per cell but retained no loss or initial-accuracy curve. Final OOD recall was near the12.5% simple baseline, while A192 accuracy was27.54-31.84% for GRU. That does not establish a cause or training-induced improvement without initial measurements.

Three approaches were considered. Adding fresh A examples and more updates, as previously proposed, would change data volume and optimization together. A lag1 immediate-copy control changes task difficulty. The selected fixed-data repeat design retains the original A task and unique examples, directly testing more passes/updates before changing task or architecture. It still increases repeated exposure and compute together; it is not an optimizer-versus-exposure decomposition or an explanation of original B/OOD failure.

## Fixed scientific protocol

Use paired seeds11,23,37 and the original order RNN11,GRU11,GRU23,RNN23,RNN37,GRU37. Each cell is one fresh process and one continuous trajectory. Keep Adam state across passes. Use the exact original3,072 train_A_seed episodes and ordering, batch16, input64events/fourkeys/eightvalues, lags1-8, final-query loss, CPUFP32, deterministic algorithms, learning rate.003, weight decay0 and gradient clip1. No B phase, larger model, new seed, optimizer tuning or adaptive checkpoint selection.

Repeat the same pool in the same order six times:1,152 updates and18,432 presentations, with3,072 unique examples per cell. Whole-pass endpoints retain balance. The first192 updates must reproduce that cell's original checkpoint-A weight hash; otherwise record an invalid comparison and stop remaining cells. Do not resize the original balanced-shuffle split or regenerate a longer split with the same name.

At updates0,32,64,128,192,384,768,1152 record fixed training-pool-probe and diagnostic-validation cross-entropy/accuracy and weight hash. Online batch loss/accuracy is PRE-update; checkpoints are AFTER updateN (0is initial). Keep all1,152 online loss/accuracy/duration observations, labelled separately from fixed-set curves. Never treat duration arrays as loss.

The128-example training-pool probe selects the first four original episodes for each(key,value), in original order. It is a training subset, not held-out data, and may be measured before exposure at early checkpoints. Use a distinct balanced512-example diagnostic-validation namespace state007_diagnostic_A and final namespace state007_final_A, both seed7007 and lags1-8. Evaluation sets are shared across initializations/models for paired comparison. Final holdout is evaluated once PER CELL at1152, never used for a curve, checkpoint choice or tuning. All six cells/configurations are fixed before any result is seen.

Evaluation uses no gradients, optimizer steps or RNG operations. Restore model mode and verify weight/RNG identity around each measurement. A finite-loss/gradient check remains an operational requirement, not evidence of convergence. The dictionary oracle is a task sanity control; it is not a compute-matched learned competitor.

## Outcomes and comparisons

Report every seed/model's complete curves, initial accuracy and final-minus192 diagnostic correct-count change. Chance and deterministic query-key-only accuracy are12.5% on the balanced evaluation sets. The original RNN remains the paired approximate-capacity baseline:4,968/4,964 parameters and about3.26% different estimated forward matrix work; training compute is not identical.

For EACH MODEL separately:

- Observed consistent increase: final diagnostic correct count exceeds the count measured at update192 for all three seeds. This is not a significance claim.
- Material-improvement screen: at least52 additional correct answers out of512 (at least10percentagepoints) on every seed.
- Basic-learnability screen: at least122/128 correct on the fixed training-pool probe and461/512 on the sealed final Aholdout, for every seed.

These are proposed screening labels, not replacements for STATE-006's gates or integration GO. Report smaller increases, mixed seeds, declining loss without validation improvement, and all negative/partial results. Threshold misses do not prove no effect or architectural impossibility. No best-checkpoint or best-seed selection. Quality failure alone does not stop the remaining paired cells; malformed/nonfinite/control/resource failures do.

If training fit improves but validation does not, record a generalization gap. If both improve, qualify basic A learning before a separately reviewed delay/curriculum study. If both remain poor, source/optimization/representation questions remain open. None automatically starts another run, changes Revia or establishes persistent state across user requests.

## Global constraints

- Preparation only until exact execution approval. No publication is included.
- One active native worker, one logical CPU,2GiB private commit/hard working set per worker. Coordinator memory is outside worker cap; no whole-machine RAM claim.
-2GiB aggregate owned logical bytes across retained C: RNL/STATE-006/007 files and all existing/new D: experiment data. Keep every prior source/result unchanged; no moves, deletions or environment reset.
-10MiB combined retained/new execution evidence. Capture256KiB per worker; scratch and installed package payload count as owned disk rather than log evidence. Disk is sampled accounting, not an OS quota/security sandbox.
- One1,200-second overall window including15seconds cleanup; no new work after1185. Actual remaining global time is each worker's allowance. No overlapping phase timers, full nominal-time admission or human review pauses between machine-validated cells.
- Reuse the existing isolated CPU environment. No installation, download, GPU, private chats, live model service, global Python, security setting or application change.
- One invocation, no automatic retry/repair/cap increase. Preserve partial/null/failure evidence. A hard exit without terminal evidence is incomplete, with exact-owned cleanup still requiring verification.

Expected cost is an estimate, not a measurement: original six cells each completed native work in about19-21s, with optimization averaging4.40s/GRU and2.27s/RNN for256updates. Repeated passes and more evaluations increase work. A20-minute envelope is deliberately larger than those observations; feasibility of this new worker is unmeasured. The current environment is already installed (538,492,227B); new model weights/checkpoints remain in RAM. No new dependency/disk-copy stage is needed.

## Review focus

1. Exact original data/prefix, balanced probes and same batches across paired models; checkpoint-A mismatch invalidates the control.
2. No native import by default/stdlib tests, no early final holdout, no evaluation-induced RNG/weight/mode changes.
3. Finite typed results and exact update/curve identities; preserve failures even after a work deadline expires.
4. Overlap-free aggregate disk/evidence scopes, actual native CPU/RAM enforcement and owned-process cleanup; no access fallback.
5. Distinguish complete negative science from malformed/incomplete execution; keep prior STATE-006 files and public findings unchanged.

## Task1: reusable scientific worker and nonmodel controls

Files: create research/rnl/state007.py, test_state007.py, STATE-007-config.json and this protocol. Reuse state006.py/data unchanged.

Interfaces: validate_config(config), validation_report(config), run_worker(model,seed,config,report), assess(cells,config); explicit CLI --worker --config PATH --model MODEL --seed SEED. Default validation is stdlib-only and untrained.

- [ ] Define deterministic fixed-pool schedule, probe selection, strict config/result/assessment contracts and final-holdout admission.
- [ ] Add a small stdlib suite covering pool reuse/prefix/balance, malformed/nonfinite/missing records, final gating and independent per-model screens.
- [ ] Independently review source before any program execution.
- [ ] Run only meaningful nonmodel controls in the restricted task context; verify no Torch import or worker launch. No training during preparation.

## Task2: bounded execution preparation

Files: create private .verification/state007_run.py, STATE-007-inputs.json, STATE-007-approval-request.json and runner notes/review. These files are not public uploads.

Interfaces: exact manifest SHA and caller-observed QPC start; one --execute action guarded by the existing unmodified state006-run06 native guard. Fresh future root a task-owned SSD output root, semantic output .verification/state007-01, scratch .verification/state007-staging-01. All must be absent before the approved attempt. Code is read from this task's new research files; no staging the environment or repeating passed sampler checks.

- [ ] Freeze critical code/interpreter/config hashes, prior evidence preservation pins and existing environment census identity. Verify environment paths/sizes once, then critical pins between cells; disclose same-size post-census limitations.
- [ ] Freeze one exact supported require_escalated invocation, with no persistent approval prefix and unconditional bounded local capture even if initialization fails.
- [ ] Independently review the controller/resource/preservation/failure paths and nonmodel controls.
- [ ] Return exact paths, hashes, limits and approval boundary. Do not execute training or publish.

## Current status and next authorization

Prepared locally; no STATE-007 training/probe/evaluation/native framework execution has run. Proposed claims remain unmeasured. Review/tests and exact bundle identity are recorded in the private preparation handoff rather than changing this historical protocol after a run. Original STATE-006 no-go and all earlier positive/negative evidence remain intact.

Next authorization would cover one reviewed CPU-only batch, the new D: output path and bounded local results/cleanup. No dependency installation or public upload is needed to run it. Publication, app integration and any subsequent experiment remain separate decisions.
