# RNL-003 results

**Measured synthetic CPU benefit under exact repetition.** Correct raw-product
reuse reduced the complete event time in all eight fixed repeat conditions,
with median paired U/M ratios from 1.213 to 3.994. All four zero-repeat controls
had zero hits and no benefit. U and M matched every numeric component across
2,400 validation steps. This is a conditional toy runtime result, not evidence
of faster Revia, lower RAM, learned intelligence or language-model quality.
No Candidate for Revia or Integrated promotion.

The [protocol](RNL-003.md) was committed as
`94ce9ea25546bd97228091b7d4ce81a4e332abee` before implementation/execution.
The [evidence summary](results/RNL-003/summary.json) links the unchanged raw
results, source/binary hashes, accounting and supervision records. One reviewed
attempt ran; no timing retries or tuning followed the results.

## Complete event timings

U always recomputed the selected byte-index matrix product. M used the RNL-002
contract: an immutable model, one entry keyed by expert index and every FP32
input bit, retaining the raw sum by value. Both modes ran the same router,
activation and recurrent update every time. A common non-inlined event routine
and matrix kernel kept the representation and accumulation order identical.
All three experts remained resident.

The entire timed batch includes routing, key lookup/comparison, input/product
copies, decoding/multiply on misses, tanh, state update and the common scalar
checksum. It excludes generation, VM construction/reset, validation, memory API
queries, final hashing and result serialization. Storage/scratch was preallocated.
Timings include M's key-comparison counters; their incremental cost was not
isolated. There is no per-MAC counter in the timed kernel. This is an ordinary synthetic implementation, not
a tuned kernel or original CLI latency.

| Square D | Repeated events | Events/batch | Actual M hits | U event us min/median/max | M event us min/median/max | Median paired U/M | Decision |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 | 0% | 100,000 | 0.00% | 0.374/0.376/0.381 | 0.392/0.396/0.400 | 0.954 | No benefit |
| 16 | 50% | 100,000 | 46.00% | 0.377/0.382/0.387 | 0.311/0.314/0.316 | 1.213 | Benefit on this corpus |
| 16 | 90% | 100,000 | 81.00% | 0.357/0.361/0.364 | 0.231/0.234/0.236 | 1.547 | Benefit on this corpus |
| 64 | 0% | 13,000 | 0.00% | 3.892/3.927/4.090 | 3.910/4.002/4.103 | 0.983 | No benefit |
| 64 | 50% | 13,100 | 46.00% | 3.797/3.824/3.901 | 2.623/2.643/2.648 | 1.451 | Benefit on this corpus |
| 64 | 90% | 14,200 | 82.99% | 3.626/3.701/3.840 | 1.430/1.446/1.455 | 2.569 | Benefit on this corpus |
| 256 | 0% | 1,000 | 0.00% | 49.880/50.521/51.117 | 50.968/51.098/51.697 | 0.979 | No benefit |
| 256 | 50% | 1,100 | 41.00% | 50.628/51.320/52.282 | 31.680/32.272/32.744 | 1.590 | Benefit on this corpus |
| 256 | 90% | 1,100 | 71.00% | 50.077/50.473/53.424 | 17.703/18.133/19.356 | 2.789 | Benefit on this corpus |
| 1024 | 0% | 100 | 0.00% | 769.400/773.334/781.597 | 775.554/778.593/795.661 | 0.991 | No benefit |
| 1024 | 50% | 100 | 43.00% | 772.523/775.938/784.261 | 448.448/450.524/456.353 | 1.719 | Benefit on this corpus |
| 1024 | 90% | 100 | 77.00% | 770.553/773.327/789.611 | 189.800/193.596/195.861 | 3.994 | Benefit on this corpus |

The repeated-event column is equal repeats/N: blocks of 1, 2 or 10 in a fixed
100-event corpus. Equal adjacent pairs/(N-1) have a different denominator.
Routing can select a different expert while input repeats, so hit rate is below
the repeated-event fraction. Timing cycles can have different hit rates from
the 100-event memory warmup as recurrent state settles.

U-only calibration targeted 50ms, within 100,000 events and 200 million matrix
terms per batch. The cap stopped D16 at 100,000 and D1024 at 100 events.
One same-size warmup per mode preceded five alternating paired measurements.
The median paired ratio is the median of five U-time/M-time ratios, not the
ratio of separately rounded medians. Variability and every raw tick are retained.

All 12 scenarios met the predeclared resolution rule. QPC frequency was
10,000,000 Hz; the smallest observed positive read gap among 2,000 reads was
one tick (100ns) in each worker. The shortest comparison batch was
18.980ms. This empirical gap is a read-based check, not a
claim about CPU instruction resolution. Every supported repeat case had median
paired U/M >=1.10, every pair >1.05 and max(M time) < min(U time). No zero-hit
control reached the 1.10 invalidation threshold. Five fixed pairs describe this
run; they do not establish statistical population or deployment guarantees.

## Numeric checks and provenance

There were 12 D/repeat scenarios, each with 100 events from zero state and 100
from tiled [0.25,-0.5,0.75]. U/M input, router scores, selected expert, raw
product, output and before/after state matched bit-for-bit on all 2,400 steps.
Every raw row also matched an independent int64 Q10 sum: ternary weights and
bounded input numerators make this raw FP32 sum exact. Full D16 U traces were
checked by the independent Python FP32 recurrence at 1e-6 tolerance, with exact
integer products and independently regenerated model/corpus fingerprints.
Large-D validation streamed every comparison; it retained hashes and counts,
rather than full large-D traces. A failure would retain a bounded full first-step
witness and stop. No numeric failure occurred.

