# STATE-006: a small trainable persistent-state model

Status: local preparation; native import, training and publication have not run.
This protocol is an independently reviewed proposal made concrete for approval.
The earlier PREFIX-004 result qualified existing llama.cpp reuse. This study
instead trains recurrent weights and a query head from scratch on public synthetic
events. A successful toy study would justify another bounded research question;
it would not establish a language model, autonomous learning or Revia integration.

## One question and baseline

Under approximately matched trainable-parameter and matrix-work budgets, does a
standard GRU learn to retain and update bindings through distractors better than
an ordinary tanh RNN? Both have recurrent state and see identical episodes within
each paired seed. Routing, quantization and a new learning framework are excluded.

| Model | Trainable FP32 parameters | State per sequence | Dense forward MACs per episode |
| --- | ---: | ---: | ---: |
| GRU32 plus eight-class head | 4,968 | 128 B | 289,024 |
| RNN59 plus eight-class head | 4,964 | 236 B | 279,896 |

The two-bias standard cells use 15 input features and a head only at the final
query. The GRU has 3.26% more nominal forward matrix MACs, inside a declared 5%
tolerance. Parameter counts differ by four. State capacity, sigmoid/tanh work,
backpropagation, optimizer cost and actual runtime are different, not assumed
equal. Record full phase times and process memory; these counts are not CPU
instructions, measured throughput or proof of memory savings.

