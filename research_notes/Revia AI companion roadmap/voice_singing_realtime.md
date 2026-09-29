# Voice, Singing and Real-Time Conversation Tech for a Local-First AI Companion/VTuber (state as of late Sept 2026)

Context for the report writer: Revia today runs `Qwen3-TTS-12Hz-0.6B-Base` (a cloned voice) on both GPUs (RTX 5070 in BF16, RTX 2070 Super in FP32). The installed `qwen-tts` 0.1.1 API gives no incremental PCM, so Revia generates speech one full sentence at a time. The first sentence is audible 1.2–2.0 s after the reply starts, and the per-sentence real-time factor (RTF) is about 0.35–0.39 on the 5070 and 0.56–0.72 on the 2070S. Speech recognition is whisper.cpp `distil-small.en`, which is English-only. Lip sync is amplitude-based and visemes are listed as "future". Karaoke plays WAVs the user supplies, because "her Qwen3-TTS voice cannot sing a melody". All of this comes from the sibling file `revia_current_state.md` in this folder, which cites README L352-371, docs/TTS_PERFORMANCE.md and Config/avatar.json. This file covers only outside technology. All web data was collected 2026-09-28/29.

---

## 1. Local TTS in 2026: quality, streaming/TTFA, VRAM, emotion control, cloning, license, Windows