All timed pairs and warmups also agreed on event count, scalar checksum bits
and final full-state hash; M hit counts were stable across repeated batches.
Four independent RNL-003 checks passed. The preserved RNL-002 checker passed
seven controls, including the expert-omission failure and signed-zero cases;
these are checks of preserved evidence, not rerun RNL-002 workers. Six data-only
assessment controls rejected overlap, coarse timing, short timing, contaminated
zero-hit controls and slower cases while accepting a constructed valid case.

Every native worker used round-to-nearest with matching, unchanged SSE controls;
FTZ/DAZ were disabled. Floating-point exception status flags were recorded and
excluded from the numeric contract. Original ZIP/source/sample and previous
experiments remained unchanged. No uploaded program was run in this trial:
the reviewed worker generates its synthetic inputs and uses the same recurrence.

## Memory and storage boundaries

Separate U/M processes used D-sized models, L10 inputs and a 100-event warmup.
Their live snapshots preceded result serialization and large correctness traces.
These are whole-process measurements, not an allocation census.

| D | Partial live object/buffer bytes U / M | Added M buffers | Live working set bytes U / M | Private bytes U / M |
| --- | --- | --- | --- | --- |
| 16 | 10,352 / 10,480 | 128 | 3,653,632 / 3,653,632 | 491,520 / 491,520 |
| 64 | 42,224 / 42,736 | 512 | 3,694,592 / 3,694,592 | 532,480 / 536,576 |
| 256 | 307,952 / 310,000 | 2,048 | 3,973,120 / 3,973,120 | 815,104 / 823,296 |
| 1024 | 3,582,704 / 3,590,896 | 8,192 | 7,221,248 / 7,225,344 | 4,059,136 / 4,067,328 |

The partial subtotal counts model/corpus/VM objects, all expert gates/indices,
corpus vector storage, state/output/product scratch and M's retained key/product.
Payload aliases are not counted again. Both modes share the same VM class, so
empty cache-vector descriptors and fixed cache metadata are already charged to
both. The 8D delta is retained capacity, not an optimized minimal-U comparison.
Allocator, CRT, stack and other unattributed memory remain unknown. Cache input
and product buffers add 128/512/2,048/8,192 bytes across the four sizes.

Empty same-binary startup controls had working sets of
3,645,440, 3,645,440, 3,637,248 bytes and private bytes of
475,136, 475,136, 462,848. They give startup context; subtracting them would not yield
exact model memory. Raw worker/supervisor peaks and pagefile/commit readings
are retained. No process-memory saving was demonstrated or expected.

No generated scale-study model was serialized or loaded by native workers:
its full disk size and disk-load latency are null. The preserved RNL-002 checker
read its existing sample model. At D1024, resident matrix payload alone is 3,145,728 bytes,
plus gates and all other live objects. The executable is 338,432 bytes, separate
from model buffers and evidence. This study does not compare packed/FP32 storage.

## Resource use, reproducibility and decision

The single attempt completed in 19.516 seconds with 24 native workers and three
guarded data checks, retaining 786,185 bytes before derived publication artifacts.
Every launch exited zero with verified affinity, memory limits, deadlines,
bounded output and confirmed cleanup. The unchanged guard restricted each owned
process to one logical CPU affinity and 256MiB working-set/commit caps; the whole
attempt stayed below 600 seconds and 10MiB. Affinity does not prove one OS thread;
this resource guard is not a filesystem/network security sandbox. No other
processes, services, workloads or application checkouts were touched.

Build: x64 MSVC 19.50.35730.0, `/std:c++20 /EHsc /W4 /O2 /fp:strict`,
`/link /INCREMENTAL:NO Psapi.lib`; no warnings. Windows 10.0.26200.0 x64.
CPU model was unrecorded because the CIM query was unavailable. Clock speed,
thermal state and outside contention were not controlled or inferred. No
dependency installation, model download, network worker, training or GPU work.

To reproduce, compile [scale003.cpp](scale003.cpp) with those flags. Use the
unchanged [resource launcher](run_bounded.py) around each worker and freeze file
hashes first. CLI: `--kind validate --output-dir fresh`; timing adds
`--kind timing --dimension D --block L --output-dir fresh`; memory uses
`--kind memory --dimension D --block 10 --mode U|M --output-dir fresh`; empty uses
`--kind empty --output-dir fresh`. Fixed seeds, generation byte order, calibration
and limits are in the protocol/source. Run
`python -B check_scale003.py --results ROOT --validation-only` before timing,
then the same checker without that option after the fixed result grid exists.
Private guard argv/paths stay outside the public repository; public summaries
retain reviewed hashes, limits, outcomes and raw data without credentials.

The result supports the narrow cost hypothesis: exact raw-product reuse can
outweigh lookup/router/state overhead when the same expert/input key recurs in
these generated networks. The original CLI rejects D>256; D1024 is new synthetic
instrumentation. Real Revia key repetition, immutable-cache lifetime, invalidation
on model reload, learned-weight behavior, useful end-to-end savings, quality and
privacy remain untested. No Revia integration is justified yet. Stop here after
this reviewed study; a relevant consent-aware trace proposal is a later decision,
not an automatically started experiment or training job.