These are established architectures: [GRU research](https://arxiv.org/abs/1406.1078)
and the explicit [GRU](https://docs.pytorch.org/docs/2.15/generated/torch.nn.GRU.html)
and [RNN](https://docs.pytorch.org/docs/2.15/generated/torch.nn.RNN.html) cell
parameterizations. There is no architectural novelty claim.

## Fixed synthetic task and lineage

An episode contains 64 events and four keys/eight values. The first four SET
events initialize every key. A later random SET/NOOP history includes unrelated
writes and overwrites. The final QUERY asks for the latest value of one key.
The main generator places its last write at a specified lag before QUERY, with
the immediate predecessor holding a different value. Older history is sampled
independently of the answer; it must not exclude that answer from earlier writes.
Intervening writes affect other keys. An independent dictionary oracle supplies
labels. All key/answer combinations are balanced; there are no private chats,
downloaded datasets, tokenizer, pretrained weights or existing user data.

Encoding is three event-type, four key and eight value channels. Only SET exposes
value channels. Neither seed, answer, lag nor generator metadata enters the
model. The head and cross-entropy operate only at QUERY; SET copying cannot earn
accuracy. State resets to zero at each episode/batch. Persistent state here means
within the 64-event stream, not across people or application sessions.

Initialization seeds are 11, 23, 37. Training split namespaces are distinct by
phase and seed, with identical streams for the GRU/RNN pair. Each seed has 3,072
A episodes and 1,024 B episodes. Held-out A, B and OOD each contain 512 episodes;
their last-write lags are respectively 1-8, 9-16 and 17-32. Additional held-out
controls are 128 untouched-key episodes and 128 overwrite counterfactual pairs.
Group identities/content hashes and balanced marginals are verified before
training. Test metrics cannot change data, optimization or checkpoint selection.

Prepared data version is v2; manifest SHA256 is
`6327eb3ba013b47374f9f48ffb70d5fe2f4ec4710fd5adf9d30eb56c5668c581`.
The superseded, never-trained v1 manifest
`1166b60f74fee7a5d3b06adaf968d0aaa4470d384995be4eea5b7045d1093ee1`
is retained in private preparation evidence. Independent review found that v1
excluded the eventual answer from all older writes and exposed an elimination
shortcut in 21/512 OOD prefixes. A failing regression preceded the correction;
only the immediately previous queried-key value is now constrained to differ.
No result from either generator has been claimed as a trained-model measurement.

## Training and distinct forgetting checks

Both models use FP32 CPU tensors, batch16, full64-step backpropagation, Adam
learning rate0.003, weight decay0 and gradient clipping1. Input/head matrices use
Xavier initialization, recurrent gate blocks use orthogonal initialization and
biases start at zero. There is no dropout, hyperparameter search or adaptive
extra training. Each cell performs exactly192 A updates, saves checkpointA in
RAM, then64 B updates. Both checkpoint states are copied/digested independently.
No pickle or checkpoint file is required. Held-out scoring begins only after
both training phases; all selection is fixed in advance.

Within-episode retention is tested through distractors, overwrites and untouched
keys. Separately, the fixed A-to-B delay-distribution shift tests retention of an
acquired weight-level skill: evaluate frozen A and final checkpoints on held-outA.
If A never reaches95%, old-skill retention is unqualified rather than a success.
This small distribution-shift check is not general lifelong-learning validation.

Destroy hidden state immediately before OOD queries using zeroing and exchange
between independent episodes. Clean-reset prediction invariance must be100% after
unrelated episodes. Overwrite-pair accuracy requires both original and altered
last-write answers correct. The untouched-key probe retains its initial value
for60-63 steps; it is beyond the trained delay range, reported separately, and
cannot be used to select a favorable OOD bucket.

## Stop/go criteria and interpretation

All six cells must be complete, finite, correctly identified and controlled.
Absolute accuracy/state/retention gates apply to GRU, while RNN is the matched
baseline and may perform poorly. Every seed independently must satisfy:

- checkpointA held-outA and final held-outA/B at least95%;
- fixed OOD17-32 accuracy at least90%, and GRU minus paired RNN at least10pp;
- old-A accuracy loss at most2pp after B updates;
- state-zero and state-exchange accuracy at most25%, each falling at least50pp
  relative to the same frozen GRU's ordinary OOD accuracy;
- clean-reset prediction invariance100% and overwrite-pair accuracy at least95%.

These are proposed engineering thresholds, not performance predictions. Record
learned state, gating advantage and qualified weight retention separately. Both
models succeeding without a GRU advantage is useful evidence for the simpler RNN.
Poor untouched-key retention limits any wider memory claim even if other gates
pass. Underfit or exhausted caps mean the chosen configuration/budget is
inconclusive, not that the entire RNL direction has failed. A failed seed remains
visible; no retry, extra epochs, new seed or relaxed gate is automatic.

Stop on dependency/source/data/identity drift, malformed or nonfinite output,
invalid gradients, unexpected GPU/native backend, failed ownership/resource
readbacks, output/disk/time cap, missing cleanup or missing native probe. Preserve
partial/null metrics and stop later cells after terminal operational failure.
Scientific no-go values from otherwise valid complete cells are retained.

## Reproducible CPU setup and known limits

The inspected Python3.12.10 interpreter has no installed numeric/autodiff stack.
The existing CPU ranker trains softmax regression; the optional LoRA worker is
not this trainer. The selected official CPU wheel is torch2.15.0+cpu for
cp312-cp312-win_amd64. [Runtime metadata](STATE-006-runtime.json) and the separate
[download](state006-download.lock) and [offline install](state006-offline.lock)
locks pin ten wheels, all sources, SHA256 and published sizes. Payload totals
136,954,361B (130.610MiB), excluding TLS/headers and other transport overhead.
Only text metadata and one HTTP HEAD size were read; package bodies remain absent.
All extras and Linux-only CUDA/NVIDIA/triton dependencies are excluded. No NumPy,
vision/audio, Transformers, PEFT, pretrained model or GPU stack is needed.

After separately approved bounded download, inspect wheel central-directory
uncompressed sizes and paths before installation. A fresh isolated venv and
owned wheelhouse/setup roots have a monitored2GiB aggregate disk ceiling.
This is a conservative budget, not a known installed-footprint bound or an OS
filesystem quota. Abort instead of enlarging it. Use existing local bundledpip;
install offline from exact version/hash requirements with no index/dependency
resolution/cache/bytecode compilation. A direct URL can fetch despite --no-index,
so the direct-URL lock must never be used for offline installation.

The native forward/backward probe is a separately approved operation, with zero
optimizer steps and unchanged weight digest, on both standard cells. It verifies
actual CPU version/device, parameter count, logits and finite gradients. Failed
import, missing MSVC/DLL dependency or unsupported deterministic operation stops;
no dependency or security-setting repair is automatic.

Proposed setup limit is600s including inbound files/install/cleanup. Probe limits
are30s per cell and75s aggregate. The study is one invocation, six sequential
cells in order RNN11,GRU11,GRU23,RNN23,RNN37,GRU37. Each has one logical CPU,
2GiB private-commit/hard-working-set cap,180s including import/train/evaluation,
and the whole invocation has1200s including identity IO and cleanup. After all
prelaunch IO, aggregate headroom must still cover the nominal30/180-second cell
ceiling plus cleanup reserve. The cell ceiling also includes its own preflight
IO; it is never shortened to the aggregate remainder. Record initial-eight-update timing and
phase totals; extrapolations cannot enlarge budgets or establish success.
Captured evidence is bounded below10MiB, with per-cell reservations and retained
owned-directory accounting. The original run_bounded.py remains byte-for-byte
unchanged for earlier fingerprinted trials. The separately versioned
state006_guard.py pins and reuses its Win32 ABI/types while providing the reviewed
memory/disk/output policy for this study. A resource guard is
not a filesystem/network/GPU security sandbox, and controller memory is outside
worker limits. Any observed overrun is incomplete, not a compliant result.

Raw model arrays are small: weights occupy19,872/19,856B; weights, gradients and
two FP32 Adam moment arrays total79,488/79,424B before scalar/optimizer/tape
overhead. These sums exclude the framework, activations, allocator and process
memory. Actual installed footprint, native readiness, full RAM and training
throughput remain unmeasured. No promise that the180s/2GiB caps will suffice is
made without the guarded native checks and eventual measured attempt.

## Approval and publication boundary

This preparation authorizes neither installation nor execution. Exact local
source/config hashes and fresh output paths must be reviewed before any future
setup/probe/study. Seal installed-environment identity after approved setup and
recheck it plus all source hashes before every native launch; the seal flag is
not permission. A probe receipt must match the same source/environment before
the study starts. Canonical controller files remain pending/ineligible with
assessment.go=false. Only common completion validation may return an accepted
view, using trusted caller-observed zero controller exit, whole-command wall
time within its cap, and the candidate SHA emitted after final IO checks. A file
or marker alone is insufficient. The study's sealed private config binds those
three observed probe-confirmation fields. Operational failure preserves numeric
diagnostics but cannot promote the study. No installed Revia endpoint or user
model is contacted.

Local preparation remains on research/rnl-sandbox. Publishing source/evidence
requires an exact scoped request. A later outcome would use README.md,
STATE-006-status.json, RESULTS-STATE-006.md and results/STATE-006/summary.json under
research/rnl/, plus only docs/atlas/research.html on main. No other Atlas page,
milestone metric, shared layout/workflow or application source is included.
Candidate for Revia and Integrated remain None.

Two evidence-supported alternatives remain eligible for a later reviewed test:
[delta-rule associative memory](https://arxiv.org/abs/2102.11174) for repeated-key
corrections (count matrix-state/read/write overhead and ensure genuine learned
encoding), and [trainable expert routing](https://arxiv.org/abs/2101.03961), which
adds collapse/balancing and resident-parameter questions. No alternative runs
automatically. Positive and negative results guide the next single question.
