# Applicability audit 001: exact reuse and current Revia

Status: independently reviewed source audit; no application integration target
demonstrated. No experiment is authorized by this document. RNL-003 remains a
positive measured result for its fixed synthetic workload: eight repeat
conditions benefited, all four zero-repeat controls did not, and all 2,400
validation steps matched. See [the retained report](RESULTS-RNL-003.md).

The question is now whether that contract fits useful recurring work in Revia.
Positive and negative findings can redirect research toward another workable
approach. This audit keeps two small evidence-supported leads, not a speculative
optimization backlog. Candidate for Revia and Integrated remain **None**.

## Source boundary and installed identity

Revia source inspected: main
`1b4118f2288ee53c399f382ec5b9775a67f6568f`.
Upstream source inspected: llama.cpp tag `b10453`, commit
`3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70`.

The [installer manifest selects b10453](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Tools/ReviaAcceleration.ps1#L205-L259).
However, [installation can retain an existing runtime](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Tools/InstallLlamaCpp.ps1#L43-L61)
based on accelerator/runtime matching. Model health checking accepts a name or
basename, rather than a loaded-weight digest. Repository pins do not verify the
installed executable, loaded model, adapters, tokenizer or execution settings.
Those identities remain unknown in this source-only audit; no live server,
private conversation, runtime logs or model weights were inspected.

## Observations

1. **There is no host tensor-cache hook.** Revia constructs messages/options and
   sends `cache_prompt` through HTTP `/v1/chat/completions`; it does not expose
   internal matrix inputs or neural expert indices.
   [Request construction](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/LLamaCPP/llamaCppService.cpp#L1761-L1829).
2. **Same words do not establish the same later activation.** Prompt history,
   posture and retrieval context can change. The audited Qwen3.5 dense graph uses
   hybrid attention/recurrent state, positions and successive residual layers;
   Revia's Main/Fast/Expert service roles are not the toy's neural expert indices.
   [Prompt construction](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/promptBuilder.cpp#L148-L256),
   [Qwen3.5 graph](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/src/models/qwen35.cpp#L149-L206),
   [dense FFN](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/src/models/qwen35.cpp#L473-L482).
3. **Relevant reuse already exists.** llama.cpp reuses common token prefixes and
   restores applicable recurrent checkpoints, otherwise reprocessing the prompt.
   Revia already requests that reuse. A request flag is not evidence of actual
   hits or saved time. `cache-ram=0` is not a blanket statement that active
   KV/prompt reuse is disabled.
   [Prefix matching](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-context.cpp#L3123-L3137),
   [checkpoint handling](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-context.cpp#L3255-L3289).
4. **Full-input embedding reuse is already implemented.** Successful normalized
   vectors are retained in a 64-entry LRU keyed by endpoint, model name and exact
   `prefix + text`. Applying settings clears it. Normal startup applies settings
   before ensuring the embedding server is available.
   [Embedding key and LRU](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/LLamaCPP/llamaCppEmbeddingService.cpp#L188-L217),
   [invalidation](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/LLamaCPP/llamaCppEmbeddingService.cpp#L39-L51),
   [startup settings](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/Runtime/reviaSession.cpp#L1372-L1374).

An initial text projection before position/state operations could still receive
identical inputs; the audit does not prove that all useful repetition is already
eliminated. Token embedding lookup already gathers rows, and no repeat-rate,
remaining cost, retained-memory or synchronization evidence establishes a useful
new projection cache. This remains a possibility rather than a third workstream.
[Embedding gather and adapters](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/src/llama-graph.cpp#L2284-L2326).

## Contract before any later integration

Pure `W @ x` reuse needs all input bits, matrix/layer/operator identity, immutable
effective weights including adapter identities/scales, shapes/types and relevant
numeric execution settings. Position/KV belongs in a key when directly used by
the cached operation, or must already be represented by its actual input.
Whole-block reuse additionally depends on causal KV/recurrent state. A filename,
role name or repeated human text is insufficient evidence of that identity.

Backend/batch changes can change numeric results even for existing prefix reuse;
RNL's bitwise contract does not transfer automatically to another kernel.
[Upstream numeric warning](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/README.md#L528).

## Two alternative leads and the smallest discriminators

### 1. Qualify existing prompt-prefix reuse

**Observed:** the request flag and backend reuse paths already exist. Revia
preserves `cache_n`/`prompt_n` as token counts and prompt/generation timings;
missing fields remain unknown. Character-prefix estimates are not measured hits.
[Existing numeric metrics](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/LLamaCPP/llamaCppService.cpp#L1347-L1372).

**Hypothesis:** existing reuse may already avoid much of the relevant prefill
cost. Establish a remaining cost before considering a new backend optimization.

**Smallest later experiment:** after separate approval and a pinned actual
runtime/model/template/adapter/backend configuration, compare fixed public
shared-prefix and changed-prefix fixtures with reuse requested on/off. Retain
only fixture IDs, existing token counts, prompt time, first-token/request time
and declared output length. Treat absent metrics and slot/eviction effects as
unknown. No private prompts or new activation telemetry. Keep `/slots` disabled;
it exposes prompt text. This experiment has **not run and is not authorized**.
[Privacy boundary](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/LLamaCPP/llamaCppServerProcess.cpp#L240-L242).

### 2. Establish embedding-cache lifecycle coverage

**Observed:** the key contains a model alias, not an immutable weight/configuration
generation. `ApplySettings` clears the cache, including at normal startup. Alias
insufficiency alone does not establish a reachable bug.
[Health identity check](https://github.com/MahouSenseii/R.E.V.I.A/blob/1b4118f2288ee53c399f382ec5b9775a67f6568f/Private/LLM/LLamaCPP/llamaCppEmbeddingService.cpp#L110-L151).

**Hypothesis:** supported replacement/restart lifecycles may already provide the
necessary invalidation, or may need a stronger generation boundary. That must be
established from the owner/settings/reload call chains before proposing a fix.

**Smallest discriminator:** continue a read-only lifecycle audit first. Only if
a supported same-alias replacement can preserve the service/cache should a later
approved quiescent fake-server fixture use the same public input, orthogonal
normalized vectors `[1,0]` then `[0,1]`, and HTTP counts to distinguish invalidation.
Scaled vectors would normalize identically. Do not add another settings-clear
test or race `ApplySettings` against active reads. Old in-flight completion is a
separate question unless safe owner cancellation/join ordering is established.
No fixture, fix or performance benefit is claimed here.

## Decision and work boundary

Keep RNL's measured results and adverse controls. Hold matrix-cache integration
until a relevant recurring computation and net benefit are demonstrated. Follow
the two leads only within separately authorized scope; negative findings can
close a lead without invalidating the measured toy result.

Independent source/claims review completed before publication. This documentation
step ran no model calls, tests, telemetry, GPU/training work or dependency install.
Research documentation stays on `research/rnl-sandbox`; only the dedicated
`docs/atlas/research.html` snapshot may change on main. No product code, other
Atlas pages, stage promotion or automatic experiment follows this audit.