### Takeaway
Open-weight TTS has improved a lot. The best open models now trail the best closed APIs by roughly 100 Elo, where the gap was over 200 in 2023. The catch is licensing: most of the top open-weight models are non-commercial (Fish Audio S2 Pro, Voxtral TTS, Higgs Audio v3 apart from its creator grant, Spark-TTS, F5-TTS's official weights, XTTS-v2). For a monetized VTuber, the permissive options are:
- **Qwen3-TTS** (Apache-2.0), the model Revia already uses.
- **CosyVoice 3** (Apache-2.0).
- **Chatterbox / Chatterbox-Turbo** (MIT, with paralinguistic tags).
- **GPT-SoVITS v2ProPlus** (MIT, small).
- **Kokoro-82M** (Apache-2.0, no cloning; the obvious replacement for the SAPI fallback).
- **Step-Audio-EditX** (Apache-2.0, 3B, good for emotion and paralinguistic editing).
- **Orpheus** (Llama-derived weights license), **Dia2** (Apache-2.0, English only) and **Zonos** (Apache-2.0).

Revia's biggest gap is not model choice. The model already supports ~97 ms first-packet streaming, but Revia's installed wrapper does not expose incremental PCM.

### Cited Findings

**Leaderboards (Artificial Analysis Speech Arena: blind-vote Elo that changes over time; snapshots disagree)**
- Artificial Analysis open-weights leaderboard, fetched 2026-09-29: Breeze TTS 2 (BreezeBlue) 1207, Aug 2026; Fish Audio S2 Pro 1118; Step Audio EditX 1094; Voxtral TTS 1080; Kokoro 82M v1.0 1065; NVIDIA Magpie-Multilingual 357M 1063; Maya1 1046; OpenAudio S1 Mini 1042; Higgs Audio V3 TTS 1037; Chatterbox 1023; Zonos-v0.1 1000; VibeVoice 1.5B 955; OpenVoice v2 952; XTTS v2 916; StyleTTS 2 893. Qwen3-TTS, CosyVoice 3, GPT-SoVITS, IndexTTS2 and Orpheus are **not listed**. — [Artificial Analysis open-weights](https://artificialanalysis.ai/text-to-speech/leaderboard/provider-voice/open-weights)
- Artificial Analysis overall leaderboard (same fetch): Eleven v4 1315 ($80/1M chars, Sept 2026); Cartesia Sonic 3.6 1275 ($49); Google Gemini 3.8 Flash TTS 1267 ($16.5); Alibaba Qwen-Audio-3.0-TTS-Plus 1258 ($19.3); Inworld Realtime TTS-2 1244 ($20.8); Gemini 3.8 Flash-Lite TTS 1241 ($11); Speechify Simba 3.2 1239 ($6.6); … ElevenLabs v3 Conversational 1196 ($50); Eleven v3 1169 ($100); MiniMax Speech 2.8 HD 1171; Fish Audio S2.1 Pro 1141 (proprietary, June 2026). Breeze TTS 2 is the only open model in the top 10. — [Artificial Analysis leaderboard](https://artificialanalysis.ai/text-to-speech/leaderboard)
- An earlier snapshot (May 2026, updated Aug 1 2026) has Fish Audio S2 Pro at 1128.7 as the top open model (rank 11 overall), with Kokoro at 1056.2 and Voxtral at 1055.9. It puts the commercial-vs-open gap at 223 Elo in 2023 and 81 in early 2026. Kokoro scores 1065.8 on "knowledge sharing" but 976.0 on "entertainment". — [OfflineTTS arena summary](https://www.offlinetts.com/blog/tts-arena-leaderboard-2026/). *Conflict:* these Elo values differ from the live AA page (e.g. Fish S2 Pro 1128.7 vs 1118), which is normal Elo drift between snapshots.

**Qwen3-TTS (Alibaba). Revia's current engine**
- Released 2026-01-22 under Apache-2.0. Two sizes: 0.6B (Base, CustomVoice) and 1.7B (Base, CustomVoice, VoiceDesign). Covers 10 languages: zh, en, ja, ko, de, fr, ru, pt, es, it. vLLM has day-0 support. — [QwenLM/Qwen3-TTS GitHub](https://github.com/QwenLM/Qwen3-TTS)
- "All models support streaming generation", with "end-to-end synthesis latency as low as 97ms". Natural-language instruction control (tone, emotion, prosody) is documented for the **CustomVoice and VoiceDesign** variants. **Base** does the 3-second voice clone. — [Qwen3-TTS GitHub](https://github.com/QwenLM/Qwen3-TTS)
- Seed-TTS test WER: 1.7B-Base 0.77 zh / 1.24 en; 0.6B-Base 0.92 zh / 1.32 en. Speaker similarity 0.75–0.83. — [Qwen3-TTS GitHub](https://github.com/QwenLM/Qwen3-TTS)
- Tech report: the 12 Hz tokenizer (16-layer multi-codebook, causal ConvNet) allows "immediate first-packet emission (97 ms)". The 25 Hz tokenizer is single-codebook with block-wise DiT. Trained on more than 5M hours. — [arXiv 2601.15621](https://arxiv.org/abs/2601.15621v1)
- VRAM (secondary sources, unverified): 0.6B about 4 GB, 1.7B about 6–8 GB (bf16, with FlashAttention 2 also quoted at 8–12 GB). — [Clore.ai guide](https://docs.clore.ai/guides/audio-and-voice/qwen3-tts); [DEV guide](https://dev.to/czmilo/qwen3-tts-the-complete-2026-guide-to-open-source-voice-cloning-and-ai-speech-generation-1in6)

**Fish Audio S2 Pro (and OpenAudio S1 Mini)**
- 5B total: a 4B "Slow AR" plus a 400M "Fast AR", using an RVQ codec (10 codebooks, ~21 Hz). 80+ languages, with Tier 1 being ja/en/zh. — [HF fishaudio/s2-pro](https://huggingface.co/fishaudio/s2-pro)
- Streaming on one H200: RTF 0.195, TTFA about 100 ms. Free-form inline `[tag]` control such as `[whisper]`, `[laughing]` and `[professional broadcast tone]`, with "15,000+ unique tags". — [HF fishaudio/s2-pro](https://huggingface.co/fishaudio/s2-pro)
- License: Fish Audio Research License. Research and non-commercial use are free; commercial use needs a separate license (business@fish.audio). — [S2 Pro LICENSE](https://huggingface.co/fishaudio/s2-pro/blob/main/LICENSE.md); [Fish blog on "open source"](https://fish.audio/blog/what-we-mean-by-open-source-for-s2/)
- Community GGUF quantizations exist. — [rodrigomt/s2-pro-gguf](https://huggingface.co/rodrigomt/s2-pro-gguf)
- Bento notes it is "open weights, not fully open source" and that self-hosted latency depends on hardware. — [BentoML Apr 2026](https://www.bentoml.com/blog/exploring-the-world-of-open-source-text-to-speech-models)

**Voxtral TTS (Mistral, March 2026)**
- 4B, **CC BY-NC 4.0** (non-commercial). 9 languages: en, fr, es, de, it, pt, nl, ar, hi. **No Japanese.** TTFA of 70 ms at single concurrency. Needs at least 16 GB of GPU memory. Supports streaming and vLLM-Omni. — [HF Voxtral-4B-TTS-2603](https://huggingface.co/mistralai/Voxtral-4B-TTS-2603)
- Reference audio needed for cloning: the HF card says 10 s, while the paper and secondary sources say 2–3 s (*conflict*). Human raters preferred it over ElevenLabs Flash v2.5 68.4% of the time for multilingual cloning. — [arXiv 2603.25551](https://arxiv.org/html/2603.25551v1); [Arun Baby summary](https://www.arunbaby.com/speech-tech/0067-voxtral-mistral-tts-voice-cloning/)

**CosyVoice 3 (Fun-CosyVoice3-0.5B-2512, Dec 2025)**
- 0.5B, Apache-2.0 for weights and code. Text-in and audio-out streaming with latency "as low as 150ms". 9 languages plus 18+ Chinese dialects. Better than CosyVoice 2 on content consistency, similarity and prosody. — [HF Fun-CosyVoice3-0.5B-2512](https://huggingface.co/FunAudioLLM/Fun-CosyVoice3-0.5B-2512)

**GPT-SoVITS (RVC-Boss)**
- MIT license. v4 released 2025-04-22 (native 48 kHz output). v2Pro released 2025-06-06. — [GPT-SoVITS releases](https://github.com/RVC-Boss/gpt-sovits/releases)
- Versions per the official wiki:
  - v2 and later: zh/ja/en/ko/yue.
  - v2Pro: 133M+77M parameters, at v2 speed and cost, zero-shot similarity 0.709.
  - v2ProPlus: 152M+77M parameters, slightly more VRAM than v2Pro, best zero-shot similarity at 0.737, "surpassing v4".
  - The wiki says there is "no need to continue using v3/v4".
  - [GPT-SoVITS features wiki](https://github.com/RVC-Boss/GPT-SoVITS/wiki/GPT%E2%80%90SoVITS%E2%80%90features-(%E5%90%84%E7%89%88%E6%9C%AC%E7%89%B9%E6%80%A7))
- Few-shot fine-tuning from about 1 minute of audio. — [LocalAIMaster GPT-SoVITS guide](https://localaimaster.com/blog/gpt-sovits-voice-cloning-guide)

**Chatterbox / Chatterbox-Turbo / Multilingual (Resemble AI)**
- Turbo has 350M parameters and one-step mel decoding (down from 10), in English. The Multilingual variant covers 23 languages including ja, ko and zh. Native tags include `[cough]`, `[laugh]` and `[chuckle]`. CFG weight and "exaggeration" controls both default to 0.5. Cloning uses about 10 s of reference audio. MIT license. The **PerTh watermark is on by default**. — [HF chatterbox-turbo](https://huggingface.co/ResembleAI/chatterbox-turbo)
- Resemble claims about 75 ms latency and up to 6x real time on a modern GPU. It also mentions `[sigh]` and `[whisper]` tags. — [Resemble Chatterbox-Turbo page](https://www.resemble.ai/learn/models/chatterbox-turbo)

**Kokoro-82M**
- Apache-2.0. v1.0 released 2025-01-27. 8 languages, 54 voices. **No voice cloning.** Uses the `misaki` G2P library. No timestamps documented. Beware of scam sites. — [HF hexgrad/Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M)
- Runs in a browser via WebGPU/WASM; hosted versions cost about $0.65/1M characters. — [OfflineTTS](https://www.offlinetts.com/blog/tts-arena-leaderboard-2026/)

**Orpheus TTS (Canopy Labs, March 2025)**
- 3B, Llama-3B backbone. Inline emotive tags such as laugh and sigh. About 200 ms streaming latency, down to about 100 ms with input streaming. — [canopyai/Orpheus-TTS](https://github.com/canopyai/Orpheus-TTS)
- Fits in about 8 GB of VRAM with Q4_K_M/Q8_0 GGUF. The code is Apache-2.0 but the weights carry a Llama-derived license (secondary source). — [LocalAIMaster Orpheus guide](https://localaimaster.com/blog/orpheus-tts-setup-guide)

**Sesame CSM-1B**
- Apache-2.0, English. Llama backbone plus a small decoder that emits Mimi RVQ codes. — [HF sesame/csm-1b](https://huggingface.co/sesame/csm-1b)
- Community reports say the 1B release is "very immature" and needs a lot of compute to reach acceptable real-time latency. The Maya/Miles demo system itself is not open. — [HF discussion](https://huggingface.co/sesame/csm-1b/discussions/10); [The Decoder](https://the-decoder.com/sesame-releases-csm-1b-ai-voice-generator-as-open-source/)

**Dia / Dia2 (Nari Labs)**
- Dia2 comes as 1B and 2B checkpoints. It is a streaming dialogue TTS that starts from the first few words, can be conditioned on audio, and does nonverbals such as laughter, coughing and sighing. English only; generation is capped at 2 minutes. — [nari-labs/dia2](https://github.com/nari-labs/dia2); [HF Dia2-2B](https://huggingface.co/nari-labs/Dia2-2B)
- Apache-2.0. Caveats: no fixed voice identity by default, and "nonverbal tags unpredictable". — [BentoML](https://www.bentoml.com/blog/exploring-the-world-of-open-source-text-to-speech-models)

**IndexTTS2 (bilibili)**
- Decouples timbre from emotion, controls emotion through text descriptions, and adds duration control. — [index-tts2.org](https://index-tts2.org/)
- **License is contested across unofficial sites:** some say Apache-2.0, some MIT, and one says it is non-commercial with commercial licensing via indexspeech@bilibili.com. — [index-tts2.com](https://index-tts2.com/); [indextts2.pro](https://indextts2.pro/). *Unverified: the official repo's LICENSE file was not fetched.*

**Step-Audio-EditX (StepFun, 2025-11-12)**
- 3B, Apache-2.0. Native Mandarin, English, Sichuanese and Cantonese; Japanese and Korean via tags. Paralinguistic markers include breathing, laughter, sighing, throat clearing, coughing, hesitation, surprise vocalizations and questioning tones. Emotions: angry, happy, sad, fearful, surprised, disgusted. Styles: whisper, serious, child-like, elderly and others. BF16 needs at least 12 GB; AWQ 4-bit needs 6–8 GB. Recommended input is under 30 s per call. — [HF stepfun-ai/Step-Audio-EditX](https://huggingface.co/stepfun-ai/Step-Audio-EditX)
- It is an iterative **audio editor** plus zero-shot TTS. After editing, paralinguistic reproduction is "comparable" to built-in closed-model voices. — [Step-Audio-EditX GitHub](https://github.com/stepfun-ai/Step-Audio-EditX); [arXiv 2511.03601](https://arxiv.org/html/2511.03601)

**Higgs Audio v3 TTS (Boson AI, 2026)**
- About 4B parameters. 102 languages, 85 of them rated production-quality. 21 emotion tokens (elation, amusement, … shame, helplessness). Style tags for singing, shouting and whispering, plus 9 sound effects and speed/pitch controls. RTF 0.262 on H100, with "sub-second" TTFA when streaming. — [HF bosonai/higgs-audio-v3-tts-4b](https://huggingface.co/bosonai/higgs-audio-v3-tts-4b)
- License: "Boson Higgs TTS 3 Research and Non-Commercial License". A **Creator Use Grant** says "creators may use Higgs TTS 3 to make and monetize podcasts, videos, and social posts for free" with attribution. Production APIs and embedding it in a product need a commercial license. No cloning without consent. — [HF card](https://huggingface.co/bosonai/higgs-audio-v3-tts-4b)

**Kyutai TTS 1.6B (actually 1.8B, July 2025)**
- Weights are CC-BY 4.0. English and French. Delayed-streams modeling starts speaking after a few words. About 220 ms from first text token to first audio; 350 ms with 32 concurrent streams on one L40S. — [Kyutai TTS](https://kyutai.org/tts/); [erogol analysis](https://erogol.substack.com/p/model-check-kyutaitts-streaming-text)
- **Cloning is restricted** to pre-computed voice embeddings from the `tts-voices` repo; direct cloning is disabled. — [HF kyutai/tts-1.6b-en_fr](https://huggingface.co/kyutai/tts-1.6b-en_fr)

**VibeVoice (Microsoft)**
- MIT. Microsoft **removed the VibeVoice-TTS code** on 2025-09-05 "since responsible use of AI…"; the weights remain on HF. VibeVoice-Realtime-0.5B has about 300 ms first-audible latency, and the repo lists experimental multilingual voices. An ASR-Streaming model was added 2026-09-03. — [microsoft/VibeVoice](https://github.com/microsoft/VibeVoice)
- Realtime-0.5B is single-speaker and "intended for English". It uses a 7.5 Hz acoustic tokenizer and accepts streaming text input. — [HF VibeVoice-Realtime-0.5B](https://huggingface.co/microsoft/VibeVoice-Realtime-0.5B)

**MegaTTS3 (ByteDance)**
- Apache-2.0, 0.45B DiT, zh/en. The **WaveVAE encoder is not released**, so cloning needs pre-extracted latents or samples sent through a verification queue. No streaming. Last roadmap update March 2025. — [bytedance/MegaTTS3](https://github.com/bytedance/MegaTTS3)

**Spark-TTS (0.5B)**
- The code is Apache-2.0, but the **weights are CC BY-NC-SA 4.0**; the license was changed from Apache "due to restrictions in the training data". — [HF SparkAudio/Spark-TTS-0.5B](https://huggingface.co/SparkAudio/Spark-TTS-0.5B)
- zh/en. RTF 0.136 on L20. Controls for gender, pitch and speaking rate. Last update March 2025. — [SparkAudio/Spark-TTS GitHub](https://github.com/sparkaudio/spark-tts)

**F5-TTS, Zonos, XTTS, others**
- F5-TTS: the code is MIT, but the pretrained weights are **CC-BY-NC** (Emilia training data). OpenF5-TTS-Base is an Apache-2.0 alternative. — [SWivid/F5-TTS](https://github.com/swivid/f5-tts); [HF OpenF5-TTS-Base](https://huggingface.co/mrfakename/OpenF5-TTS-Base)
- Zonos-v0.1: Apache-2.0. — [Zyphra](https://www.zyphra.com/our-work/beta-release-of-zonos-v0-1)
- XTTS-v2: Coqui Public Model License, non-commercial, and Coqui shut down in 2024. NeuTTS Air: 0.5B, English, on-device, PerTh-watermarked. — [BentoML](https://www.bentoml.com/blog/exploring-the-world-of-open-source-text-to-speech-models)

**Compact comparison table (each cell traces to the sources above; "?" = not found)**

| Model | Params | License (monetized use) | Langs (JA?) | Streaming / TTFA | Emotion / nonverbal | Cloning | VRAM |
|---|---|---|---|---|---|---|---|
| Qwen3-TTS | 0.6B / 1.7B | Apache-2.0 ✅ | 10 (JA yes) | yes, 97 ms first packet | NL instructions (CustomVoice/VoiceDesign) | 3 s (Base) | ~4 / ~8 GB (secondary) |
| CosyVoice 3 | 0.5B | Apache-2.0 ✅ | 9 + dialects | bi-streaming, ~150 ms | ? (instructions not verified) | yes | ? |
| Chatterbox-Turbo | 350M | MIT ✅ (watermark on) | EN (ML variant 23, JA yes) | ~75 ms claim | `[laugh]`, `[cough]`, `[chuckle]`, exaggeration | ~10 s | "less than prior" |
| GPT-SoVITS v2ProPlus | 152M+77M | MIT ✅ | zh/ja/en/ko/yue | ? | reference-driven | zero-shot + ~1 min fine-tune | low |
| Kokoro | 82M | Apache-2.0 ✅ | 8 | fast, CPU OK | limited | ❌ | very low |
| Step-Audio-EditX | 3B | Apache-2.0 ✅ | zh/en (+ja/ko tags) | no (≤30 s clips) | 10+ paralinguistic, 6 emotions, styles | yes | 12 GB bf16 / 6–8 GB AWQ |
| Orpheus | 3B | Llama-derived weights ⚠️ | EN | ~200 ms (~100 ms input-streaming) | emotive tags | yes | ~8 GB GGUF Q4/Q8 |
| Dia2 | 1B / 2B | Apache-2.0 ✅ | EN | streaming | nonverbals (unpredictable) | audio-conditioned | ? |
| Fish S2 Pro | 5B | Research license ❌ (paid) | 80+ (JA tier 1) | ~100 ms (H200) | 15k+ free-form tags | yes | ? (GGUF exists) |
| Voxtral TTS | 4B | CC BY-NC ❌ | 9 (no JA) | 70 ms | "expressive" | 3–10 s | ≥16 GB |
| Higgs Audio v3 | ~4B | Research + Creator grant ⚠️ | 102 | sub-second | 21 emotions, singing/shout/whisper, SFX | yes | ? |
| Kyutai TTS | 1.8B | CC-BY 4.0 ✅ | EN/FR | ~220 ms | ? | preset embeddings only | ? |
| VibeVoice-Realtime | 0.5B | MIT (TTS code pulled) ⚠️ | EN (+exp.) | ~300 ms | limited | ? | ? |
| Spark-TTS | 0.5B | CC BY-NC-SA ❌ | zh/en | no | pitch/speed/gender | yes | ? |
| MegaTTS3 | 0.45B | Apache-2.0 (encoder withheld) ⚠️ | zh/en | no | ? | gated | ? |
| F5-TTS (official) | ? | CC-BY-NC ❌ (OpenF5 Apache) | multi | ? | ? | yes | ? |
| IndexTTS2 | ? | contested ⚠️ | zh/en | ? | text/ref emotion, duration | yes | ? |

### Inferences
- **The best local upgrade is streaming Qwen3-TTS output, not a different model.** Upstream Qwen3-TTS documents 97 ms first-packet streaming, while Revia's `qwen-tts` 0.1.1 wrapper cannot emit incremental PCM, which is why the first sentence takes 1.2–2.0 s. Streaming chunks could cut perceived latency several-fold. That could mean moving to a newer `qwen-tts` release or the vLLM(-Omni) path. vLLM is Linux-first, so Windows support has to be checked before relying on it; this is general knowledge, not verified here.
- **Emotion control on a cloned voice is a Qwen3-TTS limitation.** Instruction control is documented only for CustomVoice and VoiceDesign, while cloning is on Base. Revia's cloned voice probably cannot take instructions like "say this sadly" directly. Options:
  - (a) Keep several emotion-specific reference clips per character and switch the Base prompt audio according to Revia's emotion vector. This is cheap and works today.
  - (b) Post-process key lines with Step-Audio-EditX (Apache-2.0; AWQ at 6–8 GB fits the 2070S) to add laughter, sighs or a style.
  - (c) Try Chatterbox-Turbo or Multilingual (MIT) as a second engine for tagged nonverbals.
- **Replace Windows SAPI with Kokoro-82M as the fallback.** It is Apache-2.0, CPU-capable and far more natural (AA Elo 1065 vs SAPI's legacy voices). The trade-off is that it cannot clone, so the fallback voice would not match Revia's cloned voice. Chatterbox-Turbo (350M) is a GPU fallback that can clone.
- **For monetized streaming, avoid these as default engines:** Fish S2 Pro, Voxtral, Spark-TTS, official F5-TTS weights, XTTS-v2 and MegaTTS3 (because of gated cloning). Higgs v3's creator grant covers monetized "podcasts, videos, and social posts". Whether live streams count, and whether shipping it inside Revia counts as product embedding (which needs a commercial license), is ambiguous.
- **VRAM budget.** The RTX 2070 Super is Turing. It lacks fast BF16, which matches Revia running FP32 there (general knowledge). So 3–5B models (Fish S2 Pro, Voxtral, Higgs v3, Orpheus) will not fit comfortably alongside a local LLM on 12 GB + 8 GB. The practical local range stays at 0.35–1.7B.

### Gaps
- No independent head-to-head TTFA or VRAM measurements on consumer GPUs (RTX 5070/2070S class) for most models. Vendor figures come from H100/H200/L40S/L20.
- Qwen3-TTS, CosyVoice 3, GPT-SoVITS, IndexTTS2 and Orpheus are missing from the Artificial Analysis open-weights leaderboard, so there is no neutral Elo for them.
- No details found on Breeze TTS 2 (BreezeBlue), the top open-weight model on AA: license, size and languages are unknown. Same for NVIDIA Magpie-Multilingual 357M and Maya1.
- The IndexTTS2 official license and the Orpheus weights license text were not verified from primary files.
- Not verified: whether Qwen3-TTS Base supports any instruction or emotion control on cloned voices, and whether a newer `qwen-tts` package exposes streaming PCM on Windows.
- Windows support is only confirmed for a few projects (GPT-SoVITS and Seed-VC ship Windows paths; Moshi explicitly does not). Most TTS repos are Linux-first.

---

## 2. Cloud TTS and voice APIs for an optional hybrid mode (latency, cost)

### Takeaway
ElevenLabs' new **Eleven v4** (launched 2026-09-28) tops the Artificial Analysis arena at 1315 Elo ($80/1M characters). The **v4 Turbo** variant has about 150 ms median time-to-first-speech with bidirectional streaming. Cartesia Sonic 3.6 is next at 1275 ($49/1M, roughly 90–190 ms). Google's Gemini 3.8 Flash TTS is the best value near the top at 1267 Elo for $16.5/1M. OpenAI's Realtime API is the simplest speech-to-speech option, but it uses **preset voices only**; custom voices go through sales for eligible customers. That makes it a poor fit for a character with a cloned voice. For a hybrid Revia, the natural design is "local LLM → cloud TTS with a cloned voice", using ElevenLabs, Cartesia or Hume. Azure is the only one here with documented viseme events.

### Cited Findings
- **ElevenLabs v4 / v4 Turbo (2026-09-28).** New architecture, "most emotive yet". Clone a voice from 10 s of audio. 90+ languages. v4 Turbo supports bidirectional streaming with a median time-to-first-speech of about 150 ms. Available on the free tier through the API. — [TechCrunch](https://techcrunch.com/2026/09/28/elevenlabs-new-v4-speech-model-supports-more-expression-control-and-90-languages/); [Unite.AI](https://www.unite.ai/elevenlabs-launches-eleven-v4-with-low-latency-turbo-variant/)
- **ElevenLabs API pricing (Sept 2026, per 1,000 characters).** $0.10 for Eleven v3 and Multilingual v2; $0.05 for Flash, Turbo and v3 Conversational. Flash v2.5: about 75 ms, 32 languages, 40k characters per request. v3: 70+ languages, 5k characters per request. — [ElevenLabs pricing](https://elevenlabs.io/pricing); [Puter pricing breakdown](https://developer.puter.com/tutorials/elevenlabs-api-pricing/)
- **ElevenLabs timestamps.** The "with timestamps" endpoint returns character-level `characters`, `character_start_times_seconds` and `character_end_times_seconds`, for both raw and normalized text. The fetched page did not document whether this works over WebSocket. — [ElevenLabs API docs](https://elevenlabs.io/docs/api-reference/text-to-speech/convert-with-timestamps)
- **Cartesia Sonic.**
  - Sonic 3 (2025-10-27) added laughter and emotion. Sonic 3.5 went GA in May 2026. — [CallSphere](https://callsphere.ai/blog/vw1a-cartesia-sonic-3-april-2026-laughter-emotion-tts); [InVideo](https://invideo.io/blog/cartesia-sonic-ai-voice/)
  - Sonic 3.6 (Aug 2026) leads both Artificial Analysis arenas at release. — [MarkTechPost](https://www.marktechpost.com/2026/08/18/cartesia-ships-sonic-3-6-a-streaming-tts-model-that-now-leads-both-artificial-analysis-speech-arenas/)
  - Latency: vendor claims under 90 ms model latency. Measured medians are about 166 ms including network (Vapi, June 2026) and about 190 ms per Cartesia's changelog. Sonic Turbo TTFA is 40 ms. — [InVideo](https://invideo.io/blog/cartesia-sonic-ai-voice/)
  - Plans: Free ≈27 min; Pro $5 ≈133 min; Startup $49 ≈1,667 min; Scale $299 ≈10,667 min. — [eesel Sonic 3 pricing](https://www.eesel.ai/blog/cartesia-sonic-3-pricing)
- **OpenAI (per 1M tokens).**
  - gpt-realtime-2.1: audio in $32, cached $0.40, audio out $64; text $4 in / $24 out.
  - gpt-realtime-2.1-mini and gpt-realtime-mini: audio $10 in / $20 out; text $0.60 / $2.40.
  - gpt-4o-mini-tts: $12 audio out plus $0.60 text in.
  - gpt-4o-transcribe and Whisper: $0.006/min.
  - [OpenAI pricing](https://developers.openai.com/api/docs/pricing)
  - Rough conversion: about 600 tokens per minute of input audio and about 1,200 per minute of output, so mini costs about $0.006/min in and $0.024/min out (secondary). — [eesel](https://www.eesel.ai/blog/gpt-realtime-mini-pricing)
  - Custom voices work in the TTS and Realtime APIs but are "limited to eligible customers" through sales. — [OpenAI custom voices](https://developers.openai.com/api/docs/guides/custom-voices)
  - Otherwise Realtime is limited to preset voices, and a chained pipeline is recommended when a cloned voice is needed. — [Forasoft 2026 guide](https://www.forasoft.com/blog/article/openai-realtime-api-voice-agent-production-guide-2026)
- **Hume Octave 2 (Oct 2025).** About 100 ms model latency excluding network ("under 200ms"), 40% faster than v1, costs cut 50%. — [Hume blog](https://www.hume.ai/blog/octave-2-launch)
  - Pay-as-you-go about $7.60/1M characters. Cloning from 15 s of audio on the Creator plan ($7 intro, $14 normally). Emotion inferred from context via an LLM (secondary). — [Dupple review](https://dupple.com/reviews/hume)
- **Google Gemini TTS.** Gemini 3.8 Flash TTS: 1267 Elo, $16.5/1M characters. Flash-Lite: 1241, $11/1M. Gemini 3.1 Flash TTS: 1203, $18.3/1M. — [Artificial Analysis](https://artificialanalysis.ai/text-to-speech/leaderboard)
- **Azure Speech.** Neural voices $15/1M characters; HD $22/1M. Commitment tiers run from $12/1M effective (80M/month) down to $7.50/1M. — [TextToLab Azure pricing](https://texttolab.com/blog/azure-text-to-speech-pricing); [Azure pricing page](https://azure.microsoft.com/en-us/pricing/details/speech/)
  - Visemes: 22 viseme IDs, SVG, or 3D blend shapes through the `VisemeReceived` event in the Speech SDK. The REST API does not support visemes. — [Microsoft Learn viseme](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/how-to-speech-synthesis-viseme)
- **Other high-Elo cloud TTS:** Inworld Realtime TTS-2 at 1244 / $20.8, TTS-2 Flash at 1213 / $10.4; Qwen-Audio-3.0-TTS-Plus at 1258 / $19.3; Speechify Simba 3.2 at 1239 / $6.6. — [Artificial Analysis](https://artificialanalysis.ai/text-to-speech/leaderboard)

### Inferences
- **Hybrid design.** Keep the LLM and persona local and send sentence chunks to a cloud TTS that holds Revia's cloned voice: ElevenLabs v4 Turbo (~150 ms), Cartesia Sonic 3.6/Turbo (40–190 ms) or Hume Octave 2 (~100 ms). This keeps the voice consistent while cutting local VRAM use to zero during cloud mode.
- **Cost at one hour of speech per day** (about 54k characters/hour at 15 characters/s × 3,600 s, a rough assumption): ElevenLabs Flash about $2.7/hour, v4 about $4.3/hour at $80/1M, Gemini 3.8 Flash TTS about $0.9/hour, Azure Neural about $0.8/hour.
- **OpenAI Realtime is a poor fit** for a character whose identity rests on a custom cloned voice unless the owner qualifies for custom voices. It remains an option for an "assistant mode" with a preset voice.
- **Voice likeness consent.** Cloning a voice for commercial cloud use means following the vendor's terms (e.g. Higgs and Chatterbox forbid non-consensual cloning). The owner should use a voice they own or have licensed.

### Gaps
- Official Eleven v4 API price per character: the AA listing shows $80/1M, but the ElevenLabs price page was not directly confirmed.
- Current Cartesia per-character API price for Sonic 3.6 beyond the AA figure ($49/1M), and whether Cartesia's WebSocket returns word or phoneme timestamps, were not verified.
- Whether ElevenLabs v4 keeps v3-style inline audio tags (e.g. `[laughs]`, `[whispers]`) was not verified.
- Gemini TTS cloning availability was not checked.

---

## 3. Speech-to-text, VAD and end-of-turn detection

### Takeaway
whisper.cpp is still a reasonable Windows-native choice; faster-whisper has only a small GPU edge. Revia's specific weakness is `distil-small.en`, which is English-only. The best 2026 upgrade candidates are:
- **Parakeet-TDT-0.6B-v3** (CC-BY-4.0, 25 European languages, very fast, ONNX/sherpa-onnx builds, but no Japanese or Chinese).
- **Qwen3-ASR-0.6B/1.7B** (Apache-2.0, 30 languages plus 22 dialects, strong on singing; streaming only via vLLM).
- **NVIDIA Nemotron-3.5-ASR-Streaming-0.6B** (40 locales including Japanese, 80 ms–1.12 s chunks, OpenMDW license, Linux-listed).
- **Moonshine v2** (edge streaming, 50–258 ms).

For turn-taking, the standard stack is **Silero VAD v6** followed by **Pipecat Smart Turn v3.2** (BSD-2, 23 languages, 8 MB, about 10–100 ms on CPU). LiveKit's turn detector is an alternative tied to its framework.

### Cited Findings
- **HF Open ASR Leaderboard (blog, 2025-11-21).** Conformer-encoder plus LLM-decoder models (Canary-Qwen-2.5B, Granite-Speech-3.3-8B, Phi-4-MM) are the most accurate in English. CTC and TDT decoders are 10–100x faster. Parakeet CTC 1.1B: RTFx 2793.75 at WER 6.68, vs Whisper large-v3 at RTFx 68.56 and WER 6.43. Closed systems still lead on long-form audio. — [HF blog](https://huggingface.co/blog/open-asr-leaderboard)
- **Canary-Qwen-2.5B tops the leaderboard at 5.63% average WER.** Whisper large-v3 averages 7.44% and large-v3-turbo runs at about 216x real time (secondary). — [Gladia](https://www.gladia.io/blog/best-open-source-speech-to-text-models); [Northflank](https://northflank.com/blog/best-open-source-speech-to-text-stt-model-in-2026-benchmarks). *Conflict:* the HF blog gives Whisper large-v3 a 6.43 WER; the leaderboard version and dataset mix differ.
- **Parakeet-TDT-0.6B-v3.** 600M parameters, CC-BY-4.0, 25 European languages (no ja/zh/ko), 6.34% average WER on the Open ASR Leaderboard. — [HF nvidia/parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3)
  - ONNX and sherpa-onnx exports exist (fp16 encoder, int8 decoder), which suits a C++/Windows host without Python. — [HF istupakov ONNX](https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx); [sherpa-onnx NeMo models](https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-transducer/nemo-transducer-models.html)
- **Qwen3-ASR (2026-01-29, Apache-2.0).** 0.6B and 1.7B. 30 languages plus 22 Chinese dialects, with language ID. LibriSpeech WER 1.63 offline and 1.95 streaming (1.7B). Streaming **only via the vLLM backend**. Singing: M4Singer WER 5.98, full songs with backing music (EntireSongs-zh) 13.91. — [QwenLM/Qwen3-ASR](https://github.com/QwenLM/Qwen3-ASR)
  - The 0.6B model's streaming WER on LibriSpeech is 2.54. — [arXiv 2601.21337](https://arxiv.org/pdf/2601.21337)
  - **Qwen3-ForcedAligner-0.6B**: word or character timestamps for 11 languages including Japanese, up to 5 min of audio, 32.4 ms average alignment error. — [QwenLM/Qwen3-ASR](https://github.com/QwenLM/Qwen3-ASR)
- **NVIDIA Nemotron streaming ASR.**
  - The English 0.6B model (Jan 2026) is a cache-aware FastConformer-RNNT with chunk sizes of 80, 160, 560 and 1120 ms, plus punctuation. — [HF nemotron-speech-streaming-en-0.6b](https://huggingface.co/nvidia/nemotron-speech-streaming-en-0.6b); [HF blog](https://huggingface.co/blog/nvidia/nemotron-speech-asr-scaling-voice-agents)
  - **Nemotron-3.5-ASR-Streaming-0.6B** (HF 2026-06-04): 40 locales, including Japanese (ja-JP CER 11.48%); English WER 7.91% at 1.12 s chunks. Licensed under **OpenMDW-1.1**. Runs on NeMo, Transformers ≥5.13 or NeMo-Speech.cpp. Only Linux and L4T are listed as supported OSes. — [HF nemotron-3.5-asr-streaming-0.6b](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b)
- **Moonshine v2 (arXiv, Feb 2026).** Sliding-window streaming encoder. Latency on Apple M3: Tiny 50 ms, Small 148 ms, Medium 258 ms (43.7x faster than Whisper large-v3). WER: Tiny 12.66%, Tiny-streaming 12.00%, Base 10.07%. — [arXiv 2602.12241](https://arxiv.org/html/2602.12241v1)
- **Kyutai STT.** `stt-1b-en_fr` has a 500 ms delay; `stt-2.6b-en` has 2.5 s. Its semantic VAD predicts "the probability that the user is done talking", but this is in the Rust server only. Serves 400 streams on an H100. — [Kyutai STT](https://kyutai.org/stt/)
- **whisper.cpp vs faster-whisper.**
  - RTX 4070, large-v3: faster-whisper about 12x real time vs whisper.cpp CUDA about 8x (secondary benchmark). whisper.cpp needs no Python and supports CUDA, Vulkan and Metal. — [PromptQuorum 2026](https://www.promptquorum.com/power-local-llm/local-whisper-stt-comparison-2026)
  - RTX 3070 Ti, large-v2, 13 minutes of audio: faster-whisper fp16 1m03s vs whisper.cpp with flash-attention 1m05s, so they are comparable. — [faster-whisper (PyPI/README benchmarks)](https://pypi.org/project/faster-whisper/)
- **VibeVoice-ASR** (7B, 50+ languages) plus ASR-Streaming (2026-09-03), MIT. — [microsoft/VibeVoice](https://github.com/microsoft/VibeVoice)
- **Silero VAD v6.** 16% fewer errors on noisy real-life data and 11% fewer on multi-domain validation, with no latency change from v5. — [silero-vad v6.0 discussion](https://github.com/snakers4/silero-vad/discussions/678)
  - 512-sample (32 ms) chunks at 16 kHz. — [Soniqo guide](https://soniqo.audio/guides/vad)
  - With the right context window it cut clipped short words from 8% to 0% and noise/music false-accepts from 41% to 15%. — [saypi PR #661](https://github.com/Pedal-Intelligence/saypi-userscript/pull/661)
  - whisper.cpp has an open issue to support v6. — [whisper.cpp #3450](https://github.com/ggml-org/whisper.cpp/issues/3450)
- **Pipecat Smart Turn v3.2.** BSD-2-Clause. 23 languages including ja, zh and ko. CPU int8 model is 8 MB; GPU fp32 is 32 MB. Runs in as little as about 10 ms on some CPUs and under 100 ms on most cloud instances. Takes 16 kHz mono PCM, is designed to run after Silero detects silence, and works without Pipecat (`model.py`/`inference.py`). — [pipecat-ai/smart-turn](https://github.com/pipecat-ai/smart-turn)
  - v3 is built on Whisper-Tiny with a classifier head, about 8M parameters, 12 ms CPU inference. — [Daily v3 announcement](https://www.daily.co/blog/announcing-smart-turn-v3-with-cpu-inference-in-just-12ms/); [Daily v3.1](https://www.daily.co/blog/improved-accuracy-in-smart-turn-v3-1/)
- **LiveKit turn detector.** A newer **audio** turn detector ("encodes user audio directly", in v1 and CPU v1-mini) covers 14 languages including Japanese. The older text-based detector (Qwen2.5-0.5B, 396 MB, 50–160 ms on CPU, 99.3–99.4% true-positive rate) is **deprecated**. LiveKit Model License. — [LiveKit docs](https://docs.livekit.io/agents/build/turns/turn-detector/)
- A March 2026 arXiv hierarchical end-of-turn model reports 69.3% F1 vs 63.3% for Smart Turn, at lower latency and parameter count (research-only result). — [arXiv 2603.13379](https://arxiv.org/pdf/2603.13379)

### Inferences
- **Revia's hands-free VAD** (energy threshold 900, 350 ms silence) plus fixed silence timeouts could be replaced by Silero v6 and then Smart Turn v3.2 on CPU. This should reduce both cut-offs mid-thought and sluggish replies. It is BSD-licensed, tiny and not tied to any framework, so it fits a C++ host through ONNX Runtime.
- **Multilingual STT.** For Japanese or Chinese (common for VTubers), Qwen3-ASR-0.6B (Apache) is the strongest open choice, but its streaming is vLLM-only (Linux-oriented). On Windows, the Whisper large-v3-turbo GGML model in the existing whisper.cpp server is the lowest-effort multilingual upgrade. For European languages, Parakeet-TDT-0.6B-v3 through sherpa-onnx is much faster.
- **Lip-sync timestamps.** Qwen3-ForcedAligner can double as a local tool for word and character timestamps on TTS output (see section 6).

### Gaps
- No Windows-specific latency benchmarks were found for Parakeet, Nemotron or Qwen3-ASR on RTX 50-series.
- Nemotron-3.5 per-language WER beyond es/it/en/ja, and its VRAM, are not documented.
- Distil-whisper and large-v3-turbo WER for the current leaderboard version were not retrieved from the leaderboard itself.
- The Kyutai STT license was not shown on the fetched page (the Kyutai TTS weights are CC-BY 4.0).

---

## 4. Full-duplex / speech-to-speech models: local usability, custom voice, barge-in, backchanneling

### Takeaway
True full-duplex open models exist (Moshi, NVIDIA PersonaPlex, MiniCPM-o 4.5, Qwen3-Omni), but none of them yet fits Revia's needs: a character with its own persona and custom cloned voice, a strong LLM brain, and a 12 GB + 8 GB split. The reasons:
- Moshi needs about 24 GB in bf16 and has only two stock voices.
- PersonaPlex (Moshi-based 7B) needs about 14 GB in bf16. The repo documents only 18 preset voice embeddings, although the paper describes audio voice prompts.
- MiniCPM-o 4.5 (9B, Apache-2.0) does clone voices through a reference-audio system prompt and fits in about 11 GB at int4. However, it speaks only English and Chinese, and its first token takes about 0.6 s.
- Qwen3.5-Omni's best variants are API-only.

The pragmatic 2026 pattern is a **cascaded** pipeline (STT → local LLM → streaming TTS) with semantic end-of-turn detection and barge-in, as in Kyutai Unmute. Full-duplex models can be tracked as an experimental side mode.

### Cited Findings
- **Kyutai Moshi.** Code is MIT/Apache; weights are CC-BY 4.0. Theoretical latency 160 ms, practical about 200 ms on an L4. bf16 needs a 24 GB GPU; int8 is experimental; MLX offers int4/int8. Voices: Moshiko and Moshika. Fine-tuning via `moshi-finetune`. "We do not provide official support" for Windows. — [kyutai-labs/moshi](https://github.com/kyutai-labs/moshi)
- **NVIDIA PersonaPlex (arXiv 2602.06053, early 2026).**
  - Moshi-based full-duplex model with a "Hybrid System Prompt": a text role plus an audio voice example. — [arXiv 2602.06053](https://arxiv.org/html/2602.06053v1); [NVIDIA research page](https://research.nvidia.com/labs/adlr/personaplex)
  - The repo ships 18 prebuilt voice embeddings (NAT/VAR) and does not document cloning from arbitrary audio. It is evaluated on FullDuplexBench for user interruption and backchanneling. Code is MIT; weights use the **NVIDIA Open Model License**. Install docs cover Linux, and a `--cpu-offload` flag exists. — [NVIDIA/personaplex](https://github.com/NVIDIA/personaplex)
  - About 14 GB of VRAM in bf16, which is more than a 12 GB card. — [Menon Lab guide](https://themenonlab.blog/blog/nvidia-personaplex-full-duplex-voice-ai-how-to-guide)
  - A community 4-bit bitsandbytes quant exists. — [HF personaplex-7b-v1-bnb-4bit](https://huggingface.co/brianmatzelle/personaplex-7b-v1-bnb-4bit)
- **MiniCPM-o 4.5 (2026-02-06).** 9B (SigLip2 + Whisper-medium + CosyVoice2 + Qwen3-8B). Apache-2.0 per the HF card. Full-duplex over audio and video streams. Clones voices from reference audio in the system prompt. Speech output in **English and Chinese only**. First token in 0.6 s. VRAM: bf16 19 GB, int4 11 GB. GGUF and llama.cpp-omni builds exist. — [HF openbmb/MiniCPM-o-4_5](https://huggingface.co/openbmb/MiniCPM-o-4_5)
  - llama.cpp-omni full-duplex mode reportedly runs on NVIDIA GPUs with 12 GB or more; the PyTorch demo needs about 28 GB (secondary). — [LearnOpenCV](https://learnopencv.com/minicpm-o-4-5-a-9b-model-that-can-see-hear-and-speak-at-the-same-time/)
- **Qwen3-Omni-30B-A3B.** Mixture-of-experts Thinker–Talker design. Latency about 211 ms for audio only and about 507 ms for audio plus video. Speech generation in 10 languages, understanding in 19. Persona via system prompt, and instruction-driven control of timbre, emotion and prosody. — [QwenLM/Qwen3-Omni](https://github.com/QwenLM/Qwen3-Omni)
- **Qwen3.5-Omni (2026-03-30).** Native turn-taking intent that tells backchannels ("uh-huh") apart from real interruptions. The Plus and Flash variants are **API-only**; only "Light" has open weights. — [MarkTechPost](https://www.marktechpost.com/2026/03/30/alibaba-qwen-team-releases-qwen3-5-omni-a-native-multimodal-model-for-text-audio-video-and-realtime-interaction/); [WaveSpeed](https://wavespeed.ai/blog/posts/what-is-qwen3-5-omni/)
- **Kyutai Unmute.** An open-source cascaded STT → any text LLM → TTS stack, with semantic VAD for turn-taking. Serves 32 users at 350 ms on one L40S. — [kyutai-labs/unmute](https://github.com/kyutai-labs/unmute); [Kyutai X post](https://x.com/kyutai_labs/status/1940767331921416302?lang=en)
- **Freeze-Omni.** A frozen LLM with chunk-wise streaming speech input and a classification head that manages turn-taking. Evaluated on Full-Duplex-Bench alongside Moshi. — [Full-Duplex-Bench v1.5 paper](https://arxiv.org/pdf/2507.23159)
- **Benchmarks.** Full-Duplex-Bench v1–v3 measure turn-taking, overlap and interruption. v3 (2026) adds tool use under real disfluencies: fillers, pauses, hesitations, false starts and self-corrections. — [Full-Duplex-Bench GitHub](https://github.com/eesi-ai/Full-Duplex-Bench); [FDB-v3 arXiv](https://arxiv.org/html/2604.04847v1); [Awesome-Full-Duplex-SDM list](https://github.com/Ruiqi-Yan/Awesome-Full-Duplex-SDM)
- **OpenAI Realtime** (gpt-realtime-2.1 / 2.1-mini) is cloud speech-to-speech with preset voices; see section 2. — [OpenAI pricing](https://developers.openai.com/api/docs/pricing); [custom voices](https://developers.openai.com/api/docs/guides/custom-voices)
- **Sesame.** Only CSM-1B (TTS) is open. The full conversational demo (Maya/Miles) is not released as weights; see section 1. — [HF sesame/csm-1b](https://huggingface.co/sesame/csm-1b)

### Inferences
- **Fit for Revia's character-with-custom-voice goal:**
  - MiniCPM-o 4.5 int4 (~11 GB, Apache-2.0, clones voices) is the only open full-duplex model that plausibly runs on the RTX 5070. But it would *replace* Revia's LLM brain and persona stack, and its speech is English and Chinese only.
  - PersonaPlex has a better duplex design (backchannels, interruptions) but needs about 14 GB in bf16 and has a restrictive preset-voice path.
  - Moshi's 24 GB is out of reach.
- **Recommended path.** Keep the cascade and add:
  - (1) streaming TTS (section 1);
  - (2) Silero v6 plus Smart Turn for semantic end-of-turn (section 3);
  - (3) always-on listening during TTS with echo cancellation, for true barge-in (Revia's barge-in is currently only "Tested");
  - (4) optional short backchannel clips ("mm-hm", laugh) pre-rendered in Revia's cloned voice and triggered by the turn detector while the user is speaking. This mirrors the backchannel vs interruption distinction that Qwen3.5-Omni builds in.
- **Echo cancellation.** Full-duplex on a stream or desktop needs acoustic echo cancellation or loopback subtraction so Revia does not hear herself. None of the model repos provide this for Windows (general engineering knowledge; not sourced here).

### Gaps
- GLM-4-Voice, Hertz-dev, Step-Audio 2 / 2.5 (open-weight status), LLaMA-Omni2 and SALMONN-omni were not researched in depth. Their 2026 licenses and VRAM are unverified.
- Not verified: whether PersonaPlex accepts arbitrary voice-prompt audio in the released code, and the exact commercial terms of the NVIDIA Open Model License.
- The Qwen3-Omni license, and whether it can clone arbitrary voices, were not verified from the model card.
- No measured Windows results for any full-duplex model.

---

## 5. Singing: SVC, SVS, song generation, vocal separation, pipelines, copyright

### Takeaway
AI VTuber covers are made **offline**:
1. Separate the song into vocals and instrumental with RoFormer/UVR.
2. Convert the vocals to the character's voice with RVC v2 (MIT, trainable on about 10 min of speech), or zero-shot with Seed-VC (GPL-3.0).
3. Remix.

Revia's existing karaoke player (instrumental.wav + vocal.wav) already fits this pipeline; the missing piece is an optional conversion step. For original songs, **ACE-Step 1.5** (MIT, runs on 6–8 GB) is the permissive local choice. Suno is now a licensed platform where downloads are paid, and Udio became a walled garden with no downloads.

**Copyright is the main risk.** Twitch allows covers only when the streamer performs all elements live, and bans karaoke over original backing tracks unless licensed. YouTube lets labels request removal of AI tracks that mimic an artist's voice. A monetized AI cover over a ripped instrumental is not safe.

### Cited Findings
- **RVC (Retrieval-based-Voice-Conversion-WebUI).** MIT, about 38.6k stars, actively maintained. Real-time voice changing at 170 ms end-to-end, or 90 ms with ASIO. About 10 minutes of low-noise audio or less is enough to train a model. Uses RMVPE pitch extraction. Works with NVIDIA CUDA; DirectML covers AMD and Intel on Windows. Integrates pymss/MSST separation models. — [RVC-Project GitHub](https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI)
- **How AI-VTuber-style covers are made.**
  - A developer of a Neuro-sama-style hobby AI VTuber describes this workflow: use RVC WebUI's built-in vocal splitting, convert the vocal with a model trained on *speech*, then overlay it on the instrumental in Audacity. — [kimjammer Neuro dev log 8](https://blog.kimjammer.com/neuro-dev-log-8/)
  - Neuro-sama (Vedal) does singing and karaoke on stream. — [Wikipedia: Neuro-sama](https://en.wikipedia.org/wiki/Neuro-sama)
  - Fan-made RVC models of VTuber voices circulate publicly (an unauthorized-likeness concern). — [HF VTuber-RVC](https://huggingface.co/dacoolkid44/VTuber-RVC/blob/main/Neuro-sama/Neuro_e240_s24480.pth)
- **Seed-VC.** **GPL-3.0.** The singing model is 44.1 kHz, F0-conditioned, about 200M parameters (Whisper-small encoder plus BigVGAN). Real-time mode reaches about 430 ms total on an RTX 3060 Laptop (150 ms inference per chunk). Fine-tuning needs as little as one utterance, with 100 steps taking about 2 minutes on a T4. Windows is supported with Python 3.10. — [Plachtaa/seed-vc](https://github.com/Plachtaa/seed-vc)
  - Mic-to-speaker latency is about 690 ms at block_time 0.3. — [seed-vc-realtime fork](https://github.com/jiaheguo521/seed-vc-realtime)
  - In the paper, zero-shot Seed-VC beats speaker-specific RVCv2 models on speaker similarity and CER, but its DNSMOS audio quality is slightly lower. — [arXiv 2411.09943](https://arxiv.org/pdf/2411.09943)
  - The repo's version dates as summarized (v2.0 "April 2024") look inconsistent; treat them as unverified.
- **Singing synthesis from notes and lyrics.**
  - OpenUtau supports DiffSinger (OpenVPI), a machine-learning singing synthesizer. Anyone can record, label and train their own DiffSinger voicebank. — [OpenUtau DiffSinger wiki](https://github.com/stakira/OpenUtau/wiki/DiffSinger-support); [Voicebank development](https://github.com/stakira/OpenUtau/wiki/Voicebank-development)
  - LUNAI offers "completely ethical" DiffSinger voices. — [LUNAI Project](https://lunaiproject.github.io/)
- **Song generation (local).**
  - **ACE-Step 1.5** is MIT-licensed. The 2B turbo runs on 6–8 GB; XL (2026-04-02) needs 12 GB or more with offload. A full song takes under 10 s on an RTX 3090. Lyrics in 50+ languages. Features cover generation, repainting, vocal-to-BGM and LoRA training (8 songs, 1 hour on a 3090). A portable Windows package is available. — [ace-step/ACE-Step-1.5](https://github.com/ace-step/ACE-Step-1.5)
  - The project page says the output can be used commercially and that it was trained on licensed, royalty-free and no-copyright data. That is the project's own claim. — [ACE-Step 1.5 page](https://ace-step.github.io/ace-step-v1.5.github.io/); [arXiv 2602.00744](https://arxiv.org/abs/2602.00744)
  - **DiffRhythm** is Apache-2.0, a latent-diffusion model for full songs up to 285 s; **DiffRhythm 2** also exists. — [ASLP-lab/DiffRhythm](https://github.com/ASLP-lab/DiffRhythm); [DiffRhythm2](https://github.com/ASLP-lab/DiffRhythm2)
  - **YuE** is Apache-2.0 and makes songs of about 5 minutes. — [Shinkai blog](https://blog.shinkai.com/yue-emerges-as-open-source-ai-song-generator-offering-legal-alternative-amidst-industry-turmoil/)
  - **SongBloom** has a lower lyric error rate than DiffRhythm and YuE and is faster than YuE. — [SongBloom arXiv 2506.07634](https://arxiv.org/html/2506.07634)
- **Suno and Udio after the label deals.**
  - Warner settled with Suno and Udio in late 2025 and signed licensing deals that launch in 2026. UMG settled with Udio. — [Billboard](https://www.billboard.com/pro/what-suno-udio-licensing-deals-mean-future-ai-music/); [MBW](https://www.musicbusinessworldwide.com/warner-music-group-settles-with-suno-strikes-first-of-its-kind-deal-with-ai-song-generator/)
  - Suno keeps prompt-to-song but trains only on licensed works, and downloads cost money in 2026. — [Digital Music News](https://www.digitalmusicnews.com/2025/12/22/suno-warner-music-deal-changes/)
  - Udio became a "walled garden" with no audio, video or stem downloads. Paid Suno plans grant commercial rights. — [Dubspot 2026](https://blog.dubspot.com/ai-music-licensing-explained-2026)
- **Vocal separation.**
  - BS-RoFormer won SDX23 (MUSDB18HQ SDR 9.80 dB). Mel-Band RoFormer adds 0.43–0.58 dB. RoFormers give the best quality but are the heaviest, slowest and most VRAM-hungry. Older UVR5/MDX-Net models are less competitive. — [Tomoda RoFormer guide](https://tomodahinata.com/en/blog/bs-roformer-mel-band-roformer-vocal-separation-guide); [Mel-RoFormer arXiv 2310.01809](https://arxiv.org/pdf/2310.01809)
  - Training and evaluation framework: [MSST arXiv 2607.23395](https://arxiv.org/pdf/2607.23395). Hosted models: [MVSEP BS-RoFormer](https://mvsep.com/algorithms/34)
- **Twitch.** Covers are allowed as a live performance if you "create all audio elements yourself", without "instrumental tracks or music recordings from the original musician's piece". Karaoke is not allowed unless you own or have licensed the music, or it is part of a game such as SingStar. Saving the performance to VODs needs extra licensing. — [Twitch Music Guidelines](https://legal.twitch.com/en/legal/music/) (as summarized by search); [Lickd summary](https://lickd.co/twitch-music-rules/)
- **YouTube.**
  - Labels in YouTube's AI music programme can request removal of tracks that mimic an artist's singing voice. Synthetic-likeness removal requests have covered voice since Nov 2023. Likeness-detection tooling exists, and AI disclosure is mandatory and more strictly enforced in 2026 (secondary summary). — [AIR Media-Tech](https://air.io/en/youtube-hacks/youtube-ai-policy-2026-likeness-detection-and-the-no-fakes-act-what-creators-need-to-know)
  - Content ID is fingerprint-based, not an AI detector. — [LastPlay Distro](https://lastplaydistro.com/blog/youtube-content-id-ai-generated-music-policy-2026-what-creators-must-know)

### Inferences
- **An offline cover pipeline fits Revia's existing SongLibrary with little work:**
  1. Mel-RoFormer/BS-RoFormer separation (MSST or UVR5 GUI).
  2. RVC v2 conversion with a model trained on about 10 min of Revia's Qwen3-TTS speech. This speech-trained model is exactly the "speech model on sung vocals" method that hobby AI VTubers use.
  3. Write `vocal.wav` and `instrumental.wav` into `RuntimeData/Songs/<name>/`.

  Seed-VC gives zero-shot similarity without training. Its GPL-3.0 license means it should stay an external tool Revia calls, not code bundled into Revia's distribution.
- **Real-time singing is possible but not needed.** RVC runs live at about 170 ms and Seed-VC at 430–690 ms, but covers are pre-rendered anyway. Live conversion only matters for a "sing along with the user" feature.
- **Monetized-stream-safe music options:**
  - (a) Original songs from ACE-Step 1.5 (MIT; the project claims licensed training data), with Revia's voice applied through RVC.
  - (b) Suno paid-plan tracks (commercial rights granted, now label-licensed).
  - (c) Licensed or royalty-free instrumentals.

  Covers over ripped original instrumentals break Twitch's "all audio elements yourself" rule and are high-risk for DMCA. It is untested whether an AI-rendered vocal even counts as a "live performance" by the streamer.
- **Legal risk of voice likeness.** Never convert to a real artist's or real VTuber's voice (YouTube removal requests, NO FAKES-style laws). Converting to Revia's own voice, which the owner designed with Qwen3-TTS VoiceDesign, avoids likeness claims but not composition or master-recording copyright.

### Gaps
- Not researched in depth: Synthesizer V Studio 2 and ACE Studio (AI voice DBs, pricing, terms for streaming), DDSP-SVC, and so-vits-svc's 2026 maintenance status.
- The SongBloom license, VRAM and weights availability were not verified (the GitHub fetch returned 404).
- No primary-source text was retrieved from Twitch's page (the fetch rendered no body). The rules are cited through search summaries and Lickd.
- No legal source addresses AI-performed covers on Twitch specifically, and none clarifies whether mechanical or performance licensing (PRO blanket licenses) covers an AI cover in a live stream.

---

## 6. Non-speech vocalizations, expressiveness and lip-sync data (phonemes/visemes/timestamps)

### Takeaway
Several open models can produce laughs, sighs and breaths from inline tags:
- **Chatterbox-Turbo** (MIT): `[laugh]`, `[cough]`, `[chuckle]`, `[sigh]`, `[whisper]`.
- **Step-Audio-EditX** (Apache-2.0): breathing, laughter, sighing and more, applied as an editor.
- **Fish S2 Pro**: 15k+ free-form tags, but non-commercial.
- **Higgs v3**: 21 emotions plus singing/shouting/whispering styles and SFX; creator grant only.
- **Orpheus** and **Dia2**: emotive and nonverbal tags, but Dia2's are unpredictable.

Qwen3-TTS offers instruction control only on its non-clone variants. For lip sync, **Azure** is the only engine with native viseme events and blend shapes. **ElevenLabs** returns character-level timestamps. Local engines generally provide none. The best local answer is to run **Qwen3-ForcedAligner** on the generated audio, which gives word or character timestamps with about 32 ms error in 11 languages. Map those to visemes, or keep amplitude-based mouth movement as a fallback.

### Cited Findings
- **Chatterbox-Turbo.** Native `[cough]`, `[laugh]`, `[chuckle]`; exaggeration and CFG controls; MIT. — [HF chatterbox-turbo](https://huggingface.co/ResembleAI/chatterbox-turbo). Also `[sigh]` and `[whisper]`. — [Resemble](https://www.resemble.ai/learn/models/chatterbox-turbo)
- **Step-Audio-EditX.** Paralinguistic markers for breathing, laughter, sighing, throat clearing, coughing, hesitation, surprise, dissatisfaction and questioning. Emotion and style editing. Apache-2.0. — [HF Step-Audio-EditX](https://huggingface.co/stepfun-ai/Step-Audio-EditX)
- **Fish Audio S2 Pro.** Free-form `[tag]` control (`[laughing]`, `[whisper]`, …), 15,000+ tags. — [HF s2-pro](https://huggingface.co/fishaudio/s2-pro)
- **Higgs Audio v3.** 21 emotion tokens, singing/shouting/whispering style tags, 9 sound effects, pauses. Scores a 68.57% win rate on paralinguistics in EmergentTTS. — [HF higgs-audio-v3-tts-4b](https://huggingface.co/bosonai/higgs-audio-v3-tts-4b)
- **Orpheus.** Inline emotive tags such as laughter and sighs. — [Orpheus-TTS](https://github.com/canopyai/Orpheus-TTS)
- **Dia2.** Laughter, coughing and sighing in dialogue. — [nari-labs/dia2](https://github.com/nari-labs/dia2). "nonverbal tags unpredictable". — [BentoML](https://www.bentoml.com/blog/exploring-the-world-of-open-source-text-to-speech-models)
- **ChatTTS.** Token-level laughter and pauses, with stability issues. — [BentoML](https://www.bentoml.com/blog/exploring-the-world-of-open-source-text-to-speech-models)
- **Qwen3-TTS.** Natural-language control of tone, emotion and prosody in the CustomVoice and VoiceDesign variants. — [Qwen3-TTS GitHub](https://github.com/QwenLM/Qwen3-TTS)
- **Cartesia Sonic 3+.** Laughter and emotion, and it interprets non-verbal expressions from the transcript. — [CallSphere](https://callsphere.ai/blog/vw1a-cartesia-sonic-3-april-2026-laughter-emotion-tts)
- **ElevenLabs v4.** More expression control, marketed as able to sound "dramatic, tender, urgent, comedic". — [TechCrunch](https://techcrunch.com/2026/09/28/elevenlabs-new-v4-speech-model-supports-more-expression-control-and-90-languages/)
- **Azure visemes.** 22 viseme IDs with audio offsets, plus SVG or blend-shape frames for 3D faces, through the SDK `VisemeReceived` event and the SSML `mstts:viseme` element. Not available over REST. — [Microsoft Learn](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/how-to-speech-synthesis-viseme)
- **ElevenLabs.** Character-level start and end times for raw and normalized text. — [ElevenLabs docs](https://elevenlabs.io/docs/api-reference/text-to-speech/convert-with-timestamps)
- **Qwen3-ForcedAligner-0.6B.** Word or character timestamps for zh, en, yue, fr, de, it, ja, ko, pt, ru and es; up to 5 min; 32.4 ms average alignment error on human-labeled data; Apache-2.0. — [QwenLM/Qwen3-ASR](https://github.com/QwenLM/Qwen3-ASR)
- **Kokoro.** No timestamp support documented. — [HF Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M)

### Inferences
- **Lip sync for Revia** (currently `lipSync: "audio_amplitude"`, visemes "future"):
  - *Cheap:* keep amplitude-driven mouth movement and add smoothing. VTube Studio and Live2D renderers typically do this.
  - *Better, local:* after each sentence renders, run Qwen3-ForcedAligner (0.6B) or Whisper word timestamps on the audio. Map words to phonemes with a G2P library (misaki, the one Kokoro uses, works for English and Japanese; this is an assumption), then to about 15–22 visemes using the Azure/Oculus viseme set. Emit these as `PresentationEvent`s aligned to the playback clock. The TTS is sentence-granular, so alignment adds latency only once per sentence.
  - *Cloud:* in hybrid mode, Azure gives visemes directly and ElevenLabs gives character timings.
- **Expressiveness mapping.** Revia's emotion vector (happy, sad, playful, …) could drive:
  - (a) reference-clip switching for Qwen3-TTS Base;
  - (b) inline tags when a tag-capable engine (Chatterbox-Turbo) is selected;
  - (c) a short library of pre-rendered nonverbals (laugh, sigh, "hmm") in Revia's cloned voice, generated once offline with Step-Audio-EditX or Chatterbox and played by the reflex layer. The existing cached Reflex phrases already play in under 0.1 s per the current-state notes.

### Gaps
- Not verified: Cartesia word or phoneme timestamps, ElevenLabs WebSocket alignment, Rhubarb Lip Sync's current status, and VTube Studio's audio-lipsync API.
- No open TTS was found that emits phoneme or viseme timings natively alongside streamed audio. Kyutai's delayed-streams models may give word timing implicitly, but this is not documented in the fetched pages.
- No benchmarks were found comparing nonverbal naturalness across open models, apart from Higgs' self-reported EmergentTTS paralinguistics win rate.
