# Offline training data and jobs

Optional, and not part of normal startup. Memory changes what Revia retrieves, skills
change reusable procedures, and the self-development stages change verified code. This
path is the only one meant to change model parameters, and nothing in it runs by
itself.

Status on 2026-10-31:

- The dataset exporter, the training worker, the pinned asset installer, the llama.cpp
  conversion and check tool, the evaluation runner, the recorded-failure capture and the
  live capture from the running companion, with its correction screen and in-app export,
  exist.
- Their checks pass on Linux. The exporter and worker checks also passed in the Windows CI
  build.
- A tiny Qwen3.5 has been trained on CPU and taken through llama.cpp end to end.
- The owner approved the downloads, including the 4B base for the separately evaluated 4B
  experiment.
- No real export or real training job exists yet. The live capture and its correction
  screen have not run against the companion on the host.

## Dataset export

`Learning/datasetExport` (`BuildDatasetExport`, `StaleDatasetExamples`,
`WriteDatasetExport`) works on `DatasetCandidate` records. Each record carries:

- the exact model input and the desired response;
- the tool definitions the request offered, when it offered any, because the chat template
  renders them into the prompt;
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
- Any tools it carries are a bounded list of named functions.
- The shared secret gate finds nothing in it.
- It does not duplicate an earlier example.

Every exclusion is recorded in the manifest with its reason. A failed answer is never
written as an assistant message. It appears only as `rejected` beside its correction, for
any later preference training.

Grouping comes before splitting. Examples that share a session, a task, any source or an
identical prompt form one group, and a salted hash of the group places it in train,
validation or test.

A candidate that names the conversation it came from (a `conversation:` source) is grouped by
that conversation instead of its session. Turns of one chat still stay together, but chats in
one app run share no conversation context. Grouping by the run would keep everything judged
in it in one partition. Given the previous manifest, an example keeps the partition it had.
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

1. A source list must be complete for derived data, such as frame extracts and captions.
2. Live captures keep their own withdrawal list for deleted chats and forgotten speakers.
   The attachment library and memory still have no Forget receipts that reach it, so live
   capture refuses their material, and the host writes `withdrawn.json` by hand for recorded
   captures.

## Capture from recorded failures

`Tools/revia_dataset_capture.py` turns recorded failures into reviewed candidates without
reconstructing anything.

- `capture --report <report> --select case:turn` reads a quality report together with the
  traffic its observation proxy saved beside it: `.requests.json`, `.responses.json`,
  `.metadata.json` and `.provider.json`. It binds each selected turn to the single completion
  request whose raw output, streamed or not, is that turn's model reply. When more than one
  request matches, choose with `case:turn@requestIndex`; the tool never guesses.
- `capture --request <file> --response <file>` takes one exact serialized request and raw
  response from any other harness. It also needs `--profile-digest` and `--template-sha256`,
  because an exact capture needs its model, profile and template.

A capture keeps the messages the model received, byte for byte, and the tools the request
offered, together with the failed output and its digest, the request's sampling settings
and their digest, and the source file digests. It refuses, with a reason, anything a text
dataset cannot hold exactly:

- image or other non-text parts;
- tool-call history, and replies that are tool calls;
- a reply generated without thinking explicitly off, because the training path renders
  with thinking off.

Each capture also writes `corrections.template.json`.

For each capture, the owner fills in:

- a decision: accept, reject, rework or need-evidence;
- the issue;
- the criterion the reply failed;
- the evidence for that judgment;
- and, for an accepted capture, the correct reply.

`candidates` turns the decided captures into the interchange file
(`revia-dataset-candidates-v1`). Pending captures are left out and reported. The owner's
decision becomes an owner-judgment quality receipt bound to the failed output's digest.

`ReviaDatasetExport` passes that file to the existing exporter, which applies eligibility,
grouping, partitions and withdrawal as before. Each capture's source is its own request, so
independent cases split independently, and turns of one case share a group.

```powershell
python -I -B Tools\revia_dataset_capture.py capture --report <report.jsonl> --select <case>:<turn> --output D:\ReviaTraining\captures\stage-e-1
# Fill corrections.template.json and save it as corrections.json.
python -I -B Tools\revia_dataset_capture.py candidates --captures D:\ReviaTraining\captures\stage-e-1 --corrections corrections.json --output candidates.json
<build>\ReviaDatasetExport.exe --candidates candidates.json --name stage-e --salt stage-e-1 --withdrawn withdrawn.json --output D:\ReviaTraining\datasets\stage-e-1
```

Of the Stage E failures:

- the malformed-code replies and the stale-topic resume are text turns, so they can be
  captured;
