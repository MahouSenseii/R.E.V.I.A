# Offline training data and jobs

Optional, and not part of normal startup. Memory changes what Revia retrieves, skills
change reusable procedures, and the self-development stages change verified code. This
path is the only one meant to change model parameters, and nothing in it runs by
itself. Status on 2026-10-31: the dataset exporter exists and passes fixture checks on
Linux. No runtime capture, owner review screen, real export or training job exists yet.

## Dataset export

`Learning/datasetExport` (`BuildDatasetExport`, `StaleDatasetExamples`,
`WriteDatasetExport`) works on `DatasetCandidate` records. Each record carries:

- the exact model input and the desired response;
- the quality receipt: judgment, criterion, judged output digest, and the profile and
  configuration fingerprints the runtime captures;
- the owner's review decision;
- the model, template and capture-transformation identities;
- every source ID the input used.

An example is eligible only when all of the following hold:

- Its review is **Accept**.
- Its receipt is valid and belongs to the same session.
- Its target is one of two things:
  - an owner correction whose rejected side is byte-for-byte the judged failure, and that
    does not repeat it; or
  - a model output whose exact digest a satisfied criterion recorded.
- None of its sources is withdrawn, including the judged turn itself.
- The shared secret gate finds nothing in it.
- It does not duplicate an earlier example.

Every exclusion is recorded in the manifest with its reason. A failed answer is never
written as an assistant message. It appears only as `rejected` beside its correction, for
any later preference training.

Grouping comes before splitting. Examples that share a session, a task, any source or an
identical prompt form one group, and a salted hash of the group places it in train,
validation or test. Given the previous manifest, an example keeps the partition it had.
If a new example would join groups that earlier exports kept apart, the group follows its
most protected member (test, then validation), and any member that would otherwise move
is excluded instead. So nothing a model trained on can later appear as a held-out test
case. The same candidates, withdrawals, options and previous manifest produce the same
bytes in any input order. `manifest.json` records each file's SHA-256 and every decision.

`WriteDatasetExport` writes a new directory through a `.partial` staging directory, and
never replaces an export or ignores an unfinished one. Immediately before a worker reads
an export, `StaleDatasetExamples` compares the manifest's per-example sources with the
current withdrawals. Any result means that export must not be used; prepare a new one. A
completed weight update cannot be undone by deleting an example. The candidate trained on
it is retired or retrained instead (see the promotion workflow, when it exists).

Evidence: `Tests/datasetExportTests.cpp` passed on Linux against the production owner.
Removing the review gate, the earlier-partition rule, source grouping or the canonical
ordering each made it fail. It is registered as `ReviaDatasetExportTests`.

Still needed before a real export:

1. The runtime must capture the exact serialized provider request from the correlated
   Stage E turn trace. A transcript reconstruction is not the model's input.
2. A source list must be complete for derived data, such as frame extracts and captions.
3. An owner review step must accept a correction.
4. The withdrawal set must come from the attachment library's and memory's own Forget
   receipts.
