# Next-Concept Modeling feasibility

Research checkpoint, 2026-10-31. This is HazyResearch's Next-Concept Modeling
([HazyResearch/ncm](https://github.com/HazyResearch/ncm)). It is unrelated to the "NCM"
retrieval report followed up in [MEMORY_RETRIEVAL.md](MEMORY_RETRIEVAL.md), whose fusion
and mood/persona recommendations concern memory, not generation. Next-Concept Modeling is
not integrated, not installed and disabled.

## What it is

A trained multiscale residual vector-quantized tokenizer maps a text sequence to a
hierarchy of discrete codes. A transformer generator predicts that hierarchy from coarse
to fine, one forward pass per level, generating each level in parallel. It is a separate
model pair, not a Qwen setting, a llama.cpp mode or a planning prompt.

The released checkpoints (Hugging Face collection `hazyresearch/ncm`, MIT) are each
trained for one task or corpus:

- XSum summarization and WMT14 German→English translation
- easy, medium and hard Sudoku
- small, medium and large LM1B generators
- TinyStories and OpenWebText

None is instruction-tuned or conversational. The published comparison is "beats or
matches strong diffusion baselines … while generating 9-31× faster". That speedup is
against diffusion language models, not autoregressive models, and its per-task breakdown
is only in figures.

## Fit for Revia

No released pair can replace or accelerate Main. There is no chat checkpoint, and the
pairs do not share Qwen's tokenizer or vocabulary, so they cannot even serve as a draft
model for Main to verify. A useful
companion integration would need new training of both tokenizer and generator on chat
data, plus a runtime port. That is a research project, not an integration task.

A bounded experiment is possible only on a task that has a released pair. Use XSum or
WMT14 De→En in an isolated optional Python environment on a matched held-out sample, with
the same input set given to Revia's existing Fast and Main models. Record task quality
(ROUGE or sacreBLEU), latency, peak memory, dependencies and setup cost for each. The
result would be a feasibility measurement, not evidence about companion chat, and none of
it belongs in normal startup. It has not been run, and it should not block or share
resources with accepted application work.