- the image-feeling reply cannot, because its prompt carries the image and the training
  path is text-only.

Which files hold each failure depends on the harness that recorded it. Point the tool at the
saved traffic, or at the exact request and response pair.

Evidence, all on Linux:

- `Tests/datasetCapture.test.py` passed its 5 tests against fixtures in the proxy's exact
  format.
- In the end-to-end test, the real C++ `ReviaDatasetExport` exported 8 accepted captures
  (one rejection excluded with its reason). The training worker's own `verify_dataset` read
  the result, then refused it once a source was withdrawn.
- Each of these removals made a test fail: the single-request binding, the text-only rule,
  and per-request sources.
- `Tests/datasetExportTests.cpp` now also checks that `ReadDatasetCandidates` round-trips a
  candidate and refuses unknown values, missing fields and non-text content.

## Live capture from the running companion

When the owner judges a displayed reply a failure in the Review dialog, they can tick **Also
keep the exact exchange for a training dataset**. The box is off by default, and nothing is
written unless it is ticked.

1. `llamaCppService` attaches the exact request body it sent, and the raw streamed text, to
   every text reply (`responseOutput::exchange`, `LLM/providerExchange.h`). It adds the
   served model and the digest of the chat template the server reported at its last health
   check. A request that carried pictures is not attached.
2. The session holds the last 8 private replies in memory. Each keeps its exchange, the
   attributed speaker, the archived chat, and the profile and configuration fingerprints at
   delivery. Nothing is written yet.
3. Once the judgment itself is recorded, `ReviaSession::CaptureJudgedTurn` looks up the
   held reply whose displayed text has the judgment's target digest. It applies the same
   admissions as the judgment: an owner failure judgment from the current private session
   and audience revision.
4. `Learning/liveCapture` (`BuildLiveCapture`) builds the capture.
5. `LiveCaptureStore` writes it as `RuntimeData/Learning/Captures/live/<captureId>.json`.
   It never replaces a capture.

A live capture keeps:

- the messages and tools the model received, exactly as the request held them;
- the judged reply as `failedOutput`, the text the owner judgment is bound to;
- the model's own streamed text as `modelOutput`, with `repaired` set when style repair
  changed it;
- the owner's recorded judgment, unchanged;
- the request's sampling settings, the finish reason, the model, the template digest and
  the fingerprints;
- its sources: `request:<digest>` and `conversation:<archived chat>`;
- its speaker, recorded beside the sources as `source.participantId`. As a source, it would
  join every capture of one person into one dataset group, so no export could hold any out.

It refuses, and the dialog shows why:

- a reply that is no longer held, or that was shown in parts (a spoken reply's fragments);
- a safety replacement or an AI reviewer's rewrite, which the request did not produce;
- a cloud reply, and an unknown template;
- a profile or model configuration that changed since the reply;
- pictures, tool-call history, a tool-call reply, or thinking that is not explicitly off;
- private reference material whose sources the runtime does not name yet: conversation
  recalled from other chats, attached files, kept documents, the clipboard, live page text
  and visible-browser results;
- anything the secret gate finds.

Withdrawal follows the companion's own deletes:

- deleting a chat withdraws its captures;
- forgetting every chat withdraws them all;
- forgetting a speaker withdraws the captures whose recorded speaker they are.

In each case the capture file and its correction are deleted. The source is added to
`RuntimeData/Learning/Captures/withdrawn.json`, together with each deleted capture's judged
reply (`displayed-reply:<id>`), which no other capture shares. The request is not listed:
a reply regenerated from the same request and judged separately would be withdrawn with it.

Pass that file to `ReviaDatasetExport --withdrawn`, and as a training job's
`withdrawnSourcesPath`, so that a copy exported before the deletion is refused too. It
exists from the first withdrawal; until then, use a file containing `[]`.

Memories quoted in the prompt are not named as sources. Memory has no delete today: owner
revisions keep the history. Before a memory forget exists, it must name its records in both
places they are rendered, the retrieved-memory block and the state packet.

### Writing corrections

The owner corrects kept captures in **Skills & Learning**, under **Training corrections**.
It is listed only while the audience is private, like past chats.

For each capture, newest first, the screen shows:

- what the person said, without the runtime context the prompt placed ahead of it;
- the judged reply, and the model's own text when style repair changed it;
- the judgment recorded when the capture was kept.

The owner chooses a decision (accept, rework, need evidence or reject) and writes the reply
the companion should have given. **Save correction** saves it as the capture's next
revision in `RuntimeData/Learning/Captures/corrections/<captureId>.json`
(`revia-live-correction-v1`). That file holds:

- the decision and correction;
- the judged reply's digest, which binds the correction to its capture;
- up to 32 earlier revisions.

An accepted capture needs a correct reply that differs from the judged one, and the secret
gate applies to it.

The panel refreshes every second. A refresh never replaces a draft: the saved decision and
correction are loaded only when another capture is selected. Switching companion clears the
capture and the draft.

**Delete capture…** asks first. It then deletes the capture and its correction, and lists
its judged reply as withdrawn.

Exporting stays a separate, deliberate step: **Export dataset…** (below), or the commands
here. Without `--corrections`, `candidates` reads the saved corrections beside live
captures. It refuses one bound to another reply, and treats a capture without one as
pending.

```powershell
python -I -B Tools\revia_dataset_capture.py candidates --captures <companion>\RuntimeData\Learning\Captures\live --output candidates.json --prefix live
<build>\ReviaDatasetExport.exe --candidates candidates.json --name live --salt live-1 --withdrawn <companion>\RuntimeData\Learning\Captures\withdrawn.json --output D:\ReviaTraining\datasets\live-1
```

`template` still writes a corrections file to edit by hand, passed with `--corrections`. It
shows the recorded issue, criterion and evidence as context, and asks only for a decision
and a correction. In both ways, `candidates` uses the recorded judgment verbatim, and refuses
a corrections entry that tries to change it. Recorded captures still need a corrections
file.

### Exporting from the companion

**Export dataset…** in Training corrections is enabled once a capture is accepted. It asks
for a parent folder and writes the next numbered dataset there, such as `revia-live-3`. It
never replaces a folder.

Because an export holds private prompts, the parent must be a plain local folder. These are
refused, as the model installer refuses them:

- OneDrive, which would upload it;
- network folders;
- links and reparse points anywhere on the path.

Each companion keeps one lineage in `RuntimeData/Learning/Datasets`:

- `lineage.json` holds the dataset name, a fixed salt, and each export's number, folder,
  manifest digest and partition counts;
- `manifests/` holds a copy of every export's manifest.

Every export keeps the partition that any earlier export gave an example. That holds even
when the example was deleted and judged again, or when a new turn of its chat joins it and
changes its group. So nothing once held out can move into training.

- The lineage is recorded before the export is written, so a failed write still reserves
  those partitions.
- A missing or changed manifest copy, or an unreadable lineage, refuses the export instead
  of starting over.

Validation and test are fixed at 10% each. The summary warns when the test partition has
fewer than the evaluation plan's 5 cases.

The export applies the companion's withdrawal list, and creates `withdrawn.json` if nothing
has been withdrawn yet. The summary names that file, for a training job's
`withdrawnSourcesPath`.

The candidates are the same ones `revia_dataset_capture.py candidates --prefix live` makes
from the saved corrections. `Tests/Fixture/liveCandidates.json`, written by that tool from the
shared capture and correction fixtures, pins both.

Nothing is written in either of these cases:

- no accepted correction is eligible;
- every eligible example falls in a held-out partition, so there would be nothing to train
  on.

The captures, corrections and lineage live in the companion's `RuntimeData`. If that folder is
inside OneDrive, for example a checkout under `OneDrive\Documents`, they sync like the
companion's memory and chats already do. Exports are kept out of OneDrive whatever the
setting.

Evidence:

- On Linux, `Tests/liveCaptureTests.cpp` (`ReviaLiveCaptureTests`, 5 tests) passed
  against the production builder and store. It covers:
  - exactness and binding to the judged reply;
  - each refusal;
  - immutable keeping;
  - corrections and their revisions;
  - withdrawal and its list.
- A read of the record through the strict interchange reader and the exporter succeeded,
  and the export was refused once the chat was withdrawn.
- Removing each of 16 protections made it fail: 8 in the builder, 2 in keeping and
  withdrawal, and 6 in corrections. The correction protections are:
  - accepting needs a different reply;
  - the secret gate;
  - the binding to the judged reply;
  - deleting only the chosen capture;
  - deleting its correction with it;
  - keeping earlier revisions.
- Fixtures shared between C++ and Python, so the two cannot drift apart:
  - `Tests/Fixture/liveCapture.json` is the builder's record.
  - `Tests/Fixture/liveCorrection.json` is a correction the store saved for it.
  - The C++ test checks the store still produces both, and the Python tests read them.
- `Tests/datasetCapture.test.py` (9 tests) passed on Linux. It covers:
  - live templates and candidates using the recorded judgment verbatim;
  - saved corrections read without a corrections file;
  - the record through the real C++ `ReviaDatasetExport` with its tools;
  - the worker's read, and its refusal once the chat was withdrawn.
- Removing each of 7 capture-tool protections made a Python test fail.
- The Learning panel compiled against Qt 6.4.2 on Linux with no warnings. A headless
  harness, not committed, drove the real panel. These held:
  - drafts survive refreshes;
  - Save sends the shown capture with Accept by default;
  - a busy panel is locked;
  - choosing a capture loads its saved state;
  - a deleted capture's draft does not move to another capture;
  - a hidden list and a companion switch show nothing private.
- The changed window sources and the desktop test also compiled against those headers.
- `Tests/liveDatasetExportTests.cpp` (`ReviaLiveDatasetExportTests`, 4 tests) passed on
  Linux, and as a Windows build (MinGW-w64 GCC 13.2 and LLD 18.1.3) under Wine 9. It covers:
  - parity with the capture tool's candidates;
  - numbered exports;
  - partitions kept from any earlier export, through deletion, return and regrouping;
  - the withdrawal list;
  - each refusal, writing nothing: the folder rules (OneDrive, network, link, missing or
    relative), a missing or changed manifest copy, an unreadable lineage, and an existing
    folder.
- Removing each of 8 export protections made it fail:
  - keeping earlier partitions at all;
  - keeping them from every earlier export, not just the last;
  - the OneDrive, link and network refusals;
  - refusing an existing folder;
  - applying the withdrawal list;
  - checking the manifest copies.
- So did grouping live captures by session again, or listing the speaker among sources.
- These tests found two faults before any commit:
  - a loop over a member of a destroyed temporary, which would have silently dropped every
    earlier partition;
  - under Wine, a network path refused as "not a full path", because libstdc++ on Windows
    does not count UNC paths as absolute.
- Windows CI run 41205772282 (commit `374fc20`) passed `ReviaLiveDatasetExportTests`. It also
  passed the session tests, which export through the real Windows folder checks, and every
  other branch check.
- The test was also built for Windows with MinGW-w64 GCC 13.2 and LLD 18.1.3, using CI's
  flags. That build passed all 5 tests under Wine 9, including Windows file replacement for
  correction revisions.
- Windows CI run 41199765499 (commit `3181af9`) stopped at a link of
  `ReviaLiveCaptureTests.exe` that printed nothing, so no test ran there. On the retry (run
  41203033290, commit `ea226c6`) the same code built. These passed with the correction checks:
  `ReviaLiveCaptureTests`, `ReviaQualityFeedbackRuntimeTests`, `Revia.DatasetCapturePolicy`
  and `Revia.DesktopStopPolicy`.
- `Tests/qualityFeedbackRuntimeTests.cpp` adds the session path. It covers:
  - holding, admissions and fingerprints;
  - refusing a second keep;
  - withdrawal when every chat is forgotten;
  - listing, correcting and deleting, all refused for a public audience.

  The desktop test checks that the box exists and starts unticked. It also checks that
  correction drafts survive refreshes, and that a companion switch clears them. Both tests
  build only on Windows, so they run in Windows CI.
- Windows CI (run 41178797203, commit `64da0c5`):
  - The desktop and session changes built.
  - These passed: `ReviaLiveCaptureTests`, `Revia.DatasetCapturePolicy` (all 8, including
    both live tests and the end-to-end export) and `Revia.DesktopStopPolicy`.
  - `ReviaQualityFeedbackRuntimeTests` failed on its last assertion, through a test bug. It
    reused a judgment from before an audience change, which the session correctly refused
    as stale.
  - The fix (commit `e8d8f1c`) holds a fresh reply under the current audience before
    forgetting, and checks the reason for each refusal. In run 41181811601 the suite passed,
    along with every other branch check.
- Not yet run: the Qt dialog and screen on the host, a real server's `/props` template, and
  a real judged turn.

## Training worker

`Tools/revia_training_worker.py` runs one supervised BF16 LoRA job on a dataset export.
It uses the same contract as the video worker:

- The job is `ReviaTrainingJobs/<jobId>/job.json` in an owned scratch root, separate from
  the candidate output root.
- Progress is one JSON line per state.
- Creating `cancel` in the job directory stops the job at its next step, after saving a
  checkpoint.
- Each attempt leaves its own `result.json` (earlier attempts are kept as
  `result-N.json`).
- Exit codes: 0 complete, 1 failed, 3 cancelled.

The worker imports torch, transformers and peft only when a job asks for the
`transformers-peft` backend. A missing runtime is refused by name, so normal startup never
depends on them.

Before any model work, the worker checks the base checkpoint against its installed
`revia-install-manifest.json`. That is a JSON object with `repository`, `revision` and
`files` (each file's `bytes` and `sha256`), written by the approved download step. The
check covers its digest, repository and revision, every file's size, and a fresh hash of
each file of 64 MiB or less, such as the config, tokenizer and chat template. It checks
the dataset manifest and the train and validation files against their digests. It reruns
the withdrawal check against the current list the host writes beside the job. The test
partition is never opened.

`prepare` writes a job from a dataset export and an installed base. It uses the 4B
configuration below, and runs every check a run makes before loading a model, so a prepared
job does not fail on them later. Scratch and candidate folders inside OneDrive are refused,
both when preparing and when running: checkpoints and adapters come from private data.

Configurations other than BF16 supervised LoRA are refused. Quantized LoRA is refused
with the reason the current Qwen3.5 guidance gives: 4-bit training shows larger than
normal quantization differences. A checkpoint from another base, dataset or configuration
is refused rather than resumed. An existing candidate is never replaced.

Offered tools are handed to the chat template the way the pinned llama-server hands them.
It rebuilds each tool as `type`, then `function` with `name`, `description` and
`parameters`. A missing description becomes empty, and the parameters keep their own order.
This was checked against b10453's `/apply-template` on the tiny Qwen3.5:

- transformers rendered the identical prompt only in that order;
- in the exporter's sorted order the prompt differed;
- the masking prefix still held with tools.

Examples are rendered with `enable_thinking=False`, the setting Revia uses for ordinary
turns, and only the assistant reply, including its end-of-turn token, is supervised. An
actual Qwen3.5 template check found that with thinking on, the generation prompt
(`<think>\n`) is not a token prefix of a rendered reply (`<think>\n\n</think>\n\n…`), so
naive response masking would be wrong. The worker therefore refuses any example whose
prompt is not an exact token prefix. Reasoning-mode examples are not supported yet. The
same guidance recommends keeping at least 75% reasoning examples to preserve reasoning, so
an adapter trained only on direct replies must be evaluated for reasoning retention before
any promotion.

A finished job publishes `adapter-<jobId>/` through a staging directory. Its
`candidate.json` records:

- the base identity, including the template digest and model shape;
- the dataset identity, including the withdrawal list digest;
- the exact configuration and its digest;
- the rendering and masking rule;
- train and validation loss, and the environment versions;
- every file's digest.

Its state is `candidate`. Nothing activates, converts or merges it.

Evidence, all on Linux:

- `Tests/reviaTrainingWorker.test.py` covers orchestration with a deterministic fixture
  backend, in 16 tests. Each of these changes made its own test fail:
  - removing the withdrawal recheck;
  - reading the test partition;
  - dropping the checkpoint identity check;
  - rendering tools in the exported order instead of the server's.
- `prepare` is covered too: a prepared job runs as written, and each refusal writes nothing.
  These changes each made a test fail:
  - skipping its dataset or base checks;
  - the OneDrive refusal;
  - the three-epoch step count.
- A smoke-prepared job, with the default configuration (all seven attention and MLP
  targets), also trained the tiny Qwen3.5 with the real backend and published its adapter.
- With `REVIA_TRAINING_SMOKE_MODEL` set, the real backend trained
  `trl-internal-testing/tiny-Qwen3_5ForConditionalGeneration-Think` (`6860d7ec`, a hybrid
  linear-attention plus full-attention Qwen3.5) on CPU. The software was torch 2.15.0+cpu,
  transformers 5.22.1 and peft 0.21.2. The run was cancelled at its step-2 checkpoint,
  resumed and finished, and the published adapter changed the base model's output. One of
  its examples offered a tool, rendered through the real template.
- Without optimized kernels, transformers fell back to reference implementations of the
  gated delta rule and causal convolution. A real job's environment should include
  `flash-linear-attention` and `causal-conv1d`.

This is fixture and tiny-model evidence: no GPU, real base or real dataset has been used.

Run the checks, and the worker once prerequisites exist:

```powershell
python Tests\reviaTrainingWorker.test.py
```

```powershell
<training-python> -I -B Tools\revia_training_worker.py --job <scratch>\ReviaTrainingJobs\<jobId>\job.json
```

## Conversion and llama.cpp check

`Tools/revia_adapter_export.py` takes a candidate from a verified base to llama.cpp files.
It reuses the worker's base verification, and each command writes a new directory with an
`export.json` receipt.

- `base` converts the verified Hugging Face base to BF16 GGUF with the pinned llama.cpp
  converter and quantizes it (Q4_K_M by default). This is the matched baseline. The official
  Qwen3.5 checkpoints carry their MTP head, which the conversion keeps (`mtpHead`), so the
  baseline can also be used for MTP speculation. A config that declares a head its weights
  lack gets `--no-mtp`, because converting it would write a block the loader cannot find.
- `adapter` converts a candidate to a GGUF LoRA with `convert_lora_to_gguf.py`. It first
  refuses a candidate whose recorded base is not exactly this base, so a 4B adapter can never
  be converted for the 9B. The quantized base never changes, and serving without `--lora`
  rolls the adapter back.
- `check` runs llama-server on the baseline, and again with the adapter. Each run is
  compared with the Hugging Face base on:
  - the embedded chat template;
  - the end-of-turn token;
  - chat rendering with thinking off, with and without tool definitions;
  - tokenization of the rendered prompt;
  - the GGUF block count, including any MTP block.

  Greedy replies from the two runs are recorded for comparison but are not a gate.

The converter runs under its own interpreter (`--converter-python`), an environment built
from the tag's `requirements/requirements-convert_hf_to_gguf.txt`. That file pins
transformers 4.57.6, while training needs transformers 5. The pinned converter cannot load a
tokenizer saved by transformers 5 (`TokenizersBackend`). The official Qwen3.5 checkpoints name
`Qwen2Tokenizer`, which it loads.

Evidence, all on Linux:

- `Tests/reviaAdapterExport.test.py` covers the refusals in 6 fixture tests.
- A full roundtrip on the tiny Qwen3.5 passed every gate on both runs. It trained an adapter
  on attention plus MLP, converted the base with llama.cpp `b10453` (commit `3cb7ffb1a1f6`)
  using the converter environment (torch 2.11.0, transformers 4.57.6, numpy 1.26.4), and
  converted the adapter. Where both runs produced text, the adapter's replies differed from
  the base's.
- The tiny model's 16-wide tensors cannot be block-quantized, so its smoke run requantizes to
  F16. The real bases are 2,560 and 4,096 wide and use Q4_K_M.
- llama-server `b10453` parses every generation against the template's output format, and
  rejected some of the random model's text. That is why replies are informational.

## 4B experiment (chosen 2026-10-31)

The 4B adapter is a separately evaluated experiment on the official `Qwen/Qwen3.5-4B`. It is
never attached to the 9B Main. Using any 4B model in Revia would be a separate
model-selection decision.

Adapters stay evaluation-only (owner's decision, 2026-10-31). Nothing in Revia serves one:
Main's launch has no `--lora`, and the evaluation runner serves its own llama-server. How an
accepted adapter would be put into use is designed only after a candidate's `decision.json`
accepts. That design is also the model-selection decision above.

Steps on the host:

1. Install the base outside OneDrive:
   `python -I -B Tools\install_training_assets.py --asset qwen3.5-4b-base --destination D:\ReviaModels`.
   It lands in `D:\ReviaModels\Qwen3.5-4B-851bf6e`, with install manifest SHA-256
   `f5a92a1bf46485d70bd38e9643ee6dd29bb674029733fe0fddf097db45396a5e`.
2. Build two Python 3.12 environments.
   - Training: a CUDA build of torch for the RTX 5070, transformers 5, peft, safetensors,
     flash-linear-attention and causal-conv1d.
   - Converter: from the llama.cpp `b10453` checkout's converter requirements.

   Then run the three test files with the smoke variables set:

   ```powershell
   $env:REVIA_TRAINING_SMOKE_MODEL = "<tiny model dir>"
   $env:REVIA_LLAMA_CPP_DIR = "<b10453 checkout>"
   $env:REVIA_LLAMA_BIN_DIR = "ThirdParty\llama.cpp"
   $env:REVIA_CONVERTER_PYTHON = "<converter venv>\Scripts\python.exe"
   ```

   The tiny model is `trl-internal-testing/tiny-Qwen3_5ForConditionalGeneration-Think` at
   `6860d7ec`. This proves the host environments before the 9 GB job.
3. Correct kept captures in Training corrections and use **Export dataset…**. Then prepare a
   5-step job, which measures peak VRAM:

   ```powershell
   python -I -B Tools\revia_training_worker.py prepare --smoke --job-id qwen35-4b-companion-smoke --dataset D:\ReviaTraining\datasets\revia-live-1 --base D:\ReviaModels\Qwen3.5-4B-851bf6e --scratch-root D:\ReviaTraining\ReviaTrainingJobs --output-root D:\ReviaTraining\candidates --withdrawn <companion>\RuntimeData\Learning\Captures\withdrawn.json
   ```

   `prepare` reads every digest from the files themselves. It checks the base, the dataset and
   the withdrawal list as a run would, and writes `job.json` under the scratch root. It never
   replaces a job. The job names the companion's own withdrawal list, so a withdrawal made
   after preparing is still honoured when the run starts.
4. With Revia stopped, run it:
   `python -I -B Tools\revia_training_worker.py --job D:\ReviaTraining\ReviaTrainingJobs\qwen35-4b-companion-smoke\job.json`.

   The 10,500 MiB free-VRAM floor enforces the stop, since Main and the voice worker would
   not leave that much free. `candidate.json` then records `peakVramMiB`. The guidance's
   10 GB figure assumes Unsloth's optimizations, so plain peft on a 12 GB card has to be
   measured.

   Then prepare the full run: the same command without `--smoke` and with a new `--job-id`.
   If the smoke run's peak VRAM requires it, add a lower `--max-sequence-tokens`.
5. Export the matched baseline and the adapter, then run `check` with
   `--server-arg=-ngl --server-arg=99`.
6. Evaluate with the runner described below. Plan the campaign first, then run the base arm and
   the candidate arm, write the review sheet, judge it, and score it. The criteria are fixed
   when the plan is written:
   - the held-out test partition improves, with no new failures;
   - no more than one regression across the golden set, the answer-quality holdout and the
     tool cases;
   - prefill and decode rates within 5% of base;
   - the `check` passed.

   The base arm is the same file served without `--lora`, so it is also the rollback.
   A rejected candidate is a valid result.

For reference, this is the job `prepare` writes for a full run:

```json
{
  "schema": "revia-training-job-v1",
  "jobId": "qwen35-4b-companion-1",
  "scratchRoot": "D:/ReviaTraining/ReviaTrainingJobs",
  "outputRoot": "D:/ReviaTraining/candidates",
  "withdrawnSourcesPath": "<companion>/RuntimeData/Learning/Captures/withdrawn.json",
  "base": {"directory": "D:/ReviaModels/Qwen3.5-4B-851bf6e", "repository": "Qwen/Qwen3.5-4B",
           "revision": "851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a",
           "installManifestSha256": "f5a92a1bf46485d70bd38e9643ee6dd29bb674029733fe0fddf097db45396a5e"},
  "dataset": {"directory": "<reviewed export directory>", "manifestSha256": "<SHA-256 of its manifest.json>"},
  "resources": {"device": "cuda:0", "minimumFreeVramMiB": 10500, "minimumFreeRamMiB": 16384, "minimumFreeDiskMiB": 20480},
  "configuration": {"method": "sft-lora", "backend": "transformers-peft", "precision": "bf16", "enableThinking": false,
                    "seed": 20261031, "maxSteps": 5, "learningRate": 0.0001, "gradientAccumulation": 8,
                    "maxSequenceTokens": 4096, "loraRank": 16, "loraAlpha": 32, "loraDropout": 0.05,
                    "targetModules": ["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
                    "checkpointEverySteps": 5}
}
```

Qwen3.5 projections differ by layer type. Gated DeltaNet layers use
`linear_attn.in_proj_qkv/z/b/a` and `out_proj`, the full-attention layers (every fourth) use
`self_attn.q/k/v/o_proj`, and every layer has `mlp.gate/up/down_proj`. The listed targets
therefore cover attention where it exists and MLP everywhere. The Gated DeltaNet projections
stay frozen until measured.

For a full run, `prepare` sets `maxSteps` to three epochs, `ceil(3 × train examples / 8)`, and
checkpoints every 5 steps. It keeps the configuration otherwise fixed.

## Evaluation runner

`Tools/revia_adapter_evaluation.py` compares a candidate with its matched base. It judges
nothing itself: mechanical checks stay with the existing evaluation owners, and the
held-out replies go to the owner.

- `plan` writes `plan.json` into a new campaign directory before anything is generated. It
  records digests of:
  - the base and adapter exports and their passed `check`;
  - the dataset's held-out partition;
  - the golden set, the answer-quality holdout and `Config/Evaluation/adapter-tool-cases.json`;
  - the llama-server, `ReviaAnswerGateLive` and `ReviaAnswerQualityLive` binaries and the
    profile;
  - the sampling settings: temperature 0, a seed, thinking off.

  It also records the criteria. The plan is refused if:
  - the candidate was trained on a different dataset export;
  - the check did not pass for exactly this base and adapter;
  - the held-out partition is smaller than the declared minimum (5 by default).

  Every later command rechecks all of those digests, so nothing can change between fixing the
  criteria and applying them.
- `run --arm base|candidate` serves one arm from an owned llama-server with the arm's alias: the
  quantized base, then the same file with `--lora`. It then:
  - replays each held-out prompt exactly as captured, without its reviewed reply;
  - calls the tool cases and judges each call mechanically (right function and arguments, and
    no call when none is needed);
  - runs `ReviaAnswerGateLive` on the golden set with `--expected-model` set to the arm's alias
    and `--ignore-baseline`, without a judge unless the plan names a judge port;
  - runs `ReviaAnswerQualityLive` on the holdout.

  Each arm runs once into its own directory and is never rerun in place. The held-out replays
  and tool cases use temperature 0 and the plan's seed. The golden set and holdout run through
  the production conversation runtime with the profile's own sampling, so they measure
  retention as Revia would answer; the gate seeds it with the plan's seed.
- `review` writes `review.json`. Each held-out case shows its criterion, conversation and
  reviewed reference, with the two replies as A and B in a seeded random order. The mapping
  goes to `review-key.json`, which the reviewer should not open. A blank
  `verdicts.template.json` is written alongside.
- `score --verdicts <file>` writes `decision.json`: accept, reject, or incomplete while evidence
  is missing. Any failed criterion rejects. Retention regressions are cases the base passed and
  the candidate fails. A golden-set or holdout report counts only if it names the arm's own
  alias; otherwise its cases are unavailable, and the decision says why. Latency compares the medians of llama-server's own `prompt_ms` and
  `predicted_per_second`, over only the requests both arms completed. Golden-set judge scores,
  when present, are reported as advisory. Nothing is activated.

Evidence, all on Linux:

- `Tests/reviaAdapterEvaluation.test.py` passed its 8 fixture tests. They use a loopback model,
  and stand-ins that reproduce the quality tools' arguments, result files, model identity and
  exit codes.
- Each of these removals made a test fail: the no-new-failures rule, matched latency, the
  plan's digest recheck, and the report identity check.
- A smoke run planned both arms against a real llama-server `b10453` on the tiny Qwen3.5, then
  ran, reviewed and scored them. Without verdicts it could not accept. It rejected on latency,
  which is correct, because LoRA overhead on a tiny CPU model is large.
- The smoke run also found a bug in `check`: run results overwrote the base identity and the
  adapter flag. Run results now sit under `runs`, and the roundtrip test asserts the identity.
- A second smoke run drove the real tools: Windows builds of `ReviaAnswerGateLive` and
  `ReviaAnswerQualityLive` under Wine 9, against the same llama-server and tiny model. It ran 14
  golden cases (two per category), the six-case holdout and the tool cases. It found that the
  quality tool's default mode answered as a fixed model name, `Qwen3.5-4B-Q4_K_M.gguf`.
  The runtime refused the owned server, every holdout case came back unjudged, and no
  evaluation could have accepted. The tool now answers as the model the server reports. With
  the fix, each report named its own arm. Every golden case and 11 of the 12 holdout results
  were judged. llama-server rejected the random model's output on the twelfth, which counted
  as unavailable. The decision was reject, on held-out and latency.

On the host, with Revia stopped so both arms get the 5070:

```powershell
python -I -B Tools\revia_adapter_evaluation.py plan --output D:\ReviaTraining\evaluations\qwen35-4b-companion-1 `
  --base-export D:\ReviaTraining\exports\qwen35-4b-base --adapter-export D:\ReviaTraining\exports\qwen35-4b-companion-1 `
  --check D:\ReviaTraining\exports\qwen35-4b-companion-1-check.json --dataset <reviewed export> `
  --server ThirdParty\llama.cpp\llama-server.exe --gate <build>\ReviaAnswerGateLive.exe `
  --quality <build>\ReviaAnswerQualityLive.exe --profile Config\Profiles\revia.json --server-arg=-ngl --server-arg=99
python -I -B Tools\revia_adapter_evaluation.py run --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1 --arm base
python -I -B Tools\revia_adapter_evaluation.py run --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1 --arm candidate
python -I -B Tools\revia_adapter_evaluation.py review --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1
python -I -B Tools\revia_adapter_evaluation.py score --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1 --verdicts verdicts.json
```

The `score` exit code is 0 for accept, 1 for reject and 2 for incomplete.

## Remaining blockers

- **Reviewed data.** The capture exists, but no reviewed export does yet. On the host:
  1. Capture the Stage E text failures from `build/stage-e-20261030`.
  2. Write their corrections.
  3. Export them, aiming for enough distinct cases that the held-out partition reaches the
     evaluation plan's minimum of five.

  Replies the owner judges in ordinary use can also be kept, corrected in the companion and
  exported from it with **Export dataset…**, as numbered datasets of the companion's lineage.
- **Evaluation runner on the host.** The runner has driven the real quality tools only as
  Windows builds under Wine, against a tiny CPU model. Its first host run is the first with
  native Windows, the GPU and the real 4B pair.
- **A 9B adapter.** Training it locally is still infeasible as configured: about 22 GB for
  BF16 LoRA against the 12 GB 5070.
