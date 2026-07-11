# CrispASR v0.8.10

205 commits since v0.8.9. Headline: the #218 long-audio arc root-caused and
fixed (qwen3-asr / glm-asr now match their Python blueprints; loop mitigation
across all LLM backends), four new backends/models (canary-qwen, OmniVoice,
Voxtral-TTS, kyutai-stt-2.6b), raw CTC logits access across every language
binding, per-token streaming in the C ABI, a GPU graph-cache use-after-free
fix across 7 backends, a class of multi-model-in-one-process correctness bugs
fixed (a second model no longer reads the first's cached weights/vocab), and
OmniVoice's silent-synthesis root cause fixed plus a TTS perf pass
(OmniVoice ~1.6-2×, qwen3-tts codec 3× on Metal).

## New backends / models

- **canary-qwen** (#233) — `nvidia/canary-qwen-2.5b` SALM: FastConformer
  encoder → Qwen3-1.7B decoder with merged LoRA. English ASR. GGUFs at
  `cstr/canary-qwen-2.5b-GGUF` (F16/Q8_0/Q4_K; Q8_0 registry default).
- **OmniVoice TTS** (#234) — `k2-fsa/OmniVoice`: Qwen3-0.6B + SoundStorm-style
  masked-iterative 8-codebook generation with classifier-free guidance,
  HiggsAudioV2 codec decoder → 24 kHz PCM. Voice cloning, 600+ languages.
  GGUFs at `cstr/omnivoice-GGUF`.
- **Voxtral-TTS** (#93) — Mistral Voxtral-based TTS end to end in ggml: LLM AR
  backbone → flow-matching ODE → codec decoder, proper Tekken
  pre-tokenization; multi-voice, EN+FR validated by ASR roundtrip.
- **kyutai-stt 2.6B** (#238) — `kyutai/stt-2.6b-en` support in the kyutai-stt
  backend: model-scaled silence prefix/tail conditioning (2.6B has 3.5 s
  lookahead), registry auto-download, diff-harness reference backend.
- **cohere-transcribe-arabic** (#231) — `--backend cohere-ar` shorthand wired
  into the factory (defaults `-l ar`), registry auto-download, GGUFs at
  `cstr/cohere-transcribe-arabic-07-2026-GGUF`.

## #218 — long-audio repetition loops / empty output: mitigated AND root-caused

- **Mitigation everywhere**: n-gram `fix_loops` applied to all LLM-decoder
  backends (incl. granite), mirrored into the session C-ABI; word-level dedup
  alongside the text collapse; long-run + phrase-repeat regression fixtures.
- **qwen3-asr root cause**: the q4_k GGUFs quantized the audio tower below
  8-bit; compounding encoder drift flipped greedy decode into loops /
  "language none" on long clips. `crispasr-quantize` now floors qwen3-asr
  `audio.*` at Q8_0 (re-baked GGUFs live for 0.6b, 1.7b, ja-anime). Runtime
  half: forced language is now an assistant-turn prefill
  (`language <Name><asr_text>`, the blueprint contract — structurally prevents
  empty output), tail-pad frames dropped before the encoder, KV grows on
  demand (a fixed 4096 capped un-chunked audio at ~5 min), `max_new` fallback
  512. Un-chunked 145 s decodes clean with the loop-fix disabled.
- **glm-asr root cause**: no quantization involved — the GGUF carried no BPE
  merges, so every plain-text instruction silently tokenized to nothing (the
  model never saw the mandatory "Please transcribe this audio into text"), and
  audio was truncated to one 30 s window. Now: real byte-level BPE (merges
  baked by the converter; `tools/gguf-add-merges.py` backfills — published
  GGUFs updated) and the blueprint multi-window prompt (up to 21×30 s = 655 s
  single-pass). q4_k matches the bf16 reference near-verbatim on the 145 s
  repro clip.
- **mega-asr**: verified against its blueprint — the long-form degeneration
  reproduces in the bf16 LoRA-merged reference itself (base model clean), so
  it is model-inherent. `--chunk-seconds 0` is unsupported for mega; the
  default 30 s-chunked path works as before.
- **Experimental**: `CRISP_AUDIO_WINDOWED_ATTN=1` — FA2-style block-diagonal
  windowed encoder attention for qwen3-asr, O(N·W) memory instead of O(N²).
  Opt-in escape hatch for >10-min single-pass audio (keep the loop fix on);
  evaluated and deliberately not the default.
- **Diagnostics**: `CRISPASR_NGRAM_LOOPFIX_OFF=1` raw-decode gate, qwen3
  stages in `crispasr-diff`, encoder-cosine quant audit tool
  (`qwen3-asr-enc-dump`), and a stderr warning when non-silent audio produces
  an empty transcript (#240).

## Bindings / API

- **Raw CTC logits accessor** across Python, Go, Ruby, Java, C#, Dart, and
  JavaScript bindings, plus a CTC vocabulary accessor for token→word
  detokenization — enables forced alignment on wav2vec2 / canary-ctc and the
  rest of the CTC set (community contribution by Michael J. Culbertson, #232).
- **Per-token streaming callback** in the session C-ABI.
- Dart: replace deprecated `Pointer.elementAt`; Go: cgo LDFLAGS sync.

## Fixes

- **OmniVoice silent synthesis (#234)**: the C++ generation-config fallbacks
  didn't match the blueprint's `OmniVoiceGenerationConfig` (guidance scale,
  class/position temperature, layer-penalty and t-shift unmask schedule), so
  the masked-iterative decode degenerated into near-constant silence codes on
  every platform and precision — the codec then faithfully rendered silence.
  With blueprint defaults it produces real audio (whisper roundtrip verbatim).
- **Multi-model-in-one-process correctness (PR #244 + follow-ups)**: several
  backends cached per-model data in process-global, pointer-keyed maps that
  outlived the model. After freeing model A and loading model B, the allocator
  commonly hands back the same addresses, so B read A's data. Fixed by scoping
  each cache to its model: the wav2vec2 GPU dequant cache and pocket_tts F16
  cache (community contribution by Michael J. Culbertson), the greedy-tokenizer
  vocab maps in kugelaudio/vibevoice, and the voxcpm2 VAE-encode memo.
- **Per-session streaming buffers**: the default segment/token callbacks (Dart
  FFI polling path) pushed into process-global buffers, so two sessions —
  concurrent or sequential — interleaved and drained each other's output.
  Scoped to the owning session (and capped so a non-draining consumer can't
  grow them without bound). The persistent CPU threadpool is now released when
  its backend is freed, instead of leaking worker threads per model.
- **canary-qwen instruction echo (#247)**: on a too-short audio window the SALM
  decoder echoed its task framing as a meta word ("Transcript", "Transcription",
  "PASS") instead of transcribing. Root-caused against the NeMo SALM reference:
  the FastConformer subsamples 8x, so a window with only a few encoder frames
  (T_enc<=5, ~<=0.4 s) gives the Qwen3 LLM decoder no acoustic content to ground
  on and it falls back to its language prior — NeMo emits the *identical* tokens
  ("Okay" on a 0.1 s clip, "Transcript" on a 0.3 s clip), so this is inherent to
  the model, not a port bug (the prompt is byte-identical to NeMo's, and full-
  utterance output matches exactly). Earlier theories (framing tokens; string
  stripping) did not fix it. Fixed in the pipeline: a degenerate-window gate
  returns empty for sub-gate windows, plus a backend-agnostic safety net that
  strips any leading instruction-echo token from **both** the text and the
  tokens array (the old workaround left the tokens array inconsistent — #218).
  Both paths are env-gated (`CRISPASR_CANARY_QWEN_MIN_ENC_FRAMES`,
  `CRISPASR_CANARY_QWEN_NO_ECHO_STRIP`) for A/B.
- **kugelaudio no-voice synthesis (#248)**: no voice packs were ever published
  upstream, so unconditioned synthesis produced noise. Implemented VibeVoice's
  zero-tensor neutral-speaker fallback (1-frame zero VAE latent through the
  acoustic connector) so it produces intelligible speech without a voice GGUF.
- **moonshine ran CPU-only from the CLI**: the adapter never forwarded the
  `--gpu` preference, so every moonshine run (including the §232 "GPU"
  benchmarks) used the CPU backend. Now forwards it — encoder is 4-6× faster on
  Metal, transcript identical, `--no-gpu` still opts out. (moonshine-streaming
  and piper stay on CPU deliberately: their tiny per-frame graphs are
  launch-bound and measurably slower on GPU.)
- **f5-tts was broken and CPU-only**: the DiT transformer (the model's dominant
  compute) was hardcoded to run on the CPU backend regardless of `--gpu`, and a
  gallocr input-aliasing bug corrupted the RoPE positions from the first ODE
  step on — so synthesis produced degenerate audio on both backends (the prior
  "f5 CPU is a dud" state). Both fixed: the DiT now runs on the selected backend
  and positions are re-set each step. On M1 Metal, synthesis roundtrips verbatim
  and is ~7.8× faster per step on GPU.
- **GPU graph-cache use-after-free (#235)**: glm-asr crashed when the encoder
  graph was cached across slices on GPU; the same stale encoder-graph cache
  was removed from 7 backends.
- **Opus (#239)**: native Opus enabled in all release builds
  (`CRISPASR_OPUS_FETCH=ON`); POSIX `popen` mode fix restores the ffmpeg opus
  fallback.
- **qwen3-asr-1.7b q4_k re-baked (#240)** — was exported before the Q8_0
  audio-tower floor and produced empty transcripts on Metal.
- canary-qwen: mel filterbank layout transpose (the cos −0.15 → 0.999 fix),
  audio token framing, pos_enc sizing.
- MSVC build break (`ca_token_record` definition order); missing
  `install(TARGETS)` DESTINATIONs; dead lid-silero registry entry removed
  (community PR #237).

## Performance

- **OmniVoice** (#245): the masked-iteration loop rebuilt and re-allocated the
  full 28-layer forward graph twice per step (cond + CFG uncond) × 64, though T
  is fixed and there's no KV state. Build each arm's graph once, refresh only
  inputs per step → ~1.6-2× synthesis (0.6B q8_0, 32 steps: 135.5 → 82.8 s CPU).
- **qwen3-tts** (§232): codec FASTCONV — K=1 convs became direct channel
  matmuls (were im2col copies with ~300 MB F32 intermediates), causal pad folded
  into im2col (no CPU-only PAD node forcing sched splits), and F16 kernels baked
  to F32 once at load. 3× codec decode on Metal, 2.1× on CPU. The code-predictor
  fast path (`CP_DIRECT`) is now default-on for GPU backends.
- **Voxtral-TTS** (#93): flow-matching CFG batches the conditional and
  unconditional passes into one cached graph, ~24% faster FM.
- **parakeet-tdt** (§232): batch-precompute the TDT decoder's encoder
  projections; RNNT decode GPU-profiled and re-prioritized.
- **moonshine**: in-graph argmax for greedy decode.
- **tinyBLAS** (`GGML_LLAMAFILE`) enabled by default for CPU GEMM.
- **irodori-tts** (#241): diffusion knobs exposed (ODE steps + CFG scale).

## Packaging / project

- `THIRD_PARTY_NOTICES.txt` attached to all release artifacts.
- Kaggle GPU benchmarks: 10-backend CrispASR vs onnx-asr fleet run (#81) and a
  CrispASR vs transcribe.cpp harness — results in `PERFORMANCE.md`.
- Issue triage: 28 stale-open issues closed against their landed fixes.
