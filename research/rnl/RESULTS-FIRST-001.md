# FIRST-001 results

**Measured, toy scope only.** Packed, byte-indexed and FP32 representations
preserved this network's numerical trajectories. The supplied cache changed
recurrent behavior. No process-memory saving or reliable speed improvement was
established. Nothing is a Candidate for Revia or Integrated.

The [sanitized evidence summary](results/FIRST-001/summary.json) accompanies the
[protocol](FIRST-001.md). The original archive and source were preserved; the
experiment used a separate instrumented worker, not a replacement Revia backend.

## Inputs and method

The original archive was 15,706 bytes, SHA-256
`a750f92354510db6337bb57d589d010ccc8caf8d5ec76aee407984e069678208`.
Its reviewed `src/rnl.cpp` SHA-256 was
`d5ee18bbe9df39eb9d99b571c93ae1f0bbeec97bf19b387587a71d4563d61b0f`.
The final [instrumented worker](first001.cpp) SHA-256 was
`83a19e05c5fc4102d1cd96e02e7b63c31cfe4361677dce6baf7247441eb71575`.

F used decoded FP32 matrices, U byte-sized codebook indices, and P packed indices
retained in RAM. C used U's representation with the supplied inclusive novelty
cache. Exact repeats and near repeats were separate input cases, not invented
cache implementations. Each variant covered 236 steps: eight cases, each reset
from zero and the fixed nonzero state `[0.25,-0.5,0.75]`. Cases included zeros,
repeats, alternating inputs, returns to zero, representable distances below/at/
above the threshold, and router choices/ties.

The independent FP32 oracle passed all **six tests**, exit 0. It checked the
frozen corpus, full trajectories and serialized containers. U and P matched F
with **zero maximum trajectory error and zero differing float bit patterns**.
These are results for authored weights in one tiny network; no quantization
learning or language task was tested.

## Storage and cache findings

| Representation | Complete model file | Matrix payload | Codebook | Resident matrix capacity |
| --- | ---: | ---: | ---: | ---: |
| F: experimental FP32 container | 200 B | 108 B | 0 B | 108 B |
| U: experimental byte-index container | 131 B | 27 B | 12 B | 27 B |
| P: original RNL1 container, packed runtime | 113 B | 9 B | 12 B | 9 B |
| C: byte indices with supplied cache | 131 B | 27 B | 12 B | 27 B |

Every complete file includes 36 routing bytes and 56 header/name bytes. P's
matrix payload includes 18 padding bits. Its file matched the original sample
byte-for-byte. F omits its unused codebook; F and U are experimental container
schemas, not formats supported by the original CLI. The original loader expands
the packed indices; the new packed-runtime result belongs to the instrumented
worker, not to the unchanged original loader.

Payload capacities are not total runtime memory. The measured object-capacity
figures were 480 B for F/P and 408 B for U/C, before matrix/codebook/gate buffers,
state, scratch space and unattributed allocator/string costs. Instrumentation,
serialization buffers and transient parsing also consume process memory. A
smaller matrix did not establish a smaller process.
Adding the reported capacity components gives F 720 B, U 579 B and P 633 B;
P is 54 B above U because its bookkeeping costs more than the 18 B saved in
matrix indices here. These partial sums still exclude unattributed memory.

The cache diverged from recomputing the recurrence in **9 of 16 case/state
combinations**. On the second `[1,0,0]` input from zero state, the cached first
state component remained `0.342717379`; recomputation produced `0.546377659`.
This rejects the cache as an equivalent optimization of that recurrence. It
does not invalidate the no-cache representation results, or hide the original's
documented decision to skip repeated events.

A separate 3-input, 1-state, 3-expert fixture reproduced the inspector's packing
accounting error: the original CLI reported 4 matrix-payload bytes while the
actual per-expert payload was 3 bytes; the complete file was 107 bytes. No
original-source fix was applied. Invalid-index and truncated-model controls
returned the expected error exit 2. The original CLI smoke run showed its
expected cache behavior, but its three-decimal output was not used as the
numeric oracle.

## Timing, resources and limits

After the correctness gate, five rotating/interleaved batches per F/U/P variant
used 100,000 event steps each, with an excluded warm-up and identical batch
resets. Timed work included event computation and a checksum; trace formatting
and VM construction were outside the measured step loop.

| Variant | Median ns/step | Observed batch range, ns/step |
| --- | ---: | ---: |
| F | 277.237 | 269.362–300.042 |
| U | 279.393 | 273.390–282.524 |
| P | 279.987 | 274.368–284.640 |

The ranges overlap. Five batches on this toy do not establish a reliable speed
gain. The separate load samples measured **in-memory container parsing and
allocation only**; disk I/O and full disk-load latency remain unmeasured.
Other host workloads were not paused or inspected; these are not isolated
performance-qualification results.

The main attempt contained 26 supervised launches and completed in 4.344 seconds.
Its recorded artifacts occupied 2,516,328 bytes before adding the 16,070-byte
derived summary, or 2,532,398 bytes including that summary. Optional supervisor
and smoke probes were retained separately. Verified worker controls included
one-logical-CPU affinity, owned-process/job limits, 256 MiB working-set and
committed-memory caps, deadlines and bounded outputs. No cap breach was recorded.

Timed-worker peak working sets ranged from 4,255,744 to 4,268,032 bytes; the
largest worker peak across the main attempt was 4,272,128 bytes. These are full
instrumented-process peaks including parsing and evidence buffers, not pure model
RSS. They do not establish a process-memory benefit. Allocator/string and
`stable_sort` overhead were not separately attributed. Hard working-set limits
may affect timing, affinity does not prove a single OS thread, and the supervisor
is a resource guard rather than a network/filesystem/GPU security sandbox.
An empty-worker RSS baseline and empirical timer resolution were not measured;
the separate argument-refusal probe is not a substitute. Full runtime-memory
accounting and disk-load qualification therefore remain incomplete.

Earlier supervisor setup failures/errors and expected negative controls remain
in the evidence. The initial worker build's signed/unsigned warning was fixed;
the final build retained six inherited size-conversion warnings from the original
source. These are not erased by the passing numeric checks. No training, model
download, GPU workload or unrelated Revia execution was part of FIRST-001.

This experiment supports a measured toy result and a concrete cache/accounting
counterexample. It establishes no Qwen/llama.cpp compatibility, language quality,
VRAM saving, production readiness or safe self-training. Any later candidate
needs a separate proposal and review using the existing
[evaluation gate](../../docs/EVALUATION_GATE.md) and, where applicable,
[offline-learning lineage rules](../../docs/OFFLINE_TRAINING.md), followed by
explicit authorization and rollback.

## Reproduction boundary

The original ZIP/source hashes above are required; the source is not vendored.
The worker includes the exact reviewed original `rnl.cpp` through an external
include directory. Build it in an owned scratch directory from an MSVC developer
shell with `/std:c++20 /EHsc /W4 /O2 /fp:strict` and that original `src` directory
on `/I`. Run only under the reviewed [Windows resource guard](run_bounded.py),
using fresh output directories and the documented aggregate result budget.

The retained numeric/serialized evidence can be checked without executing the
prototype or benchmark:

```text
python research/rnl/check_first001.py --model research/rnl/results/FIRST-001/P/model.rnb --results research/rnl/results/FIRST-001
```
