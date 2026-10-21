# NLI advisory quality assay

Run with the environment prepared by `Tools/InstallMemoryNli.ps1` and a fresh output directory:

```powershell
ThirdParty/MemoryReranker/.venv/Scripts/python.exe Tools/Quality/nliDedupBenchmark.py --model-directory Models/MemoryNli/nli-deberta-v3-small --threads 2 --output .revia-ai/evidence/nli-quality-new
```

Add `--preflight-only` to validate the corpus and installed file hashes without loading a model. Nothing downloads, changes application settings, or enables duplicate marking. The worker loads the pinned safetensors model locally on CPU with offline access and no remote code. Allow enough free host RAM before running the model.

The default corpus is `Tests/Data/semanticDuplicatePairs.json`. The runner preserves its development/held-out partition, rejects duplicate IDs or unordered text pairs, and never tunes thresholds. Both directions must reach entailment >= 0.98 and contradiction <= 0.01; invalid or truncated worker evidence cannot pass. `pairs.jsonl` retains all three raw softmax probabilities in each direction from the same unchanged worker prediction, the worker evidence, case identity/text, and measured pair latency. An unlabelled warmup precedes scoring and is timed separately from model load and corpus pairs.

`summary.json` reports equivalent/distinct confusion separately by split. Uncertain cases have separate pass/reject/error counts and IDs; they are never true positives. Inference failures are listed separately and excluded from the evaluated confusion denominator. A run with per-case errors exits with code 2. Every distinct or uncertain threshold pass requires review.

This is **model-only evidence**. It does not call the C++ deterministic prechecks, embedding shortlist, scope/admission gates, or transactional mark store. Declared `precheckMustReject` values are retained as corpus metadata, not measured gate results. Model-only precision cannot be presented as pipeline precision or used by this script to activate the feature.

`run.json` and the final summary retain the corpus, harness, worker and installation-manifest hashes; all verified model/tokenizer file hashes; dependency versions; CPU thread count; host information; and hashes of the uninvoked C++ guard sources. Existing output directories are refused. Interrupted runs retain completed pair records and a failure summary. These synthetic cases do not establish general conversational equivalence accuracy.

Lightweight contract checks, without a model:

```powershell
python Tests/nliDedupBenchmark.test.py
python Tools/Workers/test_memory_nli.py
```
