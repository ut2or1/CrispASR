# CrispASR — Pending work

Pending roadmap items. Each is self-contained with files, approach, and
effort estimate. Completed items have been moved to `HISTORY.md`.

> **Numbering convention:** `§N` refers to PLAN items (sections in this
> file). `#N` refers to GitHub issues on CrispStrobe/CrispASR. They are
> independent sequences and numbers may collide. When in doubt, PLAN
> items are always written as `§N` and GitHub issues as `#N`.

**Latest release: v0.6.12** (commit `345ecfdc`). Full notes in [`RELEASE_NOTES_v0.6.12.md`](RELEASE_NOTES_v0.6.12.md).

**Recent completions (2026-07-11):**
- **#242 moss-diarize**: SHIPPED — joint ASR + diarization + timestamps, 0.9B model,
  diff harness 4/4 cos=1.0, GGUFs on HF, full 12-point checklist. See `HISTORY.md`.
- **#200 dots-tts PatchEncoder**: FIXED — added missing RoPE (theta=10K) + QK-norm;
  full pipeline now runs e2e (LLM → DiT → PEnc → vocoder → WAV), ASR roundtrip passes.
  Wired `--tts-steps` / `--tts-cfg-scale`. GPU already supported. See `HISTORY.md`.
- **#215e gallocr UAF audit**: DONE — all 19 `cached_*_gf` sites audited across
  the codebase. 7 backends fixed (canary, canary_ctc, kyutai_stt, moonshine_streaming,
  nemotron, paraformer, sensevoice). See `HISTORY.md`.
- **Generation-health gate**: DONE — `src/core/generation_health.h` with 5 checks
  + 16 unit tests. Non-breaking additive.
- **qwen3-tts-perf (#245)**: ANALYZED — profiled on CPU: dispatch overhead (build+
  reset+alloc = ~5ms) is <0.1% of per-frame cost (5000ms compute). The O15 path
  already caches the graph and uses a dedicated sched. Skip-realloc is broken on
  CUDA+Metal. The bottleneck is pure matmul compute; perf wins require GPU where
  the ~5ms overhead becomes significant. Handover removed.
- **Untrusted-input parser hardening**: SHIPPED — multi-agent security audit of the
  audio demuxers + GGUF loader found 6 DoS/OOB defects (MP4 stsz/stco/co64 count +
  co64 offset overflow, WebM lacing, WAV + AU size clamps, GGUF split mmap bounds),
  all fixed + ASan-validated. See `HISTORY.md` + `LEARNINGS.md`.

## Untrusted-input parser hardening — follow-ups (MOSTLY DONE)

The memory-safety fixes shipped (see HISTORY 2026-07-11). Test/tooling state:
- **DONE**: AU + WAV crafted-input regression tests (ASan/UBSan-clean); the
  `linux-asan-audio` CI job builds the decoders under ASan and decodes the
  samples on every change.
- **DONE**: MP4 crafted-input *reachability* test (malicious `stsz` count)
  through the full `crispasr_audio_load` dispatch → `crispasr_m4a_decode`.
- **DONE**: libFuzzer harness over `crispasr_audio_load` (`tests/fuzz/`, gated
  `-DCRISPASR_FUZZ=ON`, clang) — seed from `samples/`; mutation covers the
  AU/AMR/WebM/MP4/WAV dispatch under ASan, so WebM/AU/AMR get reachability
  coverage there rather than as separate hand-crafted tests.
- **DONE**: `crispasr_server` security audit (multi-agent) — 7 fixes: unbounded
  upload DoS (`set_payload_max_length` + chunked reject), `/v1/audio/speech`
  voice path-traversal, CAP_TTS gates on POST/DELETE `/v1/voices`, log-injection
  sanitisation. See HISTORY.
- **DONE**: GGUF loader + tokenizer audit (multi-agent) — GGUF mmap bounds
  checks made overflow-safe (subtractive, 3 sites in `gguf_loader.cpp`; a crafted
  GGUF declaring a `SIZE_MAX` tensor wrapped `data_off+off+nbytes` → SIGBUS on
  Metal), and sentencepiece input-length clamp (`tokenize`/`tokenize_bpe`).
- **DONE**: `linux-fuzz-smoke` CI job (audio fuzzer, seeded, ASan) + a second
  harness `tests/fuzz/fuzz_gguf_meta.cpp` (`crispasr-fuzz-gguf`) over the GGUF
  metadata path.
- **DONE (ggml fork `1dc4cb93`)**: the GGUF fuzzer found a real DoS — a malformed
  GGUF with an **empty KV key** hit `GGML_ASSERT(!key.empty())` in
  `ggml/src/gguf.cpp` → `ggml_abort` → `abort()`, so ANY untrusted model load
  (incl. server `POST /load`) could crash the process. Fixed in the
  `CrispStrobe/ggml` fork: `gguf_init_from_file`'s KV loop now rejects an empty
  key (log + `ok=false` → returns nullptr) the same way a duplicate key is
  rejected. Validated on the saved reproducer (`gguf-empty-key-abort.crash`):
  now returns nullptr, valid models still load. Submodule pointer bumped here.
  Then re-fuzzed `gguf_init_from_file` against the fixed ggml on BOTH paths —
  metadata (`no_alloc=true`, 384K runs, cov→7548) and tensor-data
  (`no_alloc=false`, 497K runs, cov→10051, `-malloc_limit_mb=2048` clean) —
  **~880K runs total, zero findings**. The parser is robust under fuzzing (no
  further asserts, no unbounded alloc from declared tensor sizes). **Open (your
  call):** upstream the empty-key fix to ggml-org — it's a general robustness
  bug worth contributing back (an outbound public PR, so left for a human).
- **DONE 2026-07-12**: the `bpe.h`/`wordpiece.h` fuzz harness —
  `tests/fuzz/fuzz_tokenizer.cpp` (`crispasr-fuzz-tokenizer`, gated
  `-DCRISPASR_FUZZ=ON`). Fuzzes `core_bpe::tokenize_simple` +
  `core_wordpiece::Tokenizer::tokenize` over arbitrary text (the untrusted
  prompt/`--ref-text`/caption surface; vocab pinned benign since GGUF vocab is
  covered by `fuzz_gguf_meta`). Validated locally: **138,705 runs / 16 s clean**
  under `-fsanitize=fuzzer,address,undefined` (adversarial UTF-8, lone 0xFF/
  continuation bytes, embedded NULs) — confirms the audit's "clean" verdict with
  a runnable harness.
- **DONE 2026-07-12**: the deterministic GGUF `load_weights` bounds regression
  test — `tests/test-gguf-bounds.cpp` (`test-gguf-bounds`, `[unit]`). Writes a
  valid 1-tensor GGUF, truncates it to 8 bytes short of the tensor data (metadata
  fully parses; the declared 256-byte tensor overruns the file), and asserts
  `load_weights(..., backend_cpu)` returns `false` — no SIGBUS. Verified it hits
  the exact hardened path (the run logs the subtractive `mmap legacy path: tensor
  exceeds file bounds` check), plus a positive control (intact file loads). A fast
  always-on CI guard for the `SIZE_MAX`/truncated-tensor regression, complementary
  to `fuzz_gguf_meta` (~880K runs clean).

## Scoped next items (for a new agent picking up)

### qwen3-tts code predictor fused graph (#245, GPU-ONLY) — DONE (CP_DIRECT), verified 2026-07-12

**Status:** DONE. This was superseded by **CP_DIRECT** (§232/#245,
`src/qwen3_tts.cpp` ~L1912) — the sched-free persistent code_pred dispatch that
Option C wanted but without the O15_SKIP_REALLOC breakage. It builds the two
per-frame code_pred graph shapes (T=2 prefill + T=1 step) once, gallocr-allocates
on the dedicated code_pred backend, and each of the 15 dispatches is just
blit-lm_head-slot + tensor_set + one `ggml_backend_graph_compute` (no
`sched_reset`/`alloc`). **Default ON when code_pred runs on a GPU backend**
(`QWEN3_TTS_CP_DIRECT`, else per-backend default); md5-identical WAV validated
2026-07-10.

**Verified on M1 Metal 2026-07-12** (`qwen3-tts-12hz-0.6b-base-q8_0.gguf`, quiet
box, `QWEN3_TTS_BENCH=1`): `cp_direct active`; per-frame code_pred bench
**set≈2-4 ms, compute≈45-60 ms, read≈0.1 ms** — dispatch is now ~5% (the pre-CP_DIRECT
sched path was ~25 ms × 15 ≈ 375 ms/frame of pure dispatch). ar_loop 76 ms/frame,
**RTF 1.2×**, ASR round-trip correct ("and so my fellow americans"). code_pred is now
**compute-bound** (15 sequential T=1 steps of a 5L/d=1024 transformer), so
**Option A (unrolled 75-block graph) would buy ~nothing** — it can't parallelize
the sequential steps and dispatch is already ~zero. The O15_SKIP_REALLOC path
(Option C) remains the broken predecessor; CP_DIRECT is the shipped replacement.

**Load-dependence (same as §176k):** the CP_DIRECT comment records "M1 Metal
~equal on an idle box, ~3× under load" — confirmed compute-bound at idle here.
Nothing further to do on Metal. **CUDA: RE-VALIDATED PASS 2026-07-12** (Kaggle
P100/T4, `chr1s4/crispasr-qwen3-cp-direct-cuda` reached `COMPLETE` = base/o15/
direct/direct_lk all rc=0, md5/PCM-equivalent, ASR round-trip intact) — on top of
the prior "11% faster on P100" datum. Re-run after any code_pred change:
`kaggle kernels push -p tools/kaggle/qwen3-tts-cp-direct-cuda` (chr1s4;
base/o15/direct/direct_lk matrix, md5 + ASR-roundtrip acceptance, `CRISPASR_REF=main`).

### Defaults-audit generalisation (VPS-doable) — LARGELY DONE (2026-07-12)

**What:** Extend the tada-params defaults-audit pattern (`tests/test-tada-params.cpp`)
across backends that have params structs with documented upstream defaults.

**How:** For each backend with a `*_context_default_params()` function, write a Catch2
test asserting key defaults match the upstream Python reference.

**Status (2026-07-12):** The `test-<backend>-params.cpp` files were already
*registered* for ~52 backends but were **hollow** — only 5 (tada, chatterbox,
orpheus, voxtral-tts, dots-tts) asserted actual value knobs; the other ~47 only
smoke-checked `n_threads>=1` / `verbosity>=0`, which structurally cannot catch a
#192-class silent default drift. Swept the meaningful backends and added
grounded value-knob assertions (now **37/52** assert value knobs), in three
commits:
- TTS priority set: vibevoice, kokoro, f5-tts, dots-tts (exact upstream).
- TTS/audio-gen: bark, cosyvoice3 (RAS), csm, dia, indextts, outetts, zonos,
  fastpitch, melotts, piper, speecht5, qwen3-tts, parler, pocket-tts.
- ASR: firered-asr (beam), glm-asr, kyutai-stt, funasr, gemma4-e2b, mimo-asr
  (greedy temp 0); flash/gpu guards (PLAN #89 class) for qwen3-asr, voxtral,
  voxtral4b, granite-speech, canary-qwen, nemotron, parakeet, canary.

**Deliberately left smoke-only:** granitenle, m2m100, sensevoice, t5translate,
canaryctc (pure infra — no perceptual/decode knob to pin); firered + the two
moonshine-stream tests (duplicate the firered-asr / moonshine_streaming structs
already covered); openvoice2 (single `tau` knob). Skipped as owned/blocked:
cohere, voxcpm2, voxcpm2-tts (concurrent work), kugelaudio (CFG is a TODO).

**Files:** One test file per backend in `tests/test-<backend>-params.cpp`, registered
in `tests/CMakeLists.txt` with label `[unit]`. Read upstream defaults from the
Python source or model card.

### Diff-harness extension: per-step talker logits (#1, GPU-preferred)

**What:** Dump the talker LLM logits at each generation step in both the Python
reference and C++ runtime, compare them. Validates the text-decoder input so the
sampler is a faithful port over verified logits.

**How:** (a) Add a `talker_logits_step_N` capture to the Python reference dumper
(e.g. `tools/reference_backends/qwen3_tts.py`) using a `generate`-time hook.
(b) Add the matching C++ stage to `crispasr_diff_main.cpp`. (c) Run on the TTS
diff harness.

**Test:** needs a TTS model (qwen3-tts or tada). The 0.6B Q8_0 (941 MB) fits on VPS
but TTS generation is slow on CPU (~105x RTF). A short "Hi." input with 2-3 frames
is feasible.

### Diff-harness extension: replay-token dual-mode (#3, VPS-doable)

**What:** Dump the Python's *sampled* token IDs and replay them in C++ (instead of
re-sampling) so sampling-enabled downstream stages can be diffed deterministically
despite torch-vs-mt19937 RNG mismatch.

**How:** (a) Python dumper captures `sampled_token_ids` as a 1D int32 tensor in the
reference GGUF. (b) C++ diff harness reads them and feeds them to the backend's
step function instead of sampling. (c) Compare downstream stages (codec, vocoder)
against the Python reference that used those same tokens.

**Files:** Extend `tools/reference_backends/<tts_backend>.py` + `crispasr_diff_main.cpp`.

### #227 — VAD info reuse (VPS-doable, feature request)

**What:** User wants to run ASR multiple times on the same audio with different
backends without re-computing VAD. Expose the VAD segment boundaries so they can
be reused.

**How:** The VAD already produces segment boundaries internally. Add a
`--vad-export FILE` flag that writes the boundaries as JSON, and a
`--vad-import FILE` flag that reads them back instead of running VAD. Pure CLI
feature, no model changes.

**Files:** `examples/cli/crispasr_run.cpp` (VAD integration), add export/import
around the `vad_segments` vector.

## Gemma-4 12B (gemma4_unified) ASR support (OPEN)

The remaining open item for full 12B support (a new converter map + backend audio path for the 640-dim unified
encoder) is a larger port, scoped but not started.

## crispasr-diff harness extensions — catch decode-policy / quality bugs (OPEN)

Motivated by #197: the stage-cosine diff validates the forward pass against a
Python reference *pinned to a non-default mode* (`text_do_sample=False`) for
determinism, so it structurally cannot see (a) a production default diverging
from upstream, (b) decode-policy bugs (greedy looping / cut-off words), or (c)
perceptual pathologies. The cheap config-parity guard is **DONE** (a
defaults-audit unit test, `tests/test-tada-params.cpp`, asserting the C++
defaults equal upstream `InferenceOptions`).

**Gap exposed by #192 (2026-07-01):** that guard checks the *library* default
(`tada_context_default_params`, which correctly had `num_acoustic_candidates=1`),
but the actual reported bug lived in the **CLI/c_api adapter** which overrode it
to 4 (and a parallel session to 8). The parity test never sees the adapter
override. The audit should also assert the CLI-adapter/c_api resolved defaults
equal upstream — not just the library struct. (The cand default is now back to 1;
`extra_steps` back to 0.) **CLOSED structurally (commit `3b52d268`):** the CLI
adapter and c_api no longer re-hardcode `num_acoustic_candidates` — they inherit
the library default (`tada_context_default_params` → 1), so the existing
defaults-audit test now guards all three paths. The only deliberate adapter
override left is `text_do_sample=true`.

**Full adapter-parity audit DONE (2026-07-12).** Swept all ~40 c_api adapter
blocks + the CLI adapters for hardcoded perceptual/decode-knob overrides that
diverge from — or wrongly ignore session config vs — the library default. CLI
adapters were clean; two real c_api bugs found + fixed: cosyvoice3 hardcoded
`temperature=0.8` (ignoring `crispasr_session_set_temperature`; the CLI + siblings
gate on the session temp) and f5-tts hardcoded `seed=42` (ignoring the session
seed). Both now honour session config, falling back to the working default.
outetts/dots/parler re-state their library temp default as the fallback literal —
redundant but correct + session-gated, left as-is.

Three larger extensions remain:

1. **Per-step talker logits in the diff.** Dump the talker logits at each
   generation step in both the Python reference and the C++ runtime and compare
   them. Validates the text-decoder *input* so the sampler is a faithful port
   over verified logits — today only the FM/codec stages are diffed.
2. **Generation-health regression gate (non-diff). DONE (2026-07-11).**
   `src/core/generation_health.h` — shared header with check_not_empty,
   check_duration_plausibility, check_no_ngram_loop, check_not_truncated,
   check_tts_duration. Catch2 unit tests in `tests/test-generation-health.cpp`.
   Backends can integrate these into live tests. **en/fr/de suite +
   trailing-silence check DONE (2026-07-12):** added `check_trailing_silence`
   (windowed-RMS backward scan — flags TTS dead-air / EOS-overrun / untrimmed
   padding) plus French/German plausible-duration, UTF-8 accented + German
   word-loop, and mixed-language no-false-positive cases (25 unit cases total).
   Remaining: wiring the checks into individual backends' *live* tests (needs
   models).
3. **Replay-token dual-mode reference.** Dump the Python *sampled* token ids and
   replay them in C++ (instead of re-sampling) so the sampling-enabled
   downstream stages can be diffed deterministically despite torch-vs-`mt19937`
   RNG mismatch — gives the sampling path a ground-truth diff, not just greedy.

Generalise the defaults-audit pattern across backends: a per-backend table of
(param → upstream default) checked against the params struct, so "a knob is
declared but dead / defaults diverge from upstream" fails CI everywhere.

> **Audit 2026-06-12** — code-verified all items against HISTORY.md and codebase.
> **Newly closed (stale in table until this audit):** #42 VibeVoice-ASR 7B
> (shipped with GGUFs + layer offload), #43 Fun-ASR-Nano (shipped 2026-05-20),
> #59 Cross-binding C-ABI parity (all 7 bindings 149/149 symbols, 2026-06-04),
> #155 CONV_TRANSPOSE_1D GPU (Vulkan+CUDA done, 2026-06-10),
> §WASM Browser build (all backends + HF Space, 2026-06-10).
> **Updated:** #52 O15 broken on CUDA; #56 diff-harness done, only JA kanji remains;
> #81 Nemotron un-deferred → HIGH for dictation.
> **Still open:** #52 perf pass, #51c F16 (RAM-blocked), #56 JA kanji
> (needs MeCab/KaKaSi), #58 MOSS (in progress), #75 server
> round 2, #66 wrapper publishing, O5/O6/O7 (O6 GPU-only, O7 needs draft
> models), #101 OmniVoice, #102 RapidTP.
> **Prior audit closed:** #96, #73 FA, #61j, #94, #93, #103, #100 A+B, O4, #115.

**Current state (May 2026, v0.6.11):** 20 ASR + 3 TTS + 1 speaker-verification backends (+ Chatterbox T3 in progress), unified CLI,
OpenAI-compatible server + WebSocket streaming, shared `src/core/` library, FireRedPunc
post-processor, C-ABI + Go/Java/Ruby/JS/Python/Dart bindings, CI on 6 platforms.
All backends support `-m auto --auto-download`. Three new ggml ops
(`conv_1d_cf`, `conv_1d_dw_cf`, `conv_1d_group`). ggml bumped to 0.10.0.
Feature matrix expanded to 21 backends (README). Speaker identification via
TitaNet embeddings + profile DB, integrated into diarize pipeline.
test-all-backends.py passes 18/18 transcribe + 51/54 feature tests (3 stream skips, no failures).

> **‼️ Tooling pin: `clang-format` MUST be v18.** CI pins it
> (`.github/workflows/lint.yml`). Homebrew's default `clang-format` and
> Xcode's bundled `clang-format` both ship v22, which silently
> re-wraps lines and breaks CI lint. Use `./tools/format.sh` (refuses
> non-v18) or the explicit path
> `/opt/homebrew/opt/llvm@18/bin/clang-format`. Never `clang-format`
> bare from `PATH`. See `CLAUDE.md` + `LEARNINGS.md` for the full
> lesson.

---

## #201 follow-up — generate a TADA voice ref from audio+transcript at query time (OPEN — offline path now DONE)

The switch-voice half of #201 shipped (commit `a5cd7510`): the server reloads a
prebaked `tada-ref-*.gguf` per request, no restart.

**Offline `--make-ref` is now DONE end-to-end (#192, 2026-07-01).** The pipeline
produces a working ref (the empty-synth bug was a missing `tokenizer.ggml.merges`
in the aligner GGUF → byte-level fallback; fixed in `convert-tada-aligner-to-gguf.py`).
`--make-ref` is reachable (input-file guard fixed), auto-downloads the
encoder/aligner from the model repo (`--auto-download`), and all 10 language
aligners (q8_0, 906 MB) + encoder are published on `cstr/tada-tts-{1b,3b-ml}-GGUF`.
`--align` (word-timestamp) mode was added on the same aligner path. `--voice <wav>`
at synth time now **fails loudly** and points at `--make-ref` (was silently using
the default voice).

**CLI query-time inline cloning now DONE (#192, commit `3b52d268`).** `--tts "…"
--voice sample.wav --ref-text "…"` bakes the reference in-memory before the
backend loads and synthesizes in that voice in one command (shared helper
`tada_run_aligner_pipeline`; fails loudly if the ref can't be written). Validated
end-to-end.

The remaining OPEN half is the **server / C-ABI** on-the-fly path: accept raw
audio (+ transcript) on `/v1/audio/speech` (with the existing consent gate) and
in `crispasr_session_synthesize`, baking in-memory without a temp GGUF. The CLI
helper is the reference; the server needs the encoder/aligner loaded in the
session and the `ref_text` request field wired.

**Approach:** the make-ref pipeline already exists in C++ (`src/tada_encoder.{h,cpp}`
+ wav2vec2 aligner runtime; CLI `--make-ref` drives it via wav2vec2 aligner → BPE
tokenization → DP alignment → WavEncoder → LocalAttentionEncoder → ref GGUF in
memory). To expose it at query time:
1. Load the encoder + aligner GGUFs in the TADA backend `init()` when configured
   (new flags, e.g. `--make-ref-encoder` / `--make-ref-aligner`, already parsed
   for the CLI path — reuse them). Keep them optional: only pay the ~1.3 GB
   (178 MB encoder + 1.1 GB aligner) when ref-baking is enabled.
2. In `synthesize()` / the server handler, when `voice` is a `.wav`, run the
   in-memory make-ref to produce prompt_values/positions and feed them straight
   into the context (skip the GGUF round-trip), or bake a temp ref GGUF and
   `tada_load_prompt` it. Requires a transcript — take it from a `ref_text`
   request field (the consent gate + `consent_attestation` already exist for
   `.wav` voices on `/v1/audio/speech`).
3. Cache the baked ref keyed by (audio hash, transcript) so repeat requests with
   the same clip don't re-run the aligner.

**Files:** `examples/cli/crispasr_backend_tada.cpp` (init load + synthesize bake),
`examples/cli/crispasr_server.cpp` (`ref_text` field + `.wav` voice path),
`src/tada_tts.{h,cpp}` (a `tada_make_ref_from_pcm` entry that returns prompt
state without a GGUF), `src/tada_encoder.*` (reuse). Aligner is language-specific
(`tada-aligner-<lang>.gguf`) — must match the audio language.

**Effort:** Medium-large. The pipeline is ported and CLI-proven; the work is
wiring it into the server lifecycle + per-request path + caching, plus the
~1.3 GB memory cost gate. Lower priority than the switch-voice half (shipped),
which already covers the common "I baked refs, let me pick one live" workflow.
Tracked on #201 (left open for this).

---

## §192 follow-up — native-Vulkan TADA garbled output FIXED (codec on CPU)

**FIXED 2026-06-29** (branch `fix/tada-vulkan-repeat-f16`). The garbled/empty
native-Vulkan output was **the codec, not the FM head** — the prior "FM
time-dimension divergence" localization was a red herring. Now: native Vulkan
"I went to school and back in four hours" (seed 1) → intelligible, **identical to
Metal**; +2 sentences +seed2 all ASR-round-trip cleanly; deterministic; CPU
output byte-identical to before (fix is Vulkan-gated). Still gated
(`CRISPASR_TADA_VULKAN_NATIVE=1`), default remains CPU-fallback — ask reporter
(BergmannAtmet, RADV) to confirm before flipping the default.

**Actual root cause (the FM premise was wrong; the codec is the culprit).** A/B'd
Metal vs Vulkan native on the same input: both produce **bit-identical** durations
(`time_before = [38,2,9,6,8,8,5,10,9,5,211]`) and 522 frames — the talker + FM +
duration decode AGREE across GPUs. Metal renders them intelligibly; Vulkan
rendered empty audio. So the divergence is purely in **audio rendering = the
codec** (`tada_codec.cpp`). Pinned down with per-stage `TADA_CODEC_DUMP` (Vulkan
vs CPU, identical 522-frame features): the **attention encoder MATCHES** across
backends (`dump_attn`, `dump_layer0` identical), but the **DAC decoder front-end
explodes** — `dump_dac_in` (the `in_conv` Conv1d → im2col+mul_mat) jumps to
rms ~37 / range ±800 on Vulkan vs rms ~0.85 / ±8 on CPU (~43×), propagating to the
output; the final `Tanh` masks the range but the audio is distorted. It is
**size-dependent**: short inputs ("Hello world") render *correctly* on Vulkan
(per-stage dump bit-comparable to CPU), and only break past some sequence length.

**NOT what earlier notes claimed (each verified, not assumed).** It is **not** a
`ggml_backend_sched` cross-backend-copy bug — `GGML_SCHED_DEBUG=2` shows the codec
runs as a *single Vulkan split*, no CPU offload, no cross-backend copies. It is
**not** missing Vulkan kernels — ggml-vulkan has `conv_transpose_1d`/`col2im_1d`/
`im2col`/`mul_mat`/`flash_attn_ext`, and the codec uses **no ISTFT** (DAC *conv*
decoder). It is **not** precision — F32 weights change nothing (MoltenVK
downconverts src0→f16); and the real in_conv weight is tiny (absmax 0.068), so no
overflow. It is **not even a conv/im2col kernel bug** — a standalone repro of
`conv_1d` / `im2col` / the full `wn_conv1d` (transpose+cont→conv→reshape+transpose)
at the codec's exact dims and T=522 is **bit-correct** on MoltenVK, and capping
`GGML_VK_FORCE_MAX_ALLOCATION_SIZE` doesn't help. So it's a **graph-scale
gallocr/aliasing-class corruption** in the large real codec graph — the in_conv
output is wrong despite a correct input (`dump_attn` matches) — that only bites at
length and isn't reproducible in a minimal harness. (Also disproven for the FM red
herring: F32 FM weights and whole-FM-on-CPU both left output bit-identical;
`time_before` already matched across backends.)

**Fix.** When the codec's GPU backend is Vulkan, run the whole codec on the CPU
backend (`src/tada_codec.cpp`, `tada_codec_init_from_file_impl`). The codec is a
one-shot decode (not the AR loop) and its input features are bit-identical
Metal-vs-Vulkan (Metal renders them correctly), so CPU rendering is faithful. The
talker/FM keep their native-Vulkan path. Opt back into the broken native codec
with `CRISPASR_TADA_CODEC_VULKAN_NATIVE=1` (debug only). **Open follow-up (to keep
the codec on GPU):** the op-level repros rule out the conv kernels, so the
remaining lever is the graph-scale corruption — best chased on RADV (real
hardware, where the talker/FM native path is also pending validation) with a
ggml-vulkan gallocr/graph dump, not MoltenVK. A **chunked codec decode** (decode in
time-windows under the breaking length, where short inputs are proven correct) is
the pragmatic GPU-native workaround and sidesteps the root cause entirely.

**Chunked-decode design — DEFERRED, low priority (not queued; design-only).**
Decision 2026-06-29: do **not** implement this now. The codec-on-CPU fix already
solves #192 (validated, shipped, default). The codec is a one-shot decode, *not*
the AR bottleneck, so a GPU-native codec buys little; the corruption is
MoltenVK-only locally while the reporter is on RADV (it may not even reproduce
there). Net: high implementation+validation complexity (window seams, absolute
RoPE bookkeeping, empirical sample-trim — the codec emits `n_frames*480 − ~6`
samples, non-exact) for a marginal, driver-specific payoff. Pick this up only if
a RADV user actually needs the codec on GPU. Design preserved below for whoever
does.

Add `tada_codec_decode` a chunked wrapper, gated (e.g. `CRISPASR_TADA_CODEC_CHUNK=<N>`,
default off; auto-on for the Vulkan-native codec). The codec upsamples 480×
(strides 4·4·5·6) and is feed-forward; the only cross-frame coupling is (a) the
block attention (each frame attends to its block + the previous block) and (b) the
conv receptive field across the 4 decoder blocks. So decode is chunkable with a
context margin:
  - Split the `n_frames` features into windows of `N` (start ~192, below the
    observed break; tune by bisecting the threshold between 93 ok and 522 broken).
  - For window `[a,b)`, run the existing `build_decode_graph` on the *extended*
    slice `[a-CTX, b+CTX)` (clamped), `CTX` ≈ 64 frames to cover both the conv
    receptive field and the prev-block attention reach.
  - **RoPE positions must be absolute**: set `codec_pos[i] = (a-CTX)+i`, not 0-based,
    so attention matches the full decode for the kept region.
  - Build the block mask from the *sliced* `token_masks[a-CTX : b+CTX]` (block ids
    are relative-safe — the mask only encodes same/prev block).
  - Keep only the PCM for `[a,b)`: trim `CTX*480` samples from each side (except at
    the true sequence start/end); concatenate windows.
  - Validate: ASR-roundtrip vs the CPU codec, and check for boundary clicks (peak
    discontinuity at window seams); widen `CTX` / add a short crossfade if needed.
Risk: a conv vocoder can click at seams; the CTX-trim should prevent it but needs
the ASR + waveform check. Keep CPU-codec as the default fallback until validated.

**Caveat — MoltenVK can't fully validate GPU numerics.** MoltenVK's `mul_mm` /
`mul_mat_vec` downconvert src0 to f16 regardless of stored dtype (storing F32 FM
weights changed nothing; `GGML_VK_DISABLE_F16=1` barely moved cond cosine,
0.99919→0.99966). So the talker `cond` is ~0.9997 vs CPU on MoltenVK — fine here
(durations/candidates still agree with Metal), but a reminder that exact CPU
parity isn't achievable on MoltenVK. On RADV (f32-native matmul) the talker is
more precise; the codec-on-CPU fix is the operative change regardless.

**Preconditions (hard-won — see LEARNINGS §192).** (1) Build with
`-DGGML_METAL=OFF` and verify the `backend=Vulkan0` log line — Metal silently
wins otherwise. (2) **Free the disk first** — benchmarking with `/Volumes/backups`
near-full gives SIGBUS + nondeterministic frame counts that masquerade as a
signal. (3) Re-run each config twice for determinism before forming a hypothesis.

---

## Cross-platform (Linux/x86 CPU) validation — DONE (green) + audit tooling

The moss-transcribe / higgs-stt / ark-asr work was validated on M1/Metal only, so
the missing leg was x86 CPU-only Linux (long history of Metal-masked bugs). Ran on
the 8 GB CPU-only Ubuntu VPS (`root@168.119.190.252`, x86_64, Linux 6.8) at HEAD
`dcc7e47b` — results in `handover-prompts/vps-validation-results.md` on the VPS:

- [x] Cold build `-DGGML_METAL=OFF -DGGML_CUDA=OFF` — PASS.
- [x] ASR roundtrip on jfk.wav, CPU/x86 — **all three verbatim, identical word
  sequence to the M1/Metal baseline**: moss-transcribe / higgs-stt / ark-asr. **No
  Metal-masked or x86-SIMD divergence.**
- [x] `higgs-stt -bs 2` and `ark-asr -bs 2` — **token-identical to greedy on Linux
  CPU** (beam works off-Metal).
- [x] Go cgo LDFLAGS **drift check PASS on Linux** (`sync_go_cgo_ldflags.py --check`
  clean — incl. `-lmoss_transcribe`). Actual `go build/test` **skipped** (no Go
  toolchain on the VPS); the real link runs in CI (ubuntu-22 `Bindings Tests (Go)`).
- Timing (8 GB CPU, 11 s audio): moss 125 s, higgs 152 s; **ark-asr 2243 s —
  severely swap-bound** (3 B q4_k = 3.4 GB on 8 GB). Expected, not a bug.

Follow-ups (LOW):
- [ ] The VPS run only executed 2 unit tests (it built `crispasr`/`crispasr-diff`
  targets, not the test targets) — the handover should `cmake --build build` (all)
  before `ctest -L unit`. Fix the handover / or the VPS builds all targets next time.
- [ ] Install the Go toolchain on the VPS (or leave the Go link to CI) to close the
  one SKIPPED check.
- [ ] Optional: promote to a standing post-push Linux smoke (Routine/cron).
- Handover prompt staged at `handover-prompts/` (gitignored) + scp'd to the VPS.

**Multilingual + beam-quality spot-checks (LOW, either machine):**
- [ ] moss-transcribe is a zh/en model; only English (jfk) validated. Run one German
  + one Chinese clip (de fixtures already local under audio_samples/).
- [ ] Beam *helps* check: higgs/ark beam-2 == greedy on the easy JFK clip (proves no
  regression); run `-bs 4` on a noisy/accented clip to see if WER actually improves
  or beam is just cost.

**Audit tooling (reusable):**
- [x] `tools/check-backend-wiring.py` (commit `ccc04a02`) — audits every canonical
  backend against the required surface (factory / c_api dispatch + `available_backends`
  / feature-matrix / cli.md-beam) and reports advisory coverage gaps (test, reference
  dumper, README, registry, streaming). Aliases + standalone dumpers handled; Go check
  delegates to `sync_go_cgo_ldflags.py` (advisory; macOS bare `--check` unreliable).
  Linked from `docs/contributing.md`. 49 canonical PASS required.

**Older-backend coverage gaps the audit surfaced (LOW cleanup — not blocking):**
Run `python tools/check-backend-wiring.py` to re-list. As of `ccc04a02`:
- [x] **missing a test** — added `test-{voxcpm2-tts,firered-asr,moonshine-streaming}-params.cpp`
  (the three with a standalone `*_context_default_params`/`init_from_file`/`free` API).
  `fastconformer-ctc` and `wav2vec2` are **shared encoder components** (no standalone
  context — used by parakeet/canary etc.), so a params test is N/A; left as-is.
- [ ] **missing a reference dumper** (diff-harness coverage): `fastconformer-ctc`,
  `wav2vec2`, `m2m100`, `kyutai-stt`, `gemma4-e2b`. Mostly *intentional* — m2m100 is
  text-only MT (no audio diff), gemma4-e2b shares the gemma path, the encoder
  components diff via their host backends. Confirm per-backend before adding; not a
  blanket gap.
- [~] **`qwen3-tts` streaming.md** — NOT a gap. `streaming.md` documents ASR live
  transcription; the `streaming` cap on qwen3-tts (the only backends that set it)
  means incremental PCM *synthesis* (a TTS feature, in tts.md). Audit refined to only
  expect a streaming.md row for ASR backends (`streaming` cap && !`tts`).

---

## Recent-backend audit — wiring gaps + easy wins (last 10 backends) — CLOSED

Audit of the 10 most recently added backends (moss-transcribe, higgs-stt,
ark-asr, dots-tts, nemotron, mini-omni2, lfm2-audio, tada, kugelaudio, melotts).
Core wiring (CLI factory, c_api `available_backends`, registry, Go LDFLAGS,
README, the auto-generated feature matrix) was complete for all 10. Follow-ups,
all now closed:

**Completeness gaps:**
- [x] **kugelaudio test** — added `tests/test-kugelaudio-params.cpp` (5/5 green).
- [x] **env-live-tests entries** for `tada`, `kugelaudio`, `melotts` — added the
  `CRISPASR_MODEL_*` exports.
- [~] **melotts diff-harness "registration"** — NOT a real gap. `melotts.py` uses
  the **standalone** reference-dumper pattern (run directly), which is the *majority*
  convention: 20 of the reference dumpers are standalone (bark, csm, dia, fastpitch,
  piper, speecht5, vibevoice, tada_codec_diff, …) and only a handful use the
  `dump()` + `REGISTERED_BACKENDS` path. `melo` isn't even installed. Left as-is.

**Beam search (DONE — validated token-identical greedy↔beam-2 on jfk.wav):**
- [x] **higgs-stt** — `core_beam_decode::run_with_probs` (multi-EOS: im_end +
  endoftext), `CAP_BEAM_SEARCH` + `higgs_stt_set_beam_size`. q4_k greedy == beam-2
  verbatim.
- [x] **ark-asr** — runtime beam loop (adapter already plumbed `beam_size`),
  `CAP_BEAM_SEARCH`. `ark_run_decoder` already handles multi-token suffixes, so the
  replay is a one-liner. q4_k greedy == beam-2 verbatim (CPU).
- *Not candidates:* mini-omni2 (interleaved multi-stream), nemotron (RNN-T has its
  own beam), TTS backends (n/a).

**Optimization notes (NOT easy wins — recorded so they're not mistaken for low-hanging fruit):**
- Graph caching of the encoder/decode graph is a KNOWN trap: §176s cache is not
  re-entrant with a shared sched (2nd reuse collapses to empty), and the
  chatterbox/CFM graph-cache was a measured DUD (host build+alloc ≈ 0.3% of a
  compute-bound step). Measure `ggml_time_us()` on build+alloc BEFORE porting.
  **Parakeet enc cache measured 2026-07-01 (#208): also a DUD** — per uniform
  20 s window on M1 Metal, build ≈0.3 ms / alloc ≈1 ms / GPU compute ≈1 s, so a
  re-entrant cache saves ≈0.04%. Not on the roadmap; the `PARAKEET_ENC_PROBE`
  env hook reproduces both the timing and the 2nd-reuse std collapse. Real
  chunked-long-audio headroom = encoder compute + the ~40% overlap re-encode.
- `flash_attn_ext` is absent in tada / melotts (small attention — marginal).
- ark-asr is CPU-only; Metal-validating it is a real win but carries the usual
  sched/precision risk — not "easy."

---

## #215e follow-up — audit cross-call cached cgraphs for the gallocr UAF (DONE)

The #215 root cause (HISTORY #215e) is generic: a cgraph cached ACROSS
`sched_alloc_graph` invocations keeps `tensor->buffer` pointers into gallocr
buffers that any larger interleaved graph on the same sched frees on regrow; the
next alloc of the cached graph reads freed `ggml_backend_buffer` structs
(ASan-verified on moss-transcribe) and on native Vulkan writes through stale
`vk_buffer` handles → driver-heap corruption → delayed driver crash
(vkResetCommandPool). moss_transcribe + moss_audio are fixed (rebuild/invalidate
per encoder invocation). Same pattern still present in:

- **canary** (`cached_enc_gf`, keyed on T_mel, decoder graphs interleave on the
  same sched) — vulnerable whenever consecutive slices have equal T_mel (uniform
  30 s chunks!). Repro recipe: ASan build + >=2 equal-length slices.
- **canary_ctc** (`cached_gf`, two alloc sites) — check whether both graph shapes
  share one sched.
- **chatterbox_s3gen** (`unet_cached_gf` + many other graphs on the shared sched).
- Grep beyond these: `grep -rn "cached_.*gf" src/` and any new §176s-style caches.

**Completed 2026-07-11:** full audit done. canary + canary_ctc fixed
(rebuild-every-invocation, same as moss_transcribe/canary_qwen/funasr/glm_asr/
granite_speech encoder). chatterbox_s3gen uses a dedicated gallocr (safe).
All `cached_*_gf` sites verified across the codebase. 796/796 unit tests pass.

---

## moss-transcribe follow-ups (OPEN, LOW)

The `moss-transcribe` backend (`OpenMOSS-Team/MOSS-Transcribe-preview-2B`) shipped
`9f3c5ede` — q4_k verbatim on jfk.wav, validated against the PyTorch reference via
`crispasr-diff`. See HISTORY (`## moss-transcribe`) and LEARNINGS. Remaining optional
work:

- **DONE #218 — greedy n-gram loop collapse.** On the reporter's 145 s `t32-145s.wav`
  (auto-sliced into 6×30 s), two slices' greedy decode fell into a repeated-phrase
  attractor and emitted "Hey, hey, hey, …" up to the 512-token cap (slice 2: 511 tokens,
  ~490 of them "hey,"; slice 5: "run hey hey hey hey hey run" cycles). Upstream MOSS has
  no post-process for this. Fixed by collapsing immediately-repeated n-grams in the
  decoded text — the same algorithm higgs-stt already ships (its `ngram_loop_fix.py`
  port), now extracted to the shared header `src/core/ngram_loop_fix.h`
  (`core_ngram::fix_loops`) and used by both. Pure text post-process → token/logit
  parity vs the Python reference is unchanged, and it is a no-op on non-degenerate
  slices (verified: clean slices byte-identical, jfk diff-harness unaffected). Opt out
  with **`CRISPASR_MOSS_TRANSCRIBE_NO_LOOPFIX=1`** for raw upstream-parity output. Unit
  coverage: `tests/test-ngram-loop-fix.cpp` (`[ngram-loop]`, label `unit`).
- **DONE #218 — 30 s-seam duplication.** Same clip: adjacent 30 s slices duplicated
  their shared audio at each boundary ("…move much **of** the fence. Don't move much
  **to** the fence."). Cause: overlap-save (issue #89) extends each slice by ±3 s and
  trims back by **word-level filtering**, but moss-transcribe emits no word/token
  timestamps, so the trim falls through to segment-level filtering that keeps the whole
  extended segment → the ±3 s context is transcribed twice. The over-long buffer also
  worsened the greedy loops above (a slice that looped to the 512-token cap *with*
  context only looped ~50 tokens on the bare 30 s slice). Fixed by adding
  `"moss-transcribe"` to `kBlocked` in `examples/cli/crispasr_chunk_context_gate.h` —
  the opt-out its LLM-decoder peers (qwen3, granite, voxtral, …) already use — so long
  audio is sliced at a bare 30 s with no overlap extension. Gate unit test updated
  (`tests/test-issue-114-chunk-context-gate.cpp`). Tradeoff (accepted, matches peers):
  a word straddling a 30 s cut can be lost/split; net far better than the dup + longer
  loops. Proper per-token timestamps would let overlap-save trim cleanly, but moss has
  no alignment — noted as a future option, not worth the crude uniform-timestamp hack.
- **REJECTED #218 — input time markers.** `processing_Moss.py`'s `enable_time_marker`
  (interleave digit tokens every 2 s at 12.5 tok/s) was tested as a possible way to
  give the greedy decoder positional grounding → fewer long-audio loops → larger
  chunks → smaller seam tradeoff. Prompt construction verified byte-identical to the
  reference, but on `t32-145s.wav` it was **markedly worse**: the model reads the digits
  aloud ("Two, three, four, five…"), hallucinates, loops to the 512 cap, and runs 2.5×
  slower — because the feature defaults to `False` and this checkpoint was RL-tuned with
  markers off (no learned meaning for the digits). Reverted; see LEARNINGS. Only worth
  revisiting for a future MOSS checkpoint trained with `enable_time_marker=True`.
- **Publish f16 + q8_0** to `cstr/MOSS-Transcribe-preview-2B-GGUF` (q4_k + card are
  live; f16/q8_0 were held back for WLAN bandwidth). Re-stage from
  `/Volumes/backups/ai/moss-transcribe-preview-2b-{f16,q8_0}.gguf` and
  `hf upload-large-folder` into the existing repo; both already produce the verbatim
  transcript locally (q8_0 even keeps the trailing period).
- **GPU validation beyond Metal.** Metal (default) and CPU both verbatim; CUDA/Vulkan
  untested. The LM reuses `core_attn::kv_self_attn` (covered by the §192/#200 Vulkan
  F16-GQA guard), so the encoder's windowed `flash_attn_ext` + the conv front-end are
  the parts to check on CUDA.
- **Multilingual eval.** Authors report 4.87 % avg WER; only English (jfk) validated
  here. The model is zh/en — spot-check a Chinese clip.
- **Encoder precision.** Encoder/adapter run F16 (cos ~0.98 vs the f32 reference);
  byte-exact at layer 0, so this is pure F16 weight precision, not a bug. An f32
  encoder path is not worth it (decode is verbatim), but noted for completeness.

---

## #218 qwen3-asr long-audio root cause — quantized audio tower + prompt contract (DONE)

Follow-up to the fix_loops mitigation: WHY did qwen3-asr loop / emit empty output on
the reporter's 145 s clip at all, when the bf16 reference is clean on the same audio?
Diff-harness verdict (`crispasr-diff qwen3` extended with conv/per-block/ids/logits
stages; bf16 reference re-dumped via `tools/reference_backends/qwen3.py`):

- **Port math is faithful.** On the full un-chunked 145 s clip: mel cos_mean
  0.999996, prompt ids EXACT (1900/1900 incl. 1885 audio pads), splice complete,
  F16-GGUF LLM first-logits cos 0.9981 vs bf16 (= the known F16 floor), F16 e2e
  transcript ≈ verbatim reference. The reference's eager/SDPA (CPU) encoder does
  FULL attention — `cu_seqlens` windowing only exists on the FlashAttention-2
  path — so our mode-0 full-attention graph matches the blueprint's CPU semantics.
- **Root cause: sub-8-bit `audio.*`.** The old q4_k GGUF quantized the 18-layer
  tower to Q4_0/Q4_K; per-block drift (blk00 cos 0.9996 → blk17 0.973, no cliff)
  compounds to encoder cos_mean 0.9716 / cos_min 0.75 at 1885 frames, enough to
  flip greedy decode into "language none" (empty output) or mid-transcript
  repeated-phrase attractors ("Hey, hey, …" — the #218 loop). Fixed by a Q8_0
  floor for `audio.*` in crispasr-quantize (like the canary/mini-omni2 encoder
  carve-outs, ~half the size cost: q4_k 540→631 MB); opt-out
  `CRISPASR_QWEN3ASR_QUANT_AUDIO=1`. Re-quantized q4_k: encoder cos_mean 0.9997 /
  cos_min 0.992, e2e un-chunked 145 s clean + complete with `fix_loops` DISABLED,
  in both auto and forced-language modes. jfk verbatim.
- **Prompt contract (blueprint parity).** qwen_asr's `_build_text_prompt` expresses
  a forced language as an ASSISTANT-TURN PREFILL `language <Name><asr_text>` (the
  system turn carries only the optional context) — the model then emits transcript
  text ONLY and structurally cannot answer "language none". Our adapter used a
  "Transcribe the speech in X." system instruction instead; switched to the
  blueprint prefill (CLI + streaming + session ABI;
  `CRISPASR_QWEN3_SYSPROMPT_LANG=1` restores the legacy form). The tokenizer now
  also recognises bare `<asr_text>`-style added tokens.
- **More blueprint parity:** tail-chunk padding frames are now removed before the
  encoder blocks (`padded_embed[padded_mask_after_cnn]` contract; in-graph
  `get_rows` compaction in crisp_audio, `CRISP_AUDIO_KEEP_PAD_FRAMES=1` opt-out) so
  N_audio == the processor's `_get_feat_extract_output_lengths` for any T_mel;
  `max_new_tokens` fallback 256→512 (blueprint default); KV cache sized
  `max(4096, prompt+max_new+16)` and grow-on-demand (a fixed 4096 capped un-chunked
  audio at ~5 min).
- **Note:** the reference wrapper itself ships `detect_and_fix_repetitions()`
  (≥20× char/pattern collapse) in `parse_asr_output` — `core_ngram::fix_loops` is
  blueprint-faithful defense-in-depth; keep it.
- **CAP_UNBOUNDED_INPUT — windowed encoder attention SHIPPED opt-in
  (2026-07-10).** `CRISP_AUDIO_WINDOWED_ATTN=1` runs the encoder with the
  FA2/cu_seqlens block-diagonal semantics: full 104-frame windows as ONE
  batched unmasked attention + the ragged tail as a second small attention,
  concatenated — O(N·W) memory (no dense mask at all), enabling arbitrary
  audio length. Verified vs a windowed bf16 reference (new
  `QWEN3_REF_WINDOWED=1` dumper mode monkey-patches the block-diagonal mask
  from the modeling's own `_prepare_attention_mask` into eager attention):
  encoder cos_mean **0.99953** on the un-chunked 145 s clip (fixed-tower
  q4_k). e2e: the windowed path transcribes MORE of the clip (incl. the
  low-volume leading section full attention skips) with no raw loops; note
  the windowed *blueprint itself* transcribes noise sections aggressively
  (its own output shows collapsed "hey" runs there) — that behaviour is
  inherent to the training-time attention, fix_loops handles it. Encoder is
  somewhat slower on Metal (many 104² matmuls vs one big one). Default
  stays full attention (matches the CPU blueprint) + 30 s dispatcher chunks;
  flipping the default + declaring CAP_UNBOUNDED_INPUT needs a broader eval
  (more clips, speed profile) — the mechanism is ready.
- **Follow-ups:** (1) DONE — fixed q4_k / q4_k-imatrix / q3_k-imatrix uploaded to
  `cstr/qwen3-asr-0.6b-GGUF` + README (q8_0/f16 unaffected). A/B during the
  re-bake: quantising the tied LM head (the old imatrix recipe) re-introduces
  long-form loops even with the Q8_0 tower → imatrix variants now keep the head
  at F16; and even so the EN+DE short-clip imatrix still drifts into repetition
  on the un-chunked 145 s clip where plain q4_k is clean — README carries a
  long-form caveat (prefer `-q4_k`/`-q8_0` for `--chunk-seconds 0`).
  RECALIBRATION TESTED AND REJECTED (2026-07-10): an imatrix regenerated with
  6 long-form concatenated Common Voice clips (49–127 s, run single-pass)
  among the 48 short ones STILL drifts into the same repetition attractor at
  the same spot — the activation-weighted redistribution itself harms
  long-context decode; calibration mix doesn't rescue it. Plain q4_k stays
  the long-form recommendation; the published v2 imatrix files stay for
  short-clip use. Long-form producer recipe kept in tools/imatrix-calib/
  provenance + the generated matrix at
  /Volumes/backups/ai/crispasr-gguf/qwen3-asr-0.6b-longform.imatrix.gguf.
  (2) DONE — session-ABI spot-check (python Session → qwen3 forced-language
  prefill) passes. (3) DONE — cohere-transcribe + glm-asr audit via Kaggle
  kernel `tools/kaggle/issue218-quant-audit/` (T4, HEAD c555a40b, raw decode
  via the new `CRISPASR_NGRAM_LOOPFIX_OFF=1` gate, canonical 145 s clip):
  **no carve-outs warranted.**
  - cohere: q4_k and F16 behave IDENTICALLY (max unigram run 7 at both — the
    clip genuinely contains repeated "hey" shouts; both reach the final
    sentence, chars within 2 %). Quantisation has no behavioural effect.
  - glm-asr: raw decode degenerates at BOTH precisions (max unigram run 255
    at q4_k, 163 at F16). ~~An inherent model behaviour~~ — CORRECTED by the
    follow-up blueprint comparison (see '#218 glm-asr long-form blueprint
    parity' below): both A/B arms ran INSTRUCTION-LESS prompts (specials-only
    tokenizer silently dropped every instruction), which is what degenerates.
    With the blueprint prompt the model matches the bf16 reference at q4_k —
    no quant carve-out needed. qwen3-asr remains the only quant-caused #218
    case; granite + canary-qwen were already protected.

---

## #218 glm-asr long-form blueprint parity — instruction + multi-window (DONE)

User mandate: glm-asr must match the Python blueprint on the reporter's 145 s
clip. Ground truth from the REAL `GlmAsrForConditionalGeneration`
(transformers 5.13, bf16, Kaggle kernel `tools/kaggle/glm-asr-blueprint-ref/`,
results in the run's `results/{jfk,t32}.json`): jfk → 27 greedy tokens; t32 →
ONE 5-window 1827-token prompt, 115 tokens of clean dialogue starting
"Alex, you okay?" (the blueprint itself skips the quiet first ~70 s), NO
loops, NO empty output.

Root causes found (neither was quantization):

1. **No instruction ever reached the model.** `glm_asr_tokenize` is
   specials-only (the GGUF carries no BPE merges), so every plain-text
   instruction tokenized to NOTHING — and the CLI adapter always sets one
   ("Please transcribe in English.", `--language` defaults to en), so the
   blueprint's mandatory default prompt ("Please transcribe this audio into
   text") was silently absent from EVERY glm-asr prompt ever sent.
   Instruction-less prompts are off-distribution: the model hallucinates
   shout-loops on noise windows and instant-EOSes (`<|user|>`) on
   multi-window prompts. Fixed: blueprint default-instruction ids baked in
   (encoded with the repo's own tokenizer.json, verified against the
   transformers-5.13 processor dump), un-encodable custom instructions fall
   back to it with a warning. Chat-template newlines were also missing;
   the answer framing ('The spoken content of the audio is "…"') is now
   stripped like `GlmAsrProcessor._strip_assistant_prefix_and_quotes`.
2. **Backend truncated everything past 30 s.** `glm_asr_transcribe_impl`
   padded/truncated to ONE 3000-frame window. Fixed with the blueprint
   multi-window path: 30 s sample windows, each encoded on the padded canvas
   then trimmed to its valid post-conv frames (conv (1,3,1)+(1,3,2), merge 4
   — N matches `_get_feat_extract_output_lengths` exactly; t32 = 1812),
   concatenated into ONE prompt, ONE decode. Windows grouped per-window in
   `<|begin_of_audio|>…<|end_of_audio|>` blocks (the zai-org/GLM-ASR
   `inference.py` training-time layout; the HF processor's single-group
   variant behaves identically in ground truth). Cap 21 windows = 655 s
   (processor `max_audio_len`), warning beyond. KV cache prompt-sized +
   grow-on-demand (fixed 4096 would cap ~4.5 min).

Verification (q4_k GGUF, Metal): jfk prompt 152 tokens == blueprint, 27
generated ids == blueprint, text identical. t32 `--chunk-seconds 0`: 113 vs
blueprint's 115 tokens, near-verbatim ("I'm" vs "I am"-level q4_k variance),
same scope. Default 30 s-chunked path now recovers the whole dialogue incl.
slices the old prompt lost; raw noise-window "hey" runs are genuine audio
shouts, collapsed by fix_loops as before. Session ABI inherits everything via
`glm_asr_transcribe_impl`; the CLI streaming path mirrors the prompt. Gates:
`CRISPASR_GLM_ASR_SINGLE_WINDOW=1` (old truncate-to-30 s),
`CRISPASR_GLM_ASR_LEGACY_PROMPT=1` (old instruction-less scaffold),
`CRISPASR_GLM_ASR_DEBUG=1` (raw decode dump).

Follow-ups: (1) DONE — `tokenizer.ggml.merges` baked into converter + a
backfill tool (`tools/gguf-add-merges.py`) patched the published GGUFs;
`glm_asr_tokenize` is now core_bpe byte-level BPE (verified byte-exact vs
tokenizers-lib for the default/ask/translate/language instructions), the CLI
skips the language instruction for en/auto (English IS the blueprint default
prompt's behaviour), and instruction framing matches the verified scaffold
("\n" + text, no trailing newline); (2) CAP_UNBOUNDED_INPUT decision —
same posture as qwen3-asr for now (default 30 s chunks, `--chunk-seconds 0`
opt-in single-pass ≤655 s); (3) the blueprint itself SKIPS quiet leading
audio in single-pass mode (starts at ~75 s on t32) — chunked mode covers more
content on such clips; document in README.

## #218 qwen3-family rebake + CUDA validation — kernel results (DONE)

Kaggle kernel `tools/kaggle/qwen3-family-rebake/` (T4, HEAD 199609d0):

- **CUDA validation of the week's fixes:** qwen3-asr 0.6b un-chunked 145 s
  single-pass CLEAN on CUDA (full attention, 686 chars, reaches the final
  sentence, zero loops raw); glm-asr multi-window single-pass CLEAN on CUDA
  (reaches final sentence); glm jfk verbatim. The WINDOWED qwen3 path loops
  in the noisy lead on CUDA (238-run) where Metal recovered — consistent
  with the windowed blueprint's own noise behaviour + greedy sensitivity to
  backend numerics; reinforces keeping `CRISP_AUDIO_WINDOWED_ATTN` opt-in.
- **qwen3-asr-1.7b:** header audit shows its q4_k ALREADY has a Q8_0 audio
  tower — no rebake needed.
- **qwen3-asr-1.7b-ja-anime:** q4_k tower was Q4_K → rebaked with the floor
  (1334→1421 MB), validated (jfk 109 chars; t32 single-pass 1024 chars,
  max run 1, no loops) and UPLOADED.
- **mega-asr:** q4_k tower was Q4_K → rebaked all three variants with the
  floor, but ALL of them still degenerate on the un-chunked 145 s clip —
  q4_k and q3_k-imatrix produce a "Come on, come on, …" 2-gram cycle (the
  kernel's unigram-run metric read that as clean; the transcripts don't
  lie), q4_k-imatrix shows the familiar hey-run (172). Q8 tower alone does
  not fix mega's long-form drift → uploads correctly skipped, published
  files unchanged. mega's DEFAULT 30 s-chunked path is unaffected; treat
  `--chunk-seconds 0` as unsupported for mega at 4-bit until investigated
  (bf16 blueprint behaviour unknown — candidate follow-up).
- Metric lesson: loop gates need a PHRASE-cycle check, not just unigram
  runs (the issue218-quant-audit kernel has one; this kernel didn't).

## #218 arc — remaining open threads (OPEN)

Everything user-facing from the #218 investigation is fixed and shipped
(qwen3-asr 0.6b + ja-anime GGUFs rebaked on HF, glm-asr merges + multi-window
landed, CUDA-validated). Deliberately-open threads, in priority order:

1. **mega-asr long-form vs its Python blueprint — RESOLVED (2026-07-10).**
   Kaggle kernel `tools/kaggle/mega-asr-blueprint-ref/` (bf16, raw decode,
   LoRA hand-merged 539 pairs since the adapter declares target_modules='.*'
   which peft can't wrap): the bf16 BASE Qwen3-ASR-1.7B is CLEAN long-form
   (max phrase-cycle 3, both auto and forced-English), the bf16 MEGA
   (LoRA-merged) LOOPS MASSIVELY (unigram run 230, phrase-cycle 115, both
   modes — the prefill doesn't prevent it). Mega's long-form degeneration is
   LoRA-INDUCED AND MODEL-INHERENT; our 4-bit port reproduces the blueprint
   faithfully and the Q8-tower rebakes were behaving correctly. Verdict:
   nothing to fix in the port — fix_loops + the 30 s chunked default is the
   correct posture for mega, `--chunk-seconds 0` stays unsupported for it at
   ANY precision. (Kernel also demonstrates the dep recipe for qwen_asr on
   Kaggle: transformers==4.57.6 + force-reinstalled huggingface_hub 0.36 +
   hf_transfer + sys.modules purge, and the LoRA path rewrites
   thinker.layers → thinker.model.layers.) No-regression check on the
   published mega q8_0 + current main, DEFAULT settings (30 s chunks,
   fix_loops on): jfk verbatim; t32-145s full dialogue start-to-final-
   sentence, genuine shouts bounded — mega's previously-working paths are
   intact and improved by the blueprint prompt.
2. **Windowed encoder default flip + CAP_UNBOUNDED_INPUT (qwen3-asr) —
   RESOLVED: FLIP REJECTED (2026-07-10).** Broader eval (M1 Metal, q4_k-v2
   Q8-tower GGUF, raw decode via CRISPASR_NGRAM_LOOPFIX_OFF=1, single-pass,
   phrase-cycle metric): on t32-145s the windowed path degenerates in the
   noisy lead ON METAL TOO (unigram run 238 / cycle 119, never reaches the
   final sentence; it attends the quiet lead, then greedy collapses on
   noise) while full attention is clean+complete (run 1 / cycle 0) — the
   earlier "Metal recovers" observation was not robust; greedy is
   knife-edge there and CUDA already looped. On clean speech (jfk×12,
   132 s) both modes are clean; both EOS early on genuinely 12×-repeated
   audio (model behaviour; full slightly more complete, 198 vs 176 words).
   Windowed is also ~1.5× slower on Metal at these lengths (jfk×12: 397 s
   vs 271 s wall; t32: 658 s vs ~400 s) — the many-104²-matmuls cost is
   real; peak RSS ~2 GB in both modes at 145 s (the O(N·W) advantage only
   matters beyond full attention's ~10-min OOM point). Verdict: default
   stays FULL attention + 30 s chunks; CAP_UNBOUNDED_INPUT stays unset;
   `CRISP_AUDIO_WINDOWED_ATTN=1` remains the documented escape hatch for
   >10-min single-pass audio (pair with fix_loops ON). Not run: a JA
   240 s pair (box contention; verdict doesn't hinge on it).
3. **Loop metric hardening — DONE for new kernels.** The
   mega-asr-blueprint-ref kernel carries the phrase-cycle metric (it is
   what caught the "come on" 2-gram cycle unigram runs missed); any
   future loop gate must include it — unigram-only gates pass 2-gram
   degeneration. qwen3-family-rebake itself was not retrofitted (its
   remaining use would be a mega re-bake, which is moot per thread 1).
4. **User-side calls:** reply/close GitHub issue #218 (all reported symptoms
   fixed on main + HF); tag a release so binary users get the runtime half.

## Priority ordering

| Priority | Item | Effort | Status |
|---|---|---|---|
| **HIGH** | [#221 Issue #89 hardening + v0.8.8](#221-issue-89-hardening--v088-release) | Medium | 5 steps: CI regression guard (a), server-path mirror (b), Vulkan sanity (c), q4_k registry/UX (d), release (e). |
| **DONE** | CPU weight-read hardening + Mimi codec causal default | Medium | **DONE 2026-07.** Routed ~14 CPU-side weight readers through the quantized-safe `core_cpu::to_f32` (`src/core/cpu_ops.h`); unit (`test-cpu-ops-to-f32`) + live tested on Metal + CUDA. wav2vec2 conv_w left as-is (zero-copy hot path, never quantized). **Mimi codec:** `kyutai_stt` now **defaults to causal+sliding-window** — a >250-frame WER A/B (3× jfk ≈412 frames) showed the old full non-causal attention truncates long audio ~25%; opt out with **`CRISPASR_MIMI_NONCAUSAL=1`**. `csm_tts` **also defaults to causal** (opt out `CRISPASR_MIMI_NONCAUSAL=1`) after a TTS→ASR A/B (~256 dec frames) gave causal 9.3% vs non-causal 12.0% WER (a modest single-sample win — non-causal TTS stayed intelligible rather than truncating like STT). → HISTORY, LEARNINGS. |
| **DONE / LOW** (audited 2026-07-12) | [§176 Runtime optimization pass](#176-runtime-optimization-pass--2026-06-20-audit) | Phased | **Cluster audit 2026-07-12: 18/20 done; the 2 remaining are low-value (measure-first).** §176k shipped (matvec cache), §176n/§245 were stale-but-already-done (reclassified), §176c Dia measured ~1.2% → deferred, §176l needs a model to validate. Do not treat §176 as HIGH anymore. 20 sub-items (§176a–§176t). **16 DONE or MOSTLY DONE**: §176a (flash-attn via core_attn), §176b (bucket cache 8 backends), §176d (BLAS 9 backends), §176e (context cache all support runtimes), §176f (mel BLAS+OMP), §176g (embd cache 3 backends), §176h (F5-TTS fused graph), §176i (cross-KV F16, 5 backends), §176j (iterative FFT), §176m (nemotron memmove), §176o (embed fast path), §176p (MOSS flash), §176q (greedy alloc), §176r (beam top-K), §176s (encoder cache 16/17), §176t (weight pre-cache). **2 OPEN (both low-value/measure-first)**: §176c (device-resident KV — **Dia measured ~1.2% of decode, DEFERRED**; compute-bound decoders make this a <2% win, not the "dominant bottleneck" originally claimed), §176l (Kyutai RVQ — genuinely scalar, but no local model to validate). **§176k (FireRed) MOSTLY DONE 2026-07-12** — profiling debunked the "KV/flash" framing (decode is dispatch-bound, not attention-bound); shipped an env-gated persistent matvec graph cache (`CRISPASR_FIRERED_MATVEC_CACHE`, default ON, bit-identical, pure Pareto). **§176n (VoxCPM2 Metal) DONE 2026-07-12** — stale entry: Metal already works via `VOXCPM2_USE_GRAPH` fused graphs, verified 3.75× on M1 with correct ASR roundtrip; **CUDA validated PASS 2026-07-12** (Kaggle P100/T4 mirror-path roundtrip). |
| **MEDIUM** | [#52 Qwen3-TTS](#52-qwen3-tts) — perf pass | Medium | talker + code_predictor + codec + ECAPA + codec_encoder all done; step-4 perf pass open (~137 ms/frame → real-time). **O15 broken on CUDA and default-OFF** (`61c42bfb`) — main perf lever disabled. **2026-06-13 Kaggle P100:** dedicated-sched fix (`baef21aa`) didn't help — O15=ON still rc=-6 SIGABRT at 6.0s. Crash is on the *first* code_pred call (not cached reuse), so root cause is `ggml_set_rows`-based KV scatter or the fixed-Lk causal mask on CUDA, not sched sharing. Baseline O15=OFF: 27.4 ms/frame, WAV OK. |
| **HIGH** | [#57 Commercial-friendly TTS expansion](#57-commercial-friendly-tts-backend-expansion) | Phased | Phases 1–3 + Turbo + native voice cloning shipped (→ HISTORY §82). **#83 S3Gen production fix LANDED** — UNet weight-residency split + `parallel=true` sched cache-coherency fix; M1 Metal diff cos_min 0.940→0.999976, intelligible at all T. **Remaining:** Kartoffelbox_Turbo DE. → see HISTORY + upstream-prs/09–11. |
| **MEDIUM** | [#51c MiMo-V2.5-ASR F16 step decode](#51c-f16-step-decode) | Small | F16 step-decode validation blocked behind ≥32 GB box (see PLAN #51c); base runtime + Q4_K shipped → HISTORY §56 |
| **DONE** | [#56 Kokoro multilingual phonemizer](#56-kokoro-multilingual-phonemizer-espeak-ng) | Small | espeak-ng + DE backbone shipped; Mandarin tone strip done; JA kanji g2p DONE (`1e7755e5`) — MeCab via dlopen (BSD-3-Clause, MIT-clean). → see HISTORY |
| **MOSTLY DONE** | [#58 MOSS-Audio-4B-Instruct](#58-moss-audio-4b-instruct) | Large | Runtime + GGUFs shipped, diff cos≥0.999, Kaggle P100 CUDA PASS. **Remaining:** flash-attn encoder, sweep transcript-extraction fix. → see HISTORY |
| **DONE** | [#59 Cross-binding C-ABI parity](#59-cross-binding-c-abi-parity) | Medium | **DONE 2026-06-04.** All 7 bindings at 100% C-ABI parity (149/149 symbols, `0b64a6d7` + `4835a241`). → see HISTORY |
| **DONE** | [#104 Stateful TDT frame-streaming](#104-stateful-frame-streaming-tdt-decode-for-parakeet-long-form-issue-89) | M-L | **DONE 2026-05-23.** Global z-norm + chunked encode + single decode → 99.5 % (was 59.7 %). **§216 2026-06-21: the streamed path actually COLLAPSES on v3/multilingual (5 min→75 w); non-JA now default to NeMo-exact single-pass (100 %) + silence-split longform; `streamed` kept as JA path + `CRISPASR_PARAKEET_*` gates.** **2026-07-04 issue #89 CLOSED-OUT: JA default is now VAD/energy slices capped at 12 s + per-slice single-pass + GAP-FILL second pass (re-transcribe ≥1 s empty spans in isolation) — 97.2/96.9/95.9 % phonetic recall on the reporter's 60/120/300 s clips (= inter-model ceiling; SenseVoice scores the same), vs 51 % for NeMo's best long-form mode on the same audio. Session ABI mirrored (energy slices, same gates). `streamed` remains the `CRISPASR_PARAKEET_VAD_SLICE_CAP=0` fallback.** → see HISTORY |
| **PARKED** | [#9 Parakeet TDT GPU](#9-parakeet-tdt-decoder-gpu) | Medium | Encoder 85%+ of time; LSTM+joint <0.7s; sequential steps limit GPU benefit |
| **DONE** | [#42 VibeVoice-ASR 7B](#42-vibevoice-asr-7b) | High | **DONE.** GGUFs at `cstr/VibeVoice-7B-GGUF` (Q3_K 4.7 GB – F16 17.4 GB). → see HISTORY |
| **DONE** | [#43 Fun-ASR-Nano](#43-fun-asr-nano) | Medium | **DONE 2026-05-20.** Full LLM-decoder runtime shipped; GGUFs at `cstr/funasr-{nano,mlt-nano}-GGUF`; byte-identical diffs; ~9× RT on M1 Metal. → see HISTORY |
| **DONE** | [#80 nano-cohere-transcribe-inspired tweaks](#80-nano-cohere-transcribe-inspired-perf--chunking-tweaks) | Small | 80a parked; **80b DONE**; **80c DONE**; **80d DONE** 2026-05-23 (audit: no fixes needed — all backends use energy chunker); 80e low-priority warmup deferred → see HISTORY |
| **DONE** | [#81 Nemotron-Speech-Streaming-EN-0.6B](#81-nemotron-speech-streaming-en-06b--first-cache-aware-streaming-native-asr) | M-L | **DONE 2026-06-15.** All 4 streaming presets working. → see HISTORY |
| **DONE** | [#157 Add streaming capabilities for other runtimes](#157-add-streaming-capabilities-for-other-runtimes) | — | Granite + Voxtral4b done 2026-06-19; greedy_decode.h got dual-hook run_with_probs_cb. → see HISTORY |
| **MOSTLY DONE** | [§221 TADA encoder `--make-ref`](#221-tada-encoder---make-ref) | Medium | C++ encoder runtime + GGUF converters + diff harness shipped. GGUFs at `cstr/tada-encoder-GGUF`. **WIP:** cos_mean=0.94 parity (F16 precision), C++ BPE tokenizer for end-to-end `--make-ref`. → see HISTORY §221 |
| **DONE** | [#172 Wyoming protocol server — Home Assistant Assist](#172-wyoming-protocol-server--home-assistant-assist) | Small | **DONE 2026-06-19.** `--wyoming-port N` starts a Wyoming JSONL/TCP server alongside HTTP. → see HISTORY |
| **DONE** | [#173 `--tts-play` / `--tts-play-device`](#173-tts-play--tts-play-device--local-speaker-output) | Small | **DONE 2026-06-19.** `crispasr_speaker.{h,cpp}` wraps miniaudio playback; pre-resamples to hardware-native rate (avoids 4× upsampler artefacts). → see HISTORY |
| **DONE** | [#158 transcribe_streaming for opaque-C-library backends](#158-transcribe_streaming-for-opaque-c-library-backends) | Medium | **DONE 2026-06-19.** All 7 backends done: `_transcribe_cb` C entry points added to moss_audio, gemma4_e2b, moonshine_streaming, kyutai_stt, mimo_asr, nemotron; GLM-ASR uses exported step APIs directly. → see HISTORY |
| **DONE** | [#86 Per-backend flash-attention wiring](#86-per-backend-flash-attention-wiring-crisperweaver-driven) | — | All backends now route through core helpers (`core_attn`, `core_sanm`, `core_conformer`) that unconditionally use `ggml_flash_attn_ext`. → see HISTORY |
| **DONE** | [#87 `gpu_backend` runtime selector](#87-gpu_backend-runtime-selector-multi-backend-ggml-build) | — | **DONE 2026-07-02.** `src/core/gpu_backend_pref.h` + `crispasr_init_gpu_backend()` replaces `ggml_backend_init_best()` in all 60+ backends. Process-global preference, C API `crispasr_set_gpu_backend()`. Issue #214 fix. |
| **LOW** | [#95 IndexTTS Chinese TN binary alternative](#95-indextts-15-chinese-tn--binary-alternative-to-the-python-wetext-hook) | survey only | Python `INDEXTTS_TEXT_NORMALIZER` hook shipped 2026-05-19. Hand-roll (#95a) is the right next step *when* a user reports a digit/date prompt that breaks; OpenFST vendoring (#95b) only after #95a grows past ~5 cases. |
| **IN PROGRESS** | [#97 More Parakeet variants](#97-more-parakeet-variants) | Small per-variant | TDT/TDT+CTC DONE; **parakeet-rnnt 0.6b+1.1b DONE 2026-05-24**. **parakeet-unified-en-0.6b surveyed 2026-06-13:** Unified-FastConformer-RNNT (24L, 600M), jointly trained offline+streaming with shared params. NOT converter-only — 8× subsampling (vs 4×) + Dynamic Chunked Convolutions are new. ~80% overlap with #81 nemotron streaming work. Offline mode may work through existing converter (standard bidir FC + RNNT). realtime-EOU blocked on #81 cache-aware streaming. |
| **DONE** | [#98 Hotwords / contextual biasing](#98-hotwords--contextual-biasing) | Phased | **Phase A+B DONE.** CTC-WS Aho-Corasick trie wired into parakeet CTC + TDT; LLM prompt injection for qwen3-asr + voxtral. → see HISTORY |
| **DONE** | [#110 Global diarization timeline](#110-global-diarization-timeline) | Medium | Sherpa/ecapa now runs once on the full audio (not per-slice). → see HISTORY |
| **LOW** | [#106 TEN-VAD](#106-ten-vad--low-latency-cross-platform-vad) | Small | Technically feasible VAD backend: C-compatible, 16 kHz / 10-16 ms frames, prebuilt libs + ONNX path. License is the gate: Apache 2.0 plus extra no-compete / own-app-only conditions from Agora. |
| **MOSTLY DONE** | [#114 Long-form transcribe — chunking-default ladder for voxtral / cohere / canary](#114-long-form-transcribe--make-chunkingstreamed-the-default-for-all-asr-backends-issue-89-follow-up) | Medium | Chunking/streamed-default shipped for all ASR backends (parakeet/voxtral/cohere/canary/gemma4/mimo); per-chunk AED re-injection + LCS dedup + word-snap. **Remaining:** EN-FLEURS retokenization artifacts (out of scope). → see HISTORY |
| **DONE** | [#105 WhisperX word alignment models](#105-whisperx-word-alignment-models-wav2vec2-ctc-zoo) | Phased | **DONE 2026-05-23.** All 10 WhisperX common languages (fr/es/it/ja/zh/nl/uk/pt/ar/cs) converted, uploaded to `cstr/*-GGUF`, registry aliases wired. → see HISTORY |
| **DONE** | [#115 mimo-asr baseline broken](#115-mimo-asr-baseline-broken-silent-empty-on-short-segfault-on-long) | Small-Medium | **GPU is the default** (Option B: split-load + prefill-graph decode). → see HISTORY |
| **MOSTLY DONE** | [#125 Issue #125 — multi-backend bug sweep from montvid](#125-issue-125--multi-backend-bug-sweep-from-montvid-12-findings) | Medium | 12 montvid findings; P1–P6b all DONE (`f72d3db1`/`72b74486`/`5f0aefc0`/`8bfaff23`/`b936b488`/`ba0e388e`/`043b3ae5`). **Remaining:** P0 Blackwell retest; mimo-asr `-np` empty-transcript retest. → see HISTORY |
| **DONE (no-flip)** | §208 Chatterbox S3Gen CFM single-GPU raw-gallocr cached path | Medium | **DONE 2026-06-21 — correct, but a perf DUD, default OFF.** `CRISPASR_S3GEN_UNET_GALLOCR=1` runs the batch-2 CFG UNet on one GPU backend via raw `ggml_gallocr`, caching graph+alloc across all Euler steps (legal because `ggml_gallocr_alloc_graph` doesn't mutate the graph, unlike the sched). Parity vs legacy: step-wise `x_rms` match (Δ≤1e-4), spectral corr 0.999105, identical ASR; Bug B does not recur (no cross-backend copy). **No speedup:** instrumented host build+`sched_alloc` = ~4–7 ms/step of ~1887 ms/step (0.3%) — the CFM is compute-bound (Metal GEMM), not overhead-bound as the §207 handover assumed. Shipped env-gated, default OFF (don't flip without a real speedup); kept as the clean single-backend reference + bisection gate. → HISTORY §208, LEARNINGS §208. |
| **DONE** | §205 Chatterbox full-sweep CUDA crash + q8/Metal CFM NaN | Medium | **DONE 2026-06-21.** Kaggle P100 full-backend-sweep crashed chatterbox with `free(): corrupted unsorted chunks` → non-power-of-two FFT heap overflow in the VE/S3Tok/CAMPPlus/CosyVoice3 mel (`fft_radix2_wrapper` on N=400/1920); fixed with a mixed-radix DFT (pow2 callers bit-identical). Then the q8 s3gen CFM NaN'd on M1 Metal — properly diagnosed (NOT "compound F16 accumulation"): Metal's q8 mat-vec kernel requantises the CFM activations to q8, single-pass NaN at Euler step 1 on the batch=2 CFG graph. Fix: dequantise the quantized CFM (`s3.fd.*`) weights to F16 at load, GPU-resident (correct `mul_mm_f16_f32_hp` path, full GPU speed; `CRISPASR_S3GEN_UNET_CPU=1` for the slower CPU route). ASan-clean + ASR roundtrip OK; quality identical to CPU route (`conv_pre rms` 3.380 vs 3.379). **Open:** Kaggle CUDA re-run to confirm q8 chatterbox now passes the sweep (the Metal CFM handling is M1-only; the FFT fix alone should suffice on CUDA). → see HISTORY §205, LEARNINGS §205. |
| **DONE** | [§130 Zonos TTS](#130-zonos-tts--transformer--dac-codec-apache-20) | Medium | **DONE 2026-06-09.** End-to-end synthesis + ASR roundtrip verified. → see HISTORY |
| **DONE** | [§131 OuteTTS](#131-outetts--llm--wavtokenizer-codec-cc-by-40) | S-M | **WORKING — speech output confirmed via ASR roundtrip.** WavTokenizer decoder validated cos≥0.999 all stages. → see HISTORY |
| **DONE** | [§139 Beam search — remaining ASR backends](#139-beam-search--remaining-asr-backends-issue-136-follow-up) | Phased | **18/24 done** (was 10). All feasible backends shipped 2026-06-01/02. → see HISTORY |
| **DONE** | [#156 Permissive G2P phonemizer (replace espeak-ng GPL dep)](#156-permissive-g2p-phonemizer) | Phased | **DONE 2026-06-08**: Pre-generated IPA pronunciation dicts (EN 126K, DE 667K, FR 257K, ES 600K) at cstr/g2p-dicts — 99.5% piper-compatible. → see HISTORY |
| **DONE** | [#216 Kokoro G2P technical token regression](#216-kokoro-g2p-technical-token-regression) | Small | **DONE 2026-07-03**: `C++`→`k` fixed — text normalization expands ~40 technical tokens before tokenization. `CRISPASR_KOKORO_G2P` env var for strategy selection. → see LEARNINGS |
| **DONE** | [#155 CONV_TRANSPOSE_1D GPU optimization](#155-conv_transpose_1d-gpu-optimization-issue-155) | Small | **DONE 2026-06-10.** Crash fixed (`f8fc8b8e`); CUDA/HIP 9× speedup (1200→130 ms, `5f600f25`); Vulkan `col2im_1d` kernel ported (`cad7fbac`) — codec stays fully on-GPU. → see HISTORY |
| **DONE** | [§WASM Browser build](#wasm-browser-build--all-backends-multithreaded) | Medium | **DONE 2026-06-10.** All backends, multithreaded (`-pthread` + `PTHREAD_POOL_SIZE=8`). → see HISTORY |
| **LOW** | [#127 Coverage gaps from 2026-05-26 sweep close-out](#127-coverage-gaps-from-the-2026-05-26-overlap-save-sweep-close-out) | Small | Three loose ends: **(a) omniasr-llm DONE** — Kaggle P100 sweep v4: short=106 long=490 (4.6x scaling), correct JFK transcript, no timeout (was M1-only issue). **(b)** mimo-asr local test doesn't run in CI (4.2 GB Q4_K doesn't fit runner disk). **(c) cohere-asr-ja DONE** — correct repo is `CKHO/cohere-asr-ja-GGUF` (not `cstr/`); Kaggle P100: rc=0, 108 chars, perfect JFK: "And so, my fellow Americans, ask not what your country can do for you, ask what you can do for your country." Still needs JA fixture sweep for PERFORMANCE.md table. |

**Recently completed:** many items shipped (parakeet streamed-default, #81 FA per-head mask, #110 diarization, #98 hotwords, paraformer-zh, SenseVoice, Fun-ASR-Nano, …) — full write-ups in HISTORY.md.

**Open follow-ups from §79 — we want all of these:**
- **#73 cohere long-form rerun.** flash_attn_ext is shipped on canary + cohere (commit 193a736). JFK (~11 s) numbers: canary q8_0/q4_0 -17 % under flash (win), but cohere q8_0/q4_0 is +11 % under flash vs cast-on-read on the same workload. F16 is a tie on both. Before promoting flash as cohere's recommended path, validate on a multi-minute clip — if the crossover is workload-dependent the docs need to recommend cast-on-read for short audio and flash for long. Until then PERFORMANCE.md notes flash as available-but-regresses-on-JFK for cohere.
- **#72 Linux/CUDA validation — DONE.** **2026-06-13 Kaggle P100: gemma4-e2b GPU=5.1s vs CPU=10.8s → 2.12× speedup** (JFK 11s Q4_K, sweep v2 `baef21aa`). Confirms Metal wins translate to CUDA with even larger margin. mimo-asr CUDA also PASS (rc=0, 1.1s JFK + long audio OK).
- **encoder-decoder #69a** (canary, cohere, kyutai-stt). Cross-attention layout has no `<prefix><N>.*` block-tagged tensors; needs bespoke per-backend predicates. Own design problem.

**Issue #81 A1000 work — Phase 1 verdict in (2026-05-23):**
- `d758fe69` (fused `GGML_OP_NORM_AFFINE` + `GGML_GLU_OP_SIGLU` for FastConformer encoder) closes target (b) of the gap analysis. Measured **+5.7 % wallclock win** on A1000 Laptop (2.701 s vs 2.863 s baseline, p50/chunk 175.8 ms vs 184.8 ms, RTx 22.2× vs 21.0× — clean WDDM-warm conditions on Studio Driver 596.36). Sched-debug: CPU splits 144→72, UNARY-on-CPU 72→0. **Carry as permanent improvement.** Full write-up in PERFORMANCE.md "Phase 1 update (2026-05-23)" subsection. WIP branch `issue81-phase1-uar-wip` (commits `6a0ccc67 / a2999cf3 / 6d7872a0`) is superseded — delete when convenient.
- **#06 FA per-head mask** is the next concrete A1000 perf step. Removes the other 72 CPU splits per chunk (per-head additive mask in `fattn.cu:423` + the four kernel variants). Scoped at 2-3 days, ~300-500 LOC across `fattn.cu` / `fattn-common.cuh` / `fattn-mma-f16.cuh` (and optionally `-wmma-f16.cu` / `-tile.cu` / `-vec.cuh`). Expected wallclock gain ~10-15 % on top of postsiglu (target ~2.4 s long-clip / RTx ~25× / ~1.5× behind onnx-fp32). Don't start until WDDM-warm bench protocol below is followed for the new baseline.
- **WDDM warm-up protocol** (Windows/laptop NVIDIA only): cold A1000 sits at P5/P8/210-510 MHz during compute and runs 8-10× slower than warm; engage WDDM by running `bench-issue81/probe_postsiglu_leak.py <dll> 200` (or ~10 s of `gpu_keepalive.py`) BEFORE measuring. The 3.063 s May 11 reference is reproducible with this protocol; single-shot cold benches are noise. Documented in PERFORMANCE.md "What we learned about A1000 WDDM behavior" + LEARNINGS.md "WDDM idle-clock-state hysteresis on consumer/laptop NVIDIA SKUs".

**A1000 2026-06-12 follow-ups — DONE:** firered AED beam batching (4.48×, `88ad4b9d`) + parakeet-ja auto-VAD (`f950bd1e`). → see HISTORY.

---

## §214 follow-up — chatterbox T3 batched-CFG (B=2) — deliver the win (OPEN)

§214 shipped the batched-CFG B=2 T3 decode (`CRISPASR_CHATTERBOX_T3_CFG_B2=1`,
default OFF) — greedy-token bit-identical to legacy on CPU (all quants), GPU+F16,
and GPU+quant (via F16 dequant). It works; what's left is *delivering* the
speedup and proving it. Files: `src/chatterbox.cpp` (`build_graph_t3_kv_b2`,
`run_t3_kv_b2`, `ensure_t3_b2_f16_weights`, decode loop ~§214). See HISTORY +
PERFORMANCE §214.

1. **Quiet-machine A/B + default-flip decision (HIGH).** The CPU floor ~34 %
   (75→50 ms/tok) was measured on a contended M1 (load 8–12) — unreliable.
   Re-measure on a quiet host (alternating order, min-of-N, deterministic
   token-parity gate `CRISPASR_CHATTERBOX_TEMP=0`). If the win holds, propose
   flipping the default ON for the CFG path (keep the env + legacy path forever
   for bisection). ~1 h on a quiet box.

2. **Cached / bucketed B=2 step graph — CLOSED, confirmed DUD (measured 2026-06-21).**
   `CHATTERBOX_BENCH_B2=1` split each step into build+alloc vs compute: **CPU
   0.41 ms/step build+alloc = 0.8 %** (compute 53 ms/step); **GPU+F16 1.55 ms =
   0.7 %** (compute 232 ms/step). Per-step rebuild+alloc is <1 % on both backends
   — bucketing it could save at most ~1 %, exactly the §208 lesson. NOT worth it,
   and a cached B2 graph would risk reintroducing the §186 Lk-bucket `buffer is
   nil` Metal crash that per-step rebuild currently sidesteps. Don't pursue. (Side
   datum: GPU per-step compute 232 ms ≫ CPU 53 ms, so B2 on GPU is *not* a speed
   win over the CPU default — its value is enabling T3-on-GPU at all + the
   GPU+quant F16-dequant path, not beating CPU.)

3. **GPU speedup (LOW — first data says GPU loses to CPU).** First measurement
   (2026-06-21, residual load): GPU+F16 B2 compute **232 ms/step** vs CPU **53
   ms/step** — GPU stays ~4× slower per step (the long-standing Metal
   kernel-launch-overhead × 30L × steps story; B2 doesn't change it). So GPU B2's
   value is *enabling* T3-on-GPU (sidesteps the §186 bucket crash) + the GPU+quant
   F16-dequant path, NOT speed. A quiet-box re-measure could refine the 4× but is
   unlikely to flip it; the production T3 default stays CPU.

4. **Generalize B=2 to the other CFG backends — see §215.**


## §215 — batched-CFG (B=2) for the remaining TTS backends (OPEN)

**⚠ STATE AUDIT 2026-07-11 (code-verified — the list below predates several commits):**
- **§215a dia — DONE.** `src/dia_tts.cpp` decoder already runs B=2 (output batch dim 2,
  index 0=cond/1=uncond; single KV cache sized `*2`, line ~563) since `d63b0774`. Not 2-pass.
- **§215b tada — FM done; TALKER B=2 is a MEASURED NON-GOAL (2026-07-11).** The FM
  velocity is already B=2 (gated `CRISPASR_TADA_FM_B2`). The talker AR loop was the
  candidate remaining work, but STEP-0 measurement (see the §215b note below) shows
  batched-CFG does **not** apply cleanly: the two CFG passes take **different graph
  paths** (pos through the §176b bucket, attention padded to Lk≥512; neg exact-Lk),
  so they are 3.3× asymmetric and the "fuse two equal passes" premise fails. Talker
  is only ~40% of per-step wall time; FM+codec dominate ~60%. The real talker lever
  is orthogonal & simpler (bucket-floor tuning, not B=2). Do not port.
- **§215c zonos / §215d voxcpm2 — confirmed still 2-pass** (candidates).
- **§215e f5 — SKIP.** DiT runs on `backend_cpu` (no per-dispatch-latency win — voxtral §93
  measured batched-CFG as a Metal/CUDA-*dispatch*-bound win, ~1.2×, worthless on CPU), plus the
  §176h B=2 corruption. Non-goal unless f5 moves to GPU.
- **General caveat (voxtral §93 lesson):** batched-CFG is a MODEST (~1.2–1.3×), GPU-dispatch-
  bound win. Before each port confirm the stage is GPU-run AND dispatch-bound AND the dominant
  cost — measure, don't assume the T3 −42% transfers. seq-concat + block-diagonal-mask (voxtral
  FM) is for fixed-seq no-KV DiTs; AR-with-KV backends use the chatterbox-T3 B=2 pattern.

Full-tree audit (§214, see PERFORMANCE.md): only **s3gen CFM** and **chatterbox
T3** batch cond+uncond into one B=2 forward; every other CFG backend runs **two
sequential B=1 passes** and blends post-hoc — correct, but it reads the full
weight set twice and doubles the dispatch count per step. Where the per-step
forward is dispatch/bandwidth-bound *and* the step count is high, B=2 reads each
weight once (gianni measured −42 % on T3). Apply the **same pattern** to each:
batch over `ne[2]=2` for the heavy GEMMs, split any per-batch attention/KV-cache,
tag `GGML_PREC_F32`, gate behind an env (default OFF), validate with a greedy
token/output-parity gate vs the legacy sequential path, and on **GPU + quantized
weights dequant the batched-against weights q*→F16 GPU-resident** (the s3gen
`dequant_cfm_f16` / T3 `ensure_t3_b2_f16_weights` trick — Metal's batched `ne[2]=2`
quant mat-vec misses the PREC_F32 `mul_mv_q*_K` exact-dot kernel and degenerates).

Prioritized by expected payoff (high step count × dispatch-bound first):

1. **§215a dia (HIGH).** `src/dia_tts.cpp run_dia_synth` already builds a B=2
   *encoder*; the **decoder** AR loop still runs cond/uncond as two passes with
   two KV caches (`run_dia_decode_step`). Mirror chatterbox T3: B=2 decode-step
   graph, split per-batch KV write/read, F16-dequant on GPU+quant. Largest payoff
   — long AR token loop with CFG every step.
2. **§215b tada — MEASURED NON-GOAL (2026-07-11, do NOT port).** `src/tada_tts.cpp`
   runs the talker twice per step (`run_talker_kv` for pos + `kv_neg_*` for neg).
   STEP-0 instrumented both passes (env `CRISPASR_TADA_TALKER_TIMING=1`; timing code
   is env-gated, default off, kept for re-measure) on Metal / tada-1b q4_k. **Numbers
   are load-contaminated (session box ran loadavg 31→137); the DECISION rests on the
   load-robust per-pass RATIO + the exact-Lk A/B, not absolute ms.** Findings:
   - The two CFG passes take **different graph paths**: pos hits the §176b **bucket**
     (attention padded to Lk≥512 via `kBucketLks={512,1024,2048,4096}`); neg passes
     `kv_neg_*` → falls to the **exact-Lk** graph (`build_graph_talker_kv`). At a
     short utterance (n_past 6–59, all ≪512) pos ran **266 ms/call** vs neg **81 ms**
     — 3.3× asymmetric, `first≈steady-state` so not a warmup artifact.
   - **Mechanism confirmed by A/B** (`CRISPASR_TADA_NO_BUCKET=1`, forces pos onto the
     exact-Lk path): pos dropped **266→49 ms/call (5.4×)** and pos≈neg (49 vs 61) —
     symmetric, as expected when both use the same graph. Talker share fell 40%→24%
     of the per-step loop; whole-loop per-step also roughly halved in that run.
   - **Why batched-CFG (B=2) does not pay here:** its premise is fusing two ~equal
     dispatch-bound passes so weights read once (voxtral §93: modest ~1.2–1.3×). With
     both on exact-Lk the talker is only ~24% of the loop and the passes are ~55 ms
     symmetric bodies → B=2 saves ≤~5% of loop for the cost of F16-dequant + a
     dual-KV-split B=2 graph. FM+codec dominate (~60–76%). Not worth it.
   - **Real lever is orthogonal & simpler (candidate §215-tada-followup):** the §176b
     bucket Lk=512 floor is a net loss for SHORT generations (padded attention >
     the per-step graph-rebuild it saves). A tighter floor (add a 64/128 bucket) or
     exact-Lk for small n_past recovers ~5.4× on the pos pass — a bigger win than B=2,
     no dual-KV machinery. **Needs its own clean-box / Kaggle A/B** (guard against a
     long-utterance regression where n_past approaches the bucket size and the
     rebuild-avoidance the bucket was built for actually wins).
3. **§215c zonos (MED).** `src/zonos_tts.cpp` keeps two separate KV caches
   (`kv_k`/`kv_k_uncond`) and decodes sequentially (~lines 1740–1842). Batch the
   AR backbone B=2, split the dual-KV attention. Dual-KV CFG + (optional) random
   speaker embed must stay independent per batch.
4. **§215d voxcpm2 (MED).** `src/voxcpm2_tts.cpp` LocDiT runs `locdit_call` twice
   per ODE step (cond `mu` + zero-`mu`, ~lines 2752–2778). Diffusion (fewer steps
   than an AR loop) so lower payoff, but each LocDiT forward is heavy; B=2 over the
   DiT blocks could still help. Keep the cfg-zero-star blend per batch.
5. **§215e f5 (MED).** `src/f5_tts.cpp` runs `dit_forward` twice per CFG step
   (v_cond + v_uncond, ~lines 1563–1566). Same B=2-DiT shape as voxcpm2. Note
   §176h found a standalone B=2 DiT graph for F5 *correct* but a runtime B=2
   corrupted batch-1 (F5-runtime-specific, see [[project_ggml_batched_fused_graph_alloc_bug]])
   — so F5 needs the parity gate run especially carefully; may not be worth it.
6. **§215f cosyvoice3 (LOW — likely WON'T).** `src/cosyvoice3_tts.cpp` explicitly
   declined batching (~lines 3027–3030): the estimator is a 22-block diffusion run
   twice per step, per-call overhead small vs the forward. Only revisit if a
   profile shows the dispatch overhead actually matters; otherwise document as a
   deliberate non-goal.
7. **§215g kugelaudio (BLOCKED).** `src/kugelaudio.cpp` CFG is a TODO (cfg_scale
   read but unused, negative path not implemented). Implement sequential CFG first
   (correctness), then consider B=2.

Shared infra: factor the F16-dequant-of-matmul-weights helper (currently
duplicated in `ensure_t3_b2_f16_weights` + s3gen `dequant_cfm_f16`) into a
`core_*` helper if a third backend needs it — but only on the third consumer, per
the "don't extract single-consumer helpers" rule.

### §215b follow-up — tada talker §176b bucket floor (RESOLVED: keep opt-in, do NOT flip)

The §215b measurement above found the real tada talker lever: the §176b decode
bucket floors Lk at **512**, so a short generation (n_past ≪ 512) wastes ~500
padded/masked attention columns per step. Bypassing the bucket (exact-Lk)
dropped the positive pass **266→49 ms/call** on Metal/tada-1b q4_k.

**Implemented (opt-in, default OFF):** `kBucketLks` now
`{64,128,256,512,1024,2048,4096}`; `tada_pick_bucket` gates the eligible floor by
**`CRISPASR_TADA_BUCKET_MIN`** (default **512** = original behaviour exactly — the
small buckets are inert, never picked, never built). Set e.g. `=64` to let short
generations use a tighter Lk. Output-neutral by construction (padding masked to
`-inf`), and **PARITY PROVEN byte-identical**: default-512 vs NO_BUCKET vs
floor=64 all md5 `265b9798…` on the 5-pangram fixture. Zero regression risk.

**Why a bucket (not just NO_BUCKET):** the exact-Lk path rebuilds the graph every
step; the bucket caches + reuses it. A tight-floor bucket should be the best of
both — tight padding AND no per-step rebuild — so it should beat both the
512-floor bucket and NO_BUCKET. That is the hypothesis to confirm.

**RESOLVED on clean CUDA (P100), 2026-07-11 — DECISION: keep opt-in, do NOT flip
the default.** Kaggle kernel `chr1str/crispasr-tada-bucket-ab`
(`tools/kaggle/tada-bucket-ab/`) ran the 3-way A/B (default / floor64 / nobucket)
× short(17 steps)/long(174 steps), REPS=3, on tada-1b q4_k. Two findings kill the
default-flip:

1. **The big Metal win does NOT transfer to CUDA.** The 266→49 ms/call (5.4×) pos
   penalty above is **Metal-specific** — masked attention over Lk=512 padding is
   expensive on M1 but cheap on a GPU. On P100 pos_ss is only 9.37 (default) vs
   6.56 (nobucket) = 1.43×, and at LOOP level floor64 is just **1.058× (short),
   1.016× (long)** vs default — marginal (LEARNING 34/35: a Metal win doesn't
   generalise; measure the other platform before flipping).
2. **The tighter bucket is NOT byte-identical on CUDA** (it IS on Metal — md5
   `265b9798…`). On P100 the md5s diverge with a *coherent, deterministic*
   signature: short `floor64 == nobucket ≠ default` (both tight/exact reductions
   agree, differ from the 512-padded one); long all-three-differ (default=512,
   floor64 crosses into the 256 bucket, nobucket exact). Cause: `soft_max_ext`
   sums the −inf-masked padding as exp→0 terms, and the GPU reduction *order* over
   Lk=512 vs a tight Lk flips a borderline FP bit → a different acoustic frame,
   AR-amplified. It's a **benign FP-reduction difference** (roundtrip ASR of the
   default output was 6/6 intelligible; the divergent output was not re-ASR'd but
   the ~2–6 % win doesn't justify it either way), NOT a logic bug — but it means
   "byte-identical AND faster", the bar to flip a GPU default, is **not met on
   CUDA**.

**Clean M1/Metal A/B (loadavg ~5, 2026-07-11) — the load-137 numbers were ~3.7×
inflated; here are the trustworthy ones.** floor64 vs default:
- short (17 steps): loop 249.3→**206.2 ms/step (1.21×)**, pos_ss 70.8→28.7 (2.46×)
- long  (114 steps): loop 249.1→**233.2 ms/step (1.07×)**, pos_ss 69.2→33.1 (2.09×)
- **byte-identical on Metal** (all arms md5-equal per input: short `a5ce6e08…`,
  long `d396e8bf…`) — re-confirmed clean.
- **"best of both" hypothesis CONFIRMED:** on long, **nobucket is SLOWER than
  default (255.7 > 249.1 ms/step)** — its per-step graph rebuild finally costs more
  than the padding it saves — while **floor64 (233.2) beats BOTH**: tight padding
  AND graph caching. Exactly the regression the §176b bucket was built to avoid,
  and the tight-floor bucket sidesteps it where raw NO_BUCKET does not.

**Decision — SHIPPED a backend-conditional default (2026-07-11).**
`tada_default_bucket_min()` sets the floor by backend: **Metal + CPU → 64**
(byte-identical win), **CUDA/ROCm/Vulkan/WebGPU → 512** (output left bit-for-bit
unchanged — reduction order makes a bucket-width change non-bit-identical there,
and the win is marginal). `CRISPASR_TADA_BUCKET_MIN` still overrides. The platform
split drove it: Metal floor64 is byte-identical AND 1.07–1.21× (bigger on short
utterances); CUDA is marginal (1.02–1.06×) AND changes output bytes → a global flip
would be wrong, so the default is conditional. Discrete-GPU backends other than the
measured CUDA (Vulkan/WebGPU) stay on 512 conservatively (unmeasured → keep original).

Validation (all gates passed): **Metal** default(64) == `BUCKET_MIN=512` == golden
`a5ce6e08` (3/3 consistent — the flip does NOT change existing Metal output, just
speeds it up); **CPU** default(64) == 512 == `68ad4ce4` byte-identical AND **1.53×
faster** (1203 vs 1843 ms/step — CPU was unmeasured before, now confirmed); **CUDA**
unchanged by construction (512 preserved, no re-run needed); roundtrip clean ("the
quick brown fox…"). NOTE: Metal runs are flaky under GPU contention (transient
no-output/`MISSING` at rc=0) — a correctness signal only when the box is quiet; the
3/3 golden match was taken at loadavg ~2–5. Kernel md5 caveat (fixed): the Kaggle
kernel now gates on ASR keyword-recall of ALL arms, md5 informational only.

---

## §210 follow-up — shape-stable bucketed decode for the remaining LLM/AR backends (CUDA-graph capture) (OPEN, CONDITIONAL)

PR #207 made **granite-speech**'s single-token LLM decode a *cached, shape-stable*
graph (fixed-`Lk` KV bucket written via `ggml_set_rows` at a runtime index, in-graph
`ggml_argmax`, fused F16 embed), which unlocks **two** wins:
- **CUDA-graph capture** — the ~1.4k-op step replays as one `cudaGraphLaunch`,
  ~9–13× on the decode (RTX 5090). Engages **automatically** in ggml-cuda for any
  capturable, shape+pointer-stable graph routed through the sched — no per-backend
  CUDA code.
- **Metal gallocr allocate-once** (`granite_dec_use_gallocr`) — reserve one
  `ggml_gallocr` against the cached graph, reuse every step (skip per-step
  `sched_reset`+`sched_alloc_graph`).

The question: which other LLM/AR-decoder backends would benefit? Survey done
2026-06-30 (see [[LEARNINGS]] §210; the gating property is shape-stability —
`fixed-KV bucket` vs growing `Lk = n_past + T`).

### ⚠️ READ FIRST — the honest cost/benefit (don't mass-port)

1. **The headline (CUDA-graph) win is gated to Ampere+ (sm_80+)** — see
   `ggml/src/ggml-cuda/ggml-cuda.cu:4329` (`< GGML_CUDA_CC_AMPERE` →
   `disable_due_to_gpu_arch`). The project's usual CUDA test GPUs — **T4 (sm_75),
   P100 (sm_60) — are gated OUT.** Only RTX 5090 / A100-class actually benefit.
2. **On Metal there is NO throughput win.** §210 measured the M1 granite decode at
   host-encode **1.8 %** / GPU **98 %** — it's GPU-bound (weight-bandwidth-bound
   Q4_K GEMVs), so the shape-stable rewrite buys only memory-pressure robustness
   (alloc balloon 68–236 ms → ~0) on Metal, not speed. The real Metal/Apple-Silicon
   lever is GPU-side (kernel fusion, lower-bandwidth quant), not graph replay.
3. **Each port is a MANUAL graph rewrite + byte-identical diff-harness validation**
   — not mechanical, and **NOT delegable to agents** (runtime graph code, per
   [[feedback_no_agents_for_runtime_graphs]]). Budget one focused session per
   backend.

**Therefore: port a backend ONLY when it's actually deployed on Ampere+ CUDA
servers, and measure first** (`CRISPASR_METAL_PROFILE` for the host/GPU split;
confirm the GGML_LOG_DEBUG "disabling CUDA graphs due to …" line does NOT fire on
an Ampere+ GPU). For smaller LLM decoders the host-encode fraction may be larger
than granite's 1.8 %, which would change the calculus — so don't assume, measure.

### Templates (copy these — already shape-stable)

- **`src/granite_speech.cpp`**: `granite_build_argmax_decode` (~L2474, fixed
  `bucket_len`, in-graph argmax, fused F16 embed when `token_embd` non-quant),
  `granite_dec_use_gallocr` (~L2362, gates gallocr vs sched: gallocr on Metal/CPU,
  **sched on CUDA so capture still fires**).
- **`src/mimo_asr.cpp`**: did it independently — cached `step_t1_gf` +
  `step_t1_fixed_kv_len` (L211), `set_rows` scatter-write at runtime `kv_indices`
  (L958, L1026), skip-plan reuse "update inputs only, no reset/alloc"
  (L1507–1522). Good second reference for the set_rows path.

### Already done (no work): `granite_speech`, `mimo_asr`, `dots_tts` (Metal gallocr, `ec74c5a0`).

### irodori-tts DiT — persistent fixed-shape graph (issue #243, 2026-07-12)

Simpler than the growing-KV ASR case: the RF-DiT is a **fixed-shape diffusion** — one
generation runs `run_dit_forward` ~100× (40 ODE steps × up to 4 independent-CFG
passes) and the graph shape is **constant** across all of them (T_latent/T_text/
T_ref/T_cap fixed; only input data + the attn-mask values change). The old code
rebuilt the whole graph **and a fresh `gallocr` every call**, so on Ampere+ the CUDA
graph's tensor addresses changed each step → "properties changed" → warmup
resets/re-completes **every step** (ggml-cuda.cu:4361; the reporter's "CUDA graph
warmup complete" spam). A persistent cached graph (build once, reuse, re-set ALL
inputs each call for the §234 gotcha) makes warmup complete **once** and replay.

- **Implemented**, gated `CRISPASR_IRODORI_PERSIST_GRAPH` (default OFF), + a
  `CRISPASR_IRODORI_DIT_TIMING` construct/setinput/compute split.
- **Parity PROVEN byte-identical** persist vs rebuild: CPU `6c3e16d0`, Metal
  `1c7f9f27` (default==persist on each). No hang.
- **Metal/CPU: no throughput win** — STEP-0 shows the DiT is **98.2% compute-bound**
  (graph construct+alloc only 1.7%), matching the §210 "no Metal win" rule. So the
  default stays OFF there.
- **CUDA (Ampere+) win is UNMEASURABLE on available hardware** — ggml disables CUDA
  graphs below Ampere (cc<800, ggml-cuda.cu:4329) and Kaggle only has P100 (600) /
  T4 (750), so the re-warm can't be reproduced. The reporter (Ampere+) can confirm
  warmup-once + any speedup via `CRISPASR_IRODORI_PERSIST_GRAPH=1`. Flip the default
  (or gate it to `cc>=800`) only once a real Ampere A/B shows a win.

### Candidates — growing-shape (`Lk = n_past + T`) + naive per-step rebuild + `sched_reset`/`alloc`

ASR-LLM (prioritized by likely server deployment):
1. **voxtral** — `src/voxtral.cpp`, `Lk` at L1019; per-step graph rebuild +
   `sched_reset`+`sched_alloc_graph` at L1159/L1192/L1244.
2. **qwen3_asr** — `src/qwen3_asr.cpp`, `Lk` at L1210.
3. **voxtral4b** — `src/voxtral4b.cpp`, `Lk` at L1411 (decode via
   `core_greedy_decode`).
4. **gemma4_e2b** — `src/gemma4_e2b.cpp`, `Lk` at L995 (decode via
   `core_greedy_decode::run_with_probs_cb`, ~L1631).
5. **glm_asr** — `src/glm_asr.cpp`, `Lk` at L1322.
6. **higgs_stt** — `src/higgs_stt.cpp`, `Lk` at L1061 (Qwen3-1.7B decoder).
7. **ark_asr** — `src/ark_asr.cpp`, `ark_build_decoder_graph` (L643) /
   `ark_run_decoder` (L729), `Lk` at L674 (Qwen2.5-3B decoder).
8. **lfm2_audio** — `src/lfm2_audio.cpp`, `Lk` at L933; already on a gallocr (L875,
   the §206 weight-less-first-op fix) but growing-shape, so not yet capturable.

TTS AR decoders (LOWER priority — heavy per-step compute = even more GPU-bound, so
the Metal payoff is ~nil and even the CUDA payoff is diluted by larger per-step
GPU time): `csm_tts`, `indextts`, `bark_tts`, `moss_audio`, `mini_omni2` are naive
growing-shape. `qwen3_tts`, `chatterbox` (T3), `tada_tts`, `parler_tts`,
`vibevoice` already have per-backend perf work (`set_rows`/gallocr present) — audit
individually before touching.

### Per-backend recipe (mirror granite)

1. Allocate KV at `kv_max_ctx` once; pick a fixed `bucket_len` (≤ cache cap).
2. Rewrite the step graph to a fixed `[0, bucket_len)` KV view; write the new
   token's K/V via `ggml_set_rows` at runtime index `n_past` (not a growing
   `ggml_view` sized to `n_past`). Mask is `(bucket_len, 1)`, set host-side each
   step. Topology must be byte-identical across steps.
3. Move argmax in-graph (`ggml_argmax`); keep `logits` as a graph output for
   callers needing the vocab.
4. Make the embed **capturable**: if `token_embd` is k-quant, the in-graph
   `GET_ROWS` host-syncs and disables capture — either fuse a F16 embed (granite)
   or pass a pre-computed F32 embed input (granite's `fused_embed` branch).
5. Cache the cgraph; gate gallocr-vs-sched like `granite_dec_use_gallocr` (gallocr
   on Metal/CPU, sched on CUDA/HIP so capture engages; force sched if a CPU layer
   split exists). Add an env opt-out.
6. Bound `n_past < bucket_len` (granite's OOB guard, `c5035969`).

### Validation gate (mandatory, per [[feedback_methodology]])

- **Byte-identical transcript** vs the legacy path on jfk + fleurs_60s (the granite
  gate). This is the correctness bar — no merge without it.
- **Measure** before/after: `CRISPASR_METAL_PROFILE` (host vs GPU µs split) and a
  per-step compute-µs accumulator like `CRISPASR_GRANITE_DEC_PROFILE`. On a real
  Ampere+ GPU, confirm capture engages (no "disabling CUDA graphs" debug line) and
  A/B the decode RTFx.
- M1 wall time is noise (§210) — gate on the instrumented per-step quantity, not
  end-to-end wall.

**Effort:** ~1 focused session per backend (graph rewrite + diff-harness parity +
profile). Do the highest-deployment ASR backend first; stop if its measured CUDA
A/B on Ampere+ doesn't justify the next.

---

## §219 — more permissive audio input formats for crispasr_audio_load (MOSTLY DONE)

Done (ffmpeg-free): WAV/MP3/FLAC (miniaudio), Ogg Vorbis (stb_vorbis),
Opus (libopus/opusfile), AIFF/W64/RF64 (miniaudio dr_wav, free), and AAC/M4A/
ALAC/CAF on **Apple** via AudioToolbox `ExtAudioFile`.

Items 1–3 and 5 below all **shipped** (a parallel session implemented them in
`crispasr_audio.cpp`; this PLAN entry was stale). Coverage now, all wired into
`crispasr_audio_load` + covered by `tests/test-audio-formats.cpp`:

1. **AAC/M4A on Windows + Android (DONE).** Media Foundation `IMFSourceReader`
   (`crispasr_mf_decode`) + NDK `MediaExtractor`/`MediaCodec` (`crispasr_ndk_decode`),
   plus a portable MP4 box parser (`crispasr_m4a_decode`). Linux still relies on
   the optional ffmpeg/fdk-aac dynamic fallback only.
2. **AMR-NB/WB (DONE + fetch-path fixed here).** `crispasr_amr_decode` via
   opencore-amr (Apache-2.0), CMake-gated `CRISPASR_AMR` (system pkg-config) /
   `CRISPASR_AMR_FETCH` (static FetchContent of CrispStrobe/opencore-amr v0.1.7).
   ⚠ **The `-DCRISPASR_AMR_FETCH=ON` static build was broken and untested** (the
   parallel session only had the system pkg-config path). Two `src/CMakeLists.txt`
   glob/regex bugs, both fixed + M1-validated (`jfk.amr` decodes, ratio + xcorr
   pass): (a) the `common/src/*.cpp` glob pulled in the 7 files upstream's
   `amrnb/Makefile.am` deliberately excludes (`bits2prm`/`copy`/`div_32`/`l_abs`/
   `r_fft`/`vad1`/`vad2`) — `bits2prm.cpp` #includes a header the fork doesn't
   ship → "bits2prm.h file not found"; (b) the amr-**wb** exclusion regex
   `decoder_amr_wb\.cpp$` was unanchored and also matched `dtx_decoder_amr_wb.cpp`
   (suffix), dropping the DTX decoder → undefined `_dtx_dec_amr_wb` at link.
   Both `list(FILTER)` patterns are now `/`-anchored to basenames.
3. **WebM/Matroska Opus|Vorbis (DONE).** `crispasr_webm_decode` — inline EBML
   demux → the already-linked libopus / stb_vorbis. Web/browser audio.
4. **Speex / WavPack (LOW, still open).** BSD libs; Speex is obsolete (Opus
   superseded it), WavPack niche. Only if a corpus needs them.
5. **AU / Sun .snd (DONE).** `crispasr_au_decode` — inline PCM/µ-law/A-law parser,
   zero deps.

Pattern (unchanged): permissive decoder (or OS-native) behind the miniaudio custom
backend / fallback hook, CMake-gated (pkg-config primary + FetchContent static
fallback for no-system-lib platforms), greedy decode → 16 k. ffmpeg stays an
optional dynamic fallback only for what none of the above covers (AAC-on-Linux,
WMA, AC-3/DTS, .ape). Remaining open: only item 4 (LOW).

---

## §166 follow-up — WASM `asr*` session surface needs a build-verify (MOSTLY DONE — build-verified in CI)

Round 4 (2026-06-13, see HISTORY) added a backend-agnostic ASR session surface to
the WASM/JS binding (`bindings/javascript/emscripten.cpp`:
`asrOpen`/`asrTranscribe`/`asrSet*`) — the WASM ASR path was whisper-only
(`init`/`full_default`) before. Verified by inspection (all C-ABI decls present;
mirrors the existing `tts*` Embind patterns) but **not built locally**: emsdk
won't resolve a fetchable arm64-mac SDK on this dev box (every version errors at
manifest resolution; no Homebrew emscripten either). **Open:** build via
`build-wasm.sh` / the WASM CI and smoke-test `asrTranscribe` in node/browser once
an emcc toolchain is available. The §166 native-wrapper + server + Node-addon
parity is DONE (HISTORY).

**Update (build-verify DONE):** `build-wasm.yml` now builds the binding in CI
(`build-wasm.sh --clean` across single-thread / proxy-to-pthread / webgpu), so
the asr* surface is compiled on every relevant change — a compile break in
`emscripten.cpp` fails CI. Added an explicit `grep -q "asrOpen" …libwhisper.wasm`
guard to the Verify step (mirroring the existing `ttsSynthesizeAsync` check) so
the asr* Embind exports can't silently drop out. Remaining (LOW): a runtime
`asrTranscribe`-equivalent smoke test in node/browser — a nicety, not a
build-verify; the compile+export gate is the part that was actually missing.

---

## §175 Surgical DRY — share pure helpers across the CLI/library boundary (MOSTLY DONE)

**Context.** The `CrispasrBackend` adapter layer (`examples/cli/`) and the
session C ABI (`src/crispasr_c_api.cpp`, the dylib every binding + the HTTP
server loads) are two **separate** implementations of each backend's
transcribe/synthesize. This is structural: `CrispasrBackend` and
`whisper_params` live in `examples/cli/` and `libcrispasr` (`src/`) links
nothing from there, so the session ABI *cannot* call the adapters — it
reimplements inline. contributing.md §6 already institutionalises this as a
"9 edit points" checklist. See memory `project-session-abi-reimplements-cli`.

**Scope decision (2026-06-19).** Do **not** unify the dispatch layer (hoisting
`CrispasrBackend`+`whisper_params` into `src/` and routing both consumers
through it touches all ~25 backends, the two impls have diverged — sticky
fields, voice-key caching, server error strings, best-of-N, hotwords,
punctuation toggles — and regresses CLI + 7 bindings + server; cannot promise
"no breaking changes" without a multi-week test matrix). Instead extract the
small, **pure, copy-pasted** helpers that cross the boundary cleanly as
header-only `inline` functions in `src/core/` that both sides include (no link
dependency across the boundary).

**Gate every item on before/after regression** — bit-identical output required:
unit test the pure helper + run the Kaggle `lang-spec-sweep`, `crispasr-diff`
stage harness, and `tests/regression/manifest.json` before and after.

Candidates (audited 2026-06-19):

1. **ISO-639-1 → English language-name map — DONE.** `src/core/lang_names.h`
   header-only `inline core_lang::iso_to_english()`. All 4 former copies now
   delegate: `crispasr_backend_utils.h` (`crispasr_iso_to_english_lang`),
   `crispasr_c_api.cpp` (`ca_iso_to_english_lang`), `gemma4_e2b.cpp`
   (`g4e_lang_name`). Unit test: `tests/test-core-lang-names.cpp`.

2. **GPT-2 byte-level BPE decoder — DONE (variant a).** 6 copies of the
   full 256-byte `byte_decoder()` + `decode_token()` / `gpt2_byte_decode()`
   now delegate to `core_bpe::token_bytes_to_utf8()` (already in
   `src/core/bpe.h`): `crispasr_c_api.cpp`, `lfm2_audio.cpp`,
   `vibevoice.cpp`, `crispasr_backend_{qwen3,moss_audio,mimo_asr}.cpp`.
   Unit test: `tests/test-core-bpe.cpp`. Variant (b) — the simpler
   `Ġ→space, Ċ→newline` inline replacements in glm_asr / c_api — is
   2 call sites, not worth a helper.

3. **Language-instruction prompt templates.** "Transcribe the speech in
   <lang>." now duplicated CLI↔C-ABI per backend (§174). Semantically coupled
   to each model's chat template, so lower priority / higher coupling — leave
   unless it grows; revisit if a 3rd consumer appears.

Not candidates: `crispasr_strip_ascii_punctuation` / `crispasr_lowercase_ascii`
/ `crispasr_backend_should_use_gpu` are CLI-only (already DRY via
`crispasr_backend_utils.h`; the C ABI does not duplicate them).

---

## §177 VibeVoice #171 — remaining layer: RDNA4/RADV quantized-matmul path (OPEN)

Fixed so far: server/CLI chunking divergence (§176 prefix guard), the
voice-prompt KV stride leak (server multi-request degradation — reporter
confirmed gone at 71f0639), and the quant recipe (pred./at_conn./tts_eos.
protected, b36248c1+5c8add40, all three HF repos regenerated). The reporter
retested the regenerated q8_0 (sha256 confirmed = current HF file):
**still broken, "identical result"** — the recipe was a real hardening but not
his root cause. Superseded theory: item 1's "coopmat2 flash-attn" — his device
line says `matrix cores: KHR_coopmat`, i.e. NV_coopmat2 is absent on RADV, so
`GGML_VK_DISABLE_COOPMAT2=1` was always a no-op there (his earlier "COOPMAT2=1
fixes it" observations were confounded by the then-unfixed server state leak).

Current evidence (2026-07-06):
- Failure signature: q8_0 breaks, f16 clean, same GPU (RX 9070 XT, RADV
  GFX1201, `int dot: 1`); Metal + MoltenVK clean on the exact same q8_0 file
  (fr voice, seed 42, all repro texts, ASR-roundtrip verbatim).
- The only GPU code paths a q8_0 model takes that an f16 model never touches
  are the quantized-matmul paths: int-dot MMQ / MMVQ (activations quantized to
  q8_1) and the KHR_coopmat/scalar dequant matmul variants.
- Disproof of "q8_1 activation-quant noise" as inherent cause: shipped
  `GGML_VK_FORCE_INTEGER_DOT_PRODUCT=1` (e2da0d6e, CrispASR patch in vendored
  ggml-vulkan) which enables int-dot on MoltenVK (extension present, only the
  "accelerated" bit missing). Forced int-dot + `GGML_VK_FORCE_MMVQ=1` on M1:
  clean, roundtrips verbatim. So the shader *logic* is fine; suspicion is a
  RADV GFX1201 shader miscompile (his log: "radv is not a conformant Vulkan
  implementation, testing use only" — early RDNA4 Mesa support). Vendored ggml
  base 2026-05-05; no matching upstream correctness fix found since.

**Action:** reporter runs the knob matrix one-at-a-time on a broken sample
(all env-only, no rebuild): `GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1`,
`GGML_VK_DISABLE_MMVQ=1`, `GGML_VK_DISABLE_COOPMAT=1`, `GGML_VK_DISABLE_F16=1`,
anchor `CRISPASR_N_GPU_LAYERS=0` (TTS LM layers → CPU). Also: Mesa upgrade /
AMDVLK cross-check. Once one knob is confirmed → device-targeted safe default
(RADV GFX12xx) in vendored ggml-vulkan + upstream report to ggml-org/llama.cpp
with the minimal repro. Side note for the thread: his old
`vibevoice-1.5b-bf16.gguf` predates `--include-decoder`; the current f16 on
cstr/vibevoice-1.5b-GGUF has the decoder tensors.

---

## WASM Browser build — all backends, multithreaded

### Goal

Compile CrispASR to WebAssembly so all ASR/TTS/LID/VAD/alignment
backends run in the browser. Multithreaded via `-pthread` +
`PTHREAD_POOL_SIZE=8` (requires COOP/COEP headers on the hosting page).

### Architecture

The `bindings/javascript/emscripten.cpp` exposes ~60 functions via Embind
(`--bind`): whisper ASR (`init`/`full_default`), the backend-agnostic ASR session
surface (`asrOpen`/`asrTranscribe`/`asrSet*`, added 2026-06-13 — see the §166
follow-up above), TTS synthesis (`ttsSynthesize` + the `tts*` setters), kokoro
language routing, and registry helpers.

All ~70 backend static libs link into `crispasr-lib` via the `whisper`
alias target. The JS binding links against `whisper`, pulling everything in.

### What was done

1. **`build-wasm.sh`** — Self-contained build script. Drives `emcmake cmake`
   with CPU-only flags (no CUDA/Metal/Vulkan/BLAS/OpenMP), SIMD128 optional,
   `CRISPASR_WASM=ON`. Builds the `libwhisper` target.

2. **`CMakeLists.txt`** — Added `CRISPASR_WASM` option (default ON under
   Emscripten). When enabled, `add_subdirectory(bindings/javascript)` is
   wired into the top-level build. Threading: `-pthread` in C/CXX flags
   (already present), `USE_PTHREADS=1` + `PTHREAD_POOL_SIZE=8` in the
   JS binding's link flags (already present).

3. **`crispasr_cache.cpp`** — Added `#ifdef __EMSCRIPTEN__` guards:
   - `dir()` returns `/models` (Emscripten MEMFS path)
   - `fetch()` returns false with a diagnostic (models are pre-loaded
     by JS via `FS.writeFile`)

4. **`bindings/javascript/CMakeLists.txt`** — Existing, links `whisper`
   with `--bind`, `MODULARIZE=1`, `EXPORT_NAME=whisper_factory`,
   `FORCE_FILESYSTEM=1`, `ALLOW_MEMORY_GROWTH=1`, `USE_PTHREADS=1`,
   `PTHREAD_POOL_SIZE=8`. No changes needed.

5. **`bindings/javascript/emscripten.cpp`** — Existing, ~700 lines,
   exposes the full session C-ABI. No changes needed.

### Deployment requirements

The hosting page MUST set HTTP headers:
```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

These enable `SharedArrayBuffer` which the pthread worker pool requires.
HuggingFace Spaces (Docker SDK) sets these automatically.

### Model loading flow

1. JS fetches GGUF model via `fetch()` (or from IndexedDB cache)
2. JS writes to Emscripten MEMFS: `Module.FS.writeFile('/models/model.gguf', data)`
3. JS calls `whisper_factory()` to instantiate the module
4. JS calls `Module.init('/models/model.gguf')` or `sessionTranscribe(pcm, lang)`

### Output files

- `build-wasm/bin/libwhisper.js` — Emscripten JS loader
- `build-wasm/bin/libwhisper.wasm` — WebAssembly binary
- `build-wasm/bin/libwhisper.worker.js` — Worker for pthreads

### Effort

Small — infrastructure was 90% present from upstream whisper.cpp heritage.
Only needed: `build-wasm.sh`, wiring `add_subdirectory(bindings/javascript)`
into top-level CMakeLists, and `__EMSCRIPTEN__` guards in `crispasr_cache.cpp`.

### Relation to CrispEmbed WASM

CrispEmbed's WASM build (June 2026) took the opposite threading approach:
single-threaded, no `-pthread`, no SharedArrayBuffer requirement.
This was appropriate for the math OCR decoder which runs single-threaded.
CrispASR needs multithreading for real-time ASR inference, so it uses
the full pthread path with COOP/COEP headers.

---

## 40. More Moonshine model variants

Convert + upload to HuggingFace:
- ~~`moonshine-base` (61.5M, better WER)~~ **DONE** (cstr/moonshine-base-GGUF)
- `moonshine-streaming-tiny/small/medium` — different architecture, needs new runtime
- ~~`moonshine-tiny-{ja,ar,ko,zh,vi,uk}` (multilingual)~~ **DONE** (12 repos on HF)
- ~~`moonshine-base-{ja,uk,vi,zh,ar,ko}` (multilingual)~~ **DONE** (12 repos on HF)
- ~~`moonshine-{base,tiny}-de` German fine-tunes~~ **DONE** — fidoriel (6.9%/11.4% WER, NC-SA) + dattazigzag (MIT)

Converter fix: 1D tensors (norms, biases) forced to F32; conv_1d_f32 mul_mat
argument order fixed for F16 kernels.

---

## 9. Parakeet TDT decoder GPU

Port LSTM predictor + joint head from CPU loops to ggml graphs. LSTM
is sequential → per-step kernel launches. Encoder already 85%+ of time.

**Assessment (May 2026):** JFK 11s takes 4.39s total. Encoder dominates
(~3.7s). The LSTM predictor (2×640×640) + joint head (640→8198) run
~22 steps for JFK — the CPU loops take <0.7s. The LSTM is inherently
sequential (each step depends on prev hidden state), so GPU kernel
launch overhead would eat most of the theoretical gain. On CPU, the
tight C loops are already near-optimal for these matrix sizes.

**Verdict:** PARKED. Not worth the complexity. Would only matter for
GPU inference on very long audio (100+ tokens), where the encoder
speedup from GPU is already the dominant improvement.

**Effort:** ~150 LOC. Small gain.

---

## 97. More Parakeet variants

The runtime in `src/parakeet.cpp` already dispatches TDT vs CTC via the
`has_ctc` GGUF flag (line 1669), and `models/convert-parakeet-to-gguf.py`
reads every hparam from `model_config.yaml` + cross-checks against actual
tensor shapes (line 314+). So most NVIDIA Parakeet checkpoints that share
the FastConformer-encoder + TDT-or-CTC-decoder shape should be
converter-runs with no new C++.

### Shipped — TDT / TDT+CTC (2026-05-20)

All four converted, smoke-tested on JFK at M1 Metal, and uploaded to HF
with READMEs. Registry entries + `-m <name>` lookup + C ABI surface all
wired in `crispasr_model_registry.cpp` + `crispasr_model_mgr_cli.cpp`
(fix landed on the same branch — see commit `d8325847`). One C++ fix
along the way: `parakeet_init_from_file` now auto-flips to CTC decode
when `pred_layers < 2 && has_ctc` (commit `0a902517`); without this
the 110m's single-LSTM predictor would have failed silently.

- [x] `nvidia/parakeet-tdt-0.6b-v2` → [`cstr/parakeet-tdt-0.6b-v2-GGUF`](https://huggingface.co/cstr/parakeet-tdt-0.6b-v2-GGUF) (468 MB Q4_K, 11.4× rt) — `-m parakeet-v2`
- [x] `nvidia/parakeet-tdt-1.1b` → [`cstr/parakeet-tdt-1.1b-GGUF`](https://huggingface.co/cstr/parakeet-tdt-1.1b-GGUF) (808 MB Q4_K, 16× rt, lowercase) — `-m parakeet-tdt-1.1b`
- [x] `nvidia/parakeet-tdt_ctc-110m` → [`cstr/parakeet-tdt_ctc-110m-GGUF`](https://huggingface.co/cstr/parakeet-tdt_ctc-110m-GGUF) (91 MB Q4_K, 45× rt, auto-CTC) — `-m parakeet-tdt_ctc-110m`
- [x] `nvidia/parakeet-tdt_ctc-1.1b` → [`cstr/parakeet-tdt_ctc-1.1b-GGUF`](https://huggingface.co/cstr/parakeet-tdt_ctc-1.1b-GGUF) (810 MB Q4_K, mixed-case + punct) — `-m parakeet-tdt_ctc-1.1b`

Each GGUF available in three precisions (F16, Q8_0, Q4_K). All work via:
- the unified CLI: `crispasr -m <name> --auto-download -f audio.wav`
- C ABI: `crispasr_registry_lookup_abi(<name>, ...)` returns
  filename + URL; existing init functions consume the path directly.

Filename-heuristic dispatch in `crispasr_backend.cpp:370-372`
unchanged — `parakeet-tdt_ctc-*.gguf` matches "parakeet" with the
`!contains_ci("tdt")` guard preventing accidental fc-ctc routing.

### Done — parakeet-rnnt 0.6b + 1.1b (2026-05-24)

- **`nvidia/parakeet-rnnt-0.6b`** + **`nvidia/parakeet-rnnt-1.1b`** — standard RNN-Transducer (no duration head).
  - `parakeet_rnnt_decode` in `src/parakeet.cpp` — blank→advance t by 1,
    real token→stay on same frame, `max_per_step=10` anti-loop cap; hotword
    biasing wired.
  - Converter RNNT detection: `joint.joint_net.2.weight` key detection → sets
    `n_tdt_durations=0`; runtime dispatches via `use_rnnt = !use_ctc && n_tdt_durations==0`.
  - In-memory nemo loading avoids disk extraction (BytesIO + torch.load).
  - 0.6b: 24-layer encoder, Q4_K 447 MB; `cstr/parakeet-rnnt-0.6b-GGUF`.
  - 1.1b: 42-layer encoder, Q4_K 770 MB; `cstr/parakeet-rnnt-1.1b-GGUF`.
  - Both smoke-tested on JFK (correct transcript). Registry entries added. Committed + pushed.

**Still open:**
- **`nvidia/parakeet_realtime_eou_120m-v1`** — streaming + end-of-utterance head. Needs cache-aware FastConformer streaming (cf. PLAN #81 Nemotron), plus an EOU head. Not a converter-only job.
- **`nvidia/parakeet-unified-en-0.6b`** — **surveyed 2026-06-15 via Kaggle kernel.**
  Target class: `EncDecRNNTBPEModel` (same as other parakeets). Encoder:
  `ConformerEncoder` with new `att_chunk_context_size` param (dynamic chunked
  convolutions for unified offline+streaming). Kaggle NeMo 2.7.3 lacks this
  param — needs `nemo_toolkit>=2.8` or later. Alternatively: extract weights
  directly from the .nemo zip (torch checkpoint format) + read config from the
  Hydra config embedded in `save_restore_connector`. The existing
  `convert-parakeet-to-gguf.py` should work once the model loads — same
  `EncDecRNNTBPEModel` class, same weight naming. The C++ runtime needs
  `att_chunk_context_size` support in `core/fastconformer.h` (dynamic conv
  kernel selection per chunk, similar to nemotron's streaming conv cache).
  **2026-06-16 v4 kernel — direct zip extraction WORKS.** Custom
  `NeMo2Unpickler` reads `data.pkl` + storage files from the NeMo 2.x zip
  without instantiating the NeMo model (bypasses `att_chunk_context_size`).
  989 tensors extracted with actual data. Hparams: d_model=1024, n_layers=24,
  vocab=1025, pred=640, joint=640 — **same architecture as standard parakeet**.
  **v5 kernel — GGUF conversion succeeded (1181 MB F16).** Synthetic NeMo 1.x
  tar (config + tokenizer from parakeet-rnnt + weights) fed to existing
  converter. CrispASR test SIGABRT: runtime assumes 4x subsampling but
  parakeet-unified uses 8x (3 strided convs vs 2). **Fix needed:**
  `parakeet_build_pre_encode` in `src/parakeet.cpp` (or
  `core/fastconformer.h`) must handle `subsampling_factor=8` — 3 Conv2d
  layers with strides [1,2] instead of 2. Tensor shapes are already in
  the GGUF; the graph builder just needs the extra conv layer.

### Won't do

- `parakeet-ctc-0.6b-Vietnamese` — already runtime-supported (CTC); ship
  if a Vietnamese user asks. Tracked as a known gap, not active work.

See also: [#98 Hotwords](#98-hotwords--contextual-biasing) — orthogonal
feature that lights up biasing on every Parakeet variant once the CTC-WS
trie lands.

---

## 98. Hotwords / contextual biasing

User-supplied vocabulary that the ASR should prefer when in doubt
(names, jargon, product terms, place names). Distinct from "improve
overall WER" — hotwords only help on the biased subset, but on that
subset the lift is large (e.g. NeMo CTC-WS reports F1 jumps from ~30 %
to ~80 % for OOV name spotting; FunASR SeACo-Paraformer ~58 % F1 lift
on AISHELL-NER).

### Upstream-support survey (May 2026)

| Backend | Upstream support | Mechanism |
|---|---|---|
| parakeet-tdt, -ctc, -rnnt, -tdt_ctc, -unified | YES | NeMo CTC-WS (Aho-Corasick phrase-boost trie + shallow fusion); MBS Transducer hotwords (Feb 2026) |
| fastconformer-ctc | YES | Same NeMo CTC-WS pipeline |
| funasr / fun-asr-nano / mlt-nano (paraformer) | YES | Native `hotword=` kwarg in upstream `AutoModel.generate`; SeACo-Paraformer-style hotword encoder pre-decoder |
| qwen3-asr | YES | DashScope `vocabulary_id` + free-text `-c <context>` |
| voxtral / voxtral4b | YES | `context_bias` API field, up to 100 entries |
| granite-speech-4.1-2b-plus | YES | Keyword list baked into LLM prompt (the `-plus` variant is the keyword-prompted one) |
| mimo-asr | YES (research) | PromptASR cross-attends a text-prompt encoder into the speech encoder; HF card doesn't expose a flag yet |
| whisper | partial | `initial_prompt` only — decoder text-prompt conditioning, no hard bias |
| firered-asr-llm | partial | `(prompt, speech, transcript)` triplet on the LLM variant supports free-text prompt |
| cohere-transcribe | NO | API takes model / language / file / temperature only |
| moonshine | NO | Whisper-style encoder-decoder; no `initial_prompt` either |
| kyutai-stt | NO | "Contextual accuracy" from delayed-streams modelling, no keyword list API |
| omniasr (CTC head, Meta) | NO upstream | But our CTC-WS trie would just work — model-agnostic on the logit stream |
| glm-asr | NO | Plain transformers / vLLM / SGLang inference, no documented hotword field |

### Phased implementation

**Phase A — generic CTC-WS phrase-boost trie** (covers parakeet-ctc /
parakeet-tdt / fastconformer-ctc / omniasr in one shot)

- New shared helper `src/core/asr_context_bias.{h,cpp}` —
  Aho-Corasick trie over piece-id sequences with a configurable boost
  score per matched phrase; emits a per-frame log-prob bias vector that
  the CTC / TDT decoder shallow-fuses into its argmax / beam scoring.
- Wire-in points: `parakeet_ctc_decode` and `parakeet_tdt_decode` in
  `src/parakeet.cpp:999+` and `parakeet.cpp:1670` dispatch. Phrase
  tokenisation goes through the same SentencePiece model the backend
  already loads, so users supply human-readable strings.
- CLI: `--hotwords "Acme Corp,Sandra Berenz,GPU-PB"` and/or
  `--hotwords-file <path>` (one phrase per line, optional `^N` boost
  suffix); env var `CRISPASR_HOTWORDS=...` for the OpenAI-server path.
- Estimated ~250–400 LOC including beam-search rescoring path; pure CPU,
  no ggml graph needed.
- Reference impl to mirror: NeMo CTC-WS notebook + the `BoostingTree`
  C++ in TurboBias (`arxiv.org/html/2508.07014v1`).

**Phase B — `--hotwords` → LLM prompt-prefix helper** (covers funasr,
granite-plus, voxtral, qwen3-asr)

- Tiny shared helper in `src/core/` that renders a hotword list into
  the backend's expected prompt format (each backend has a different
  template — granite uses `Keywords: …`; funasr uses a hotword token
  block; qwen3 uses free-text context). One template registry, one
  call site per backend.
- Wire-in points: `funasr_transcribe_ex` (the LLM-decoder prompt
  builder we just shipped), `granite_nle_transcribe`, voxtral,
  qwen3-asr. All four already have a "system prompt" path the helper
  plugs into.
- Estimated ~150 LOC + per-backend template strings.

**Phase C — parakeet TDT joint-net boost (Transducer-native)**

- Mirror NeMo's MBS hotwords for Transducer: add a per-step bias on the
  joint-net output when the partial hypothesis matches a prefix in the
  trie. More accurate than shallow-fusion at the cost of being
  TDT-specific (CTC path already covered by Phase A).
- Defer until Phase A is shipped + benchmarked; only worth the
  complexity if Phase A on TDT undershoots NeMo's reference numbers.

### Out of scope

- **Whisper `initial_prompt`** — already supported upstream via the
  whisper.cpp loader. If a user really wants Whisper biasing, the
  current path is to set `--initial-prompt`; no new CrispASR work.
- **MiMo PromptASR exposure** — the architecture supports it but
  upstream doesn't expose a flag and the HF card has no hotword
  example. Park until upstream ships an API.
- **Cohere / Moonshine / Kyutai-STT / GLM-ASR** — no upstream support,
  no architectural hook; would require training a side-channel which
  is outside this engine's scope.

### Validation

- Add a `tests/test_hotwords.py` that runs a synthetic clip with a
  rare name (e.g. "Berenz") through each Phase-A backend with and
  without `--hotwords Berenz`, asserts the unbiased transcript
  mispells it and the biased one nails it.
- For Phase B, point at the upstream reference Python and assert the
  prompt-prefix matches byte-for-byte.

### Effort estimate

- Phase A: 2–3 days (helper + 4 wire-ins + tests + docs).
- Phase B: 1 day (helper + 4 wire-ins + tests).
- Phase C: 1–2 days, only if Phase A undershoots on TDT.

Total: ~1 week of work covering 9 of 14 backends.

### Sources

- NeMo CTC-WS tutorial: `tutorials/asr/ASR_Context_Biasing.ipynb`
- NVIDIA word boosting docs: `docs.nvidia.com/nemo-framework/.../word_boosting.html`
- Fast Context-Biasing CTC-WS paper: `arxiv.org/html/2406.07096v1`
- TurboBias / GPU-PB: `arxiv.org/html/2508.07014v1`
- FunASR Paraformer hotword: `modelscope/FunASR examples/industrial_data_pretraining/paraformer/README.md`
- Voxtral context_bias: `docs.mistral.ai/studio-api/audio/speech_to_text`
- Qwen3-ASR-Toolkit: `QwenLM/Qwen3-ASR-Toolkit`
- Xiaomi PromptASR: `arxiv.org/pdf/2309.07414`
- icefall shallow-fusion: `k2-fsa/icefall docs/source/decoding-with-langugage-models/shallow-fusion.rst`

---

## 42. VibeVoice-ASR 7B

**DONE.** Full ASR+TTS GGUF (1205 tensors) at `cstr/VibeVoice-7B-GGUF`,
7 quantizations Q3_K (4.7 GB) through F16 (17.4 GB). Layer offload
validated on M1 Metal (ASR 28L, TTS 20L). See HISTORY for details.

---

## funasr — perf follow-ups (LOW priority, not blocking)

The 2026-05-20 funasr port ships with `ggml_flash_attn_ext` on the
encoder + adaptor (FA on by default, opt out with `FUNASR_NO_FA=1`)
and `core_attn::kv_self_attn` on the LLM body. Three opportunistic
optimisations that didn't make the first cut and are worth ~one bench
session each when somebody wants to push the numbers:

1. **Per-step LLM decode graph cache.** On JFK (T_lfr=183, 29 tokens
   decoded) the decode loop runs at 37.6 ms/token; the unfused
   memory-bound floor for F16 Qwen3-0.6B on M1 is ~6 ms/token, so
   ~30 ms is unaccounted-for graph build + sched alloc overhead.
   Pattern: build the step graph once at `funasr_kv_init` time with
   `kv_indices` runtime input (so K/V writes go to a runtime slot
   via `ggml_set_rows` instead of the default static-offset
   `ggml_cpy`) and `fixed_kv_len = kv_max_ctx` (so topology stays
   constant). Each decode step then only writes the positions /
   kv_indices / causal_mask / inputs_embeds inputs and re-runs the
   cached graph. Expected savings: 5-10 ms/tok ≈ 15-25 % of total
   decode time. Same pattern qwen3_asr could adopt.

2. **Encoder graph cache by T_lfr bucket.** At T_lfr=183 the encoder
   takes 258 ms; back-to-back calls on similar-length clips pay the
   graph build cost each time. Bucket to {128, 256, 512, 1024, 2048}
   like voxcpm2's TSLM (HISTORY 2026-05-19) — pad the inputs to the
   bucket and emit a static mask that drops the trailing rows. The
   first call to each bucket pays the build cost; everything after
   reuses it. Expected savings: 10-20 ms per call once warm.

3. **Fused LLM QKV — DONE (shipped at initial port time).** Pattern from
   `qwen3_asr.cpp`: concat Q/K/V weights at load, one matmul per layer.
   Already implemented in `funasr_init_from_file` (unconditional, no
   env-var opt-out — fused at load if Q/K/V shapes + types match).

4. **Single-token embed fast path — DONE (2026-06-20, HISTORY §180).**
   `CRISPASR_FUNASR_EMBED_FAST` (default ON). AR decode hot path called
   `funasr_embed_tokens({next_id})` once per step, paying full graph-build
   + sched-alloc overhead for a single GET_ROWS. New path dequants one
   row directly from `token_embd.weight`, skipping the graph. Transcript
   byte-identical. VPS A/B: embed step ~1.6× faster (15–26 vs 27–41
   ms/tok). On M1 Metal (~37 ms/tok total decode), this is the
   lowest-hanging fruit from item #1 above.

4. **Two-pass: CTC fast pass → Fun-ASR-Nano LLM rescore.**
   RapidAI/RapidSpeech.cpp claims a "CTC fast pass + LLM rescoring" path
   for FunASR-Nano. The state of CTC support for Fun-ASR-Nano took a
   couple of investigative passes to map cleanly; **the situation as
   of 2026-05-21**:

   - **Official upstream `FunAudioLLM/Fun-ASR-Nano-2512/model.pt`** —
     1880 MB, **1261 tensors, 0 with `ctc` in the name**. Prefix
     breakdown: `audio_encoder` (914), `llm` (311), `audio_adaptor`
     (36). Same shape for `Fun-ASR-MLT-Nano-2512/model.pt`
     (independent binary, also 0 CTC). Verified locally 2026-05-21
     against the cached HF snapshots; consistent with `840e36dd`
     "no-CTC finding".
   - **Official FunASR framework `funasr/models/fun_asr_nano/`** —
     does ship CTC code. `model.py`'s `FunASRNano.__init__` sets
     `self.ctc_decoder = None` by default and only builds a CTC
     head when `ctc_decoder` is present in `kwargs` (i.e. set in
     the training config). `ctc.py` defines the standard `CTC`
     module (single Linear `ctc_lo` + `CTCLoss`). Recipes live in
     `examples/industrial_data_pretraining/fun_asr_nano/`.
   - **`csukuangfj/funasr-nano-with-ctc`** (sherpa-onnx / k2-fsa
     maintainer, Apache-2.0) — the only public *trained* CTC head.
     Recipe in `config.yaml`: `ctc_decoder: Transformer` with
     `n_layer: 5`, `ffn_dim: 2048`, `encoder_dim: 512`, `llm_dim:
     512`; encoder/adaptor/LLM frozen, only CTC trained;
     `effective_save_name_excludes: - llm.` so the saved `model.pt`
     (599 MB) is encoder + adaptor + CTC head, no LLM.
   - **`manyeyes/Fun-ASR-Nano-2512-CTC-onnx`** (and `-int8-onnx`),
     **`Oulasong/Funasr_Nano_MLT_ONNX`**, **`jiyilin123/FunASR-CTC-Nano-INT8-ONNX`**
     — almost certainly downstream ONNX/int8 conversions of
     csukuangfj's trained CTC head. manyeyes' description text
     ("encoder与Fun-ASR-Nano-2512-CTC中的encoder一致") confirms
     they reuse the upstream frozen encoder, which is csukuangfj's
     setup.

   So upstream released a pure LLM-style ASR by choice — the
   framework supports CTC as opt-in, but the released checkpoint
   omits the head. CTC is one trained-from-scratch head away, and
   csukuangfj has already done that training under Apache-2.0.

   ### Two viable two-pass patterns

   | Source for Pass 1 | Encoder forwards | Vocab mapping | Trust |
   |---|---|---|---|
   | **csukuangfj's CTC head + upstream encoder/adaptor/LLM** | **one** (shared) | none (CTC head was trained against Qwen3 tokenizer / SANM frame rate; same tokens.txt as upstream) | single-author training, no published WER, but framework-blessed recipe |
   | **SenseVoice-Small (already in CrispASR)** | two (different encoder weights) | needed (SenseVoice's CTC vocab vs Fun-ASR-Nano's Qwen3 tokenizer differ) | gold — official Alibaba release |

   The first pattern is the cleaner architecture (single encoder
   forward, no cross-vocab) but inherits csukuangfj's training
   trust. The second is more conservative but pays an extra
   encoder pass and a vocab translation step.

   ### Phase A — measurement only (no code)

   Before writing any C++:

   - Tensor-list csukuangfj's `model.pt` to confirm his encoder
     weights are byte-identical to upstream's (they should be —
     he loaded them with `freeze: true`). Same shape, same dtype,
     same values modulo precision conversion.
   - Run csukuangfj's CTC head + upstream's encoder+adaptor on a
     small Chinese + English benchmark set we have ground-truth
     transcripts for. Measure CER/WER vs upstream's pure-LLM
     path; the framework's CTC training was auxiliary
     (`detach_ctc_decoder: true`, `ctc_weight: 1.0` as one of
     several losses), so CTC quality won't match the LLM head —
     we just need it within ~3-5% relative for rescore to be a
     net win.
   - If quality is within bounds, write up the result in
     LEARNINGS.md and proceed to Phase B.

   ### Phase B — implementation

   - Grow `models/convert-funasr-to-gguf.py` to (optionally) pick
     up `ctc_decoder.*` tensors from a separate "with-ctc"
     checkpoint, written as `funasr-nano-with-ctc-q4_k.gguf` or
     similar. Auto-download from `cstr/funasr-nano-with-ctc-GGUF`
     (we'd mirror csukuangfj's weights under our HF account, with
     attribution).
   - Opt-in flag `CRISPASR_FUNASR_TWOPASS=1` that requires the
     companion `with-ctc` GGUF to be loadable; fall back to the
     single-pass path when it's missing. New `--asr-rescore` CLI
     flag for explicit selection.
   - Wire one encoder forward, fork into CTC head + LLM head.
     Pass 1: greedy CTC → per-frame token probs. Pass 2: skip
     LLM if avg per-frame confidence >0.95; otherwise use CTC
     top-K hypothesis as decode-prefix candidates for the LLM.
     Expected savings: 2-4× on high-confidence clips, neutral on
     hard audio (still pays both heads but only one encoder).

   ### Fallback (only if Phase A's quality measurement fails)

   Use SenseVoice-Small as the fast pass. Pays two encoder
   forwards and a vocab translation, but inherits gold-source
   trust. Same opt-in flag, different model lookup.

None of these affect correctness — they're pure throughput pickings.

---

## §187 Cross-runtime embed fast path sweep (MOSTLY DONE)

Single-token embed fast path for all LLM-based AR decode runtimes.
Same pattern as funasr §180 item #4: per-step `embed_tokens({next_id})`
called in the decode loop; fast path dequants one row directly from the
token embedding weight, skipping graph-build + sched overhead.

| Backend | Env var | Status |
|---------|---------|--------|
| funasr | `CRISPASR_FUNASR_EMBED_FAST` | **DONE** §180 |
| glm_asr | `CRISPASR_GLM_ASR_EMBED_FAST` | **DONE** §187 |
| moss_audio | `CRISPASR_MOSS_AUDIO_EMBED_FAST` | **DONE** §187 |
| qwen3_asr | `CRISPASR_QWEN3_ASR_EMBED_FAST` | **DONE** §187 |
| gemma4_e2b | `CRISPASR_GEMMA4_E2B_EMBED_FAST` | **DONE** §187 (+ sqrt(d) scale) |
| outetts | — | Already had n==1 fast path at port time |
| orpheus | — | Already had n==1 fast path at port time |
| lfm2_audio | — | Already does direct row reads (no graph) |
| mini_omni2 | — | N/A — never calls embed with n=1 |
| granite_speech | — | OPEN (model too large for VPS bench) |

### Other perf candidates from bench data

1. **Deepen shallow bench stages.** vibevoice, wav2vec2-ggml, m2m100,
   t5_translate only have `_total` stages — add finer-grained stages.
2. **§175 DRY: `src/core/lang_names.h`** — 4 copies → 1 header.
3. **Encoder graph cache by bucket** — funasr, sensevoice, qwen3_asr.

---

## §201 Kaggle CUDA backend failures — full sweep 2026-06-20 (OPEN)

The first full-coverage Kaggle GPU sweep (`tools/kaggle-benchmark-all-backends.py`,
streamed to `cstr/crispasr-kaggle-progress/full-backend-sweep/`, see PERFORMANCE.md
2026-06-20) ran 59 backends. The first pass showed 10 "failures", but a follow-up
audit + a fixed-subset re-test (run tag `voicefix-retest`) found **2 were benchmark
mistakes, not bugs**, leaving **7 genuine failures** (+1 pending). Of those 7,
**fastpitch + speecht5 are now fixed** (HISTORY §204), leaving **5 open**:

**RESOLVED — were benchmark args/category, fixed in the script (commit on main):**
- ✅ **vibevoice-1.5b** — is a *TTS* model (`vibevoice-1.5b-tts-q4_k.gguf`), was
  wrongly run as ASR → empty. Moved to TTS; now PASSES (367 KB).
- ✅ **vibevoice-tts** — voice-conditioned; the bare `--tts` gave it no voice.
  With `--voice <ref.wav>` it now PASSES (255 KB).

**Pending one more confirm:**
- [ ] **f5-tts** — once given a reference voice it *runs* (was a fast-fail before),
  but **TIMEOUT at 120 s** in the re-test. Bump the smoke timeout (≥240 s) and
  re-run to settle pass-vs-stuck; passes on M1 Metal locally.

**FIXED — moved to HISTORY §204 (FastPitch + SpeechT5 GPU/CUDA):**
- ✅ **fastpitch** (TTS) + **speecht5** (TTS) — both fixed by `69dc1789`. Two
  Metal-masked HiFi-GAN bugs (sched-allocated decoder/vocoder graphs computed on
  `backend_cpu`; channel-major `convt1d_decomp` on a time-major input). Both
  validated on M1 Metal. CUDA re-test pending. Full write-up in HISTORY §204.

**RESOLVED — lfm2-audio GPU backbone fixed (HISTORY §206):**
- ✅ **lfm2-audio** (ASR) — fully fixed. The crash was embed lookups on CUDA
  (device-ptr deref, like §204). The garbage was the ROOT CAUSE found via the
  diff harness + GGML_SCHED_DEBUG (`63d4c013`): the backbone's weight-less leading
  RMSNorm made `ggml_backend_sched` put the input + first op on CPU and feed the
  GPU a miscomputed cross-backend copy. Fix: compute the backbone directly on
  ctx->backend via gallocr (single-backend, no copy) in `backbone_step` + `run_lfm`.
  GPU transcribes JFK verbatim; GPU diff matches the PyTorch ref like CPU.
  **GPU is default again**; `CRISPASR_LFM2_AUDIO_CPU=1` forces CPU. PyTorch ref +
  GPU diff (`CRISPASR_DIFF_USE_GPU=1`) shipped. Follow-up: GPU-decode graph caching
  (AR decode is currently dispatch-bound, GPU ~slower than CPU on JFK).

**RESOLVED — kugelaudio GPU empty-output (HISTORY §209):**
- ✅ **kugelaudio** (TTS) — fixed by `bdb3f42f`. Same §206 root cause: the
  Qwen2.5-7B LM's weight-less leading RMSNorm made `ggml_backend_sched` put the
  input + first op on CPU and feed the GPU a miscomputed copy → garbage LM → no
  speech-diffusion token → empty output. Fix: backbone graphs compute directly on
  ctx->backend via gallocr; only the VAE decoder stays on the sched (its `ggml_pad`
  is Metal-unsupported → CPU fallback; CUDA supports PAD so it runs on GPU there).
  Validated on M1 Metal (q4_k): GPU generates 83200 samples, ASR-roundtrips to
  "Hello there.".

**GENUINE bugs — fail on CUDA even with correct args (TODO):**
- [x] **orpheus** (TTS) — **FIXED (§215, HISTORY).** The 0-byte/SIGSEGV was the
  §176b Lk-bucket decode's dedicated step-sched: a fresh `ggml_backend_sched`'s
  first `alloc_graph` left the cross-backend input copies unbacked on GPU, so the
  first decode step bound a garbage device buffer (crash in `ggml_metal_op_norm`
  on node #0, before any set_rows). §213 was right that neither the talker AR
  decode nor SNAC was the culprit — it was the bucket's separate sched. Fix:
  run the cached bucket graph on the already-warm prefill sched `c->sched`
  (drop `ar_step_sched`) + read the KV from the `ggml_set_rows` result for the
  Metal write→read edge. GPU + CPU bucket both ASR-roundtrip verbatim on M1.
  Stays opt-in (`CRISPASR_ORPHEUS_BUCKET=1`): correct now, but ~30 % slower than
  the non-bucket path on M1 unified memory (over-read); may still win on CUDA.
  **CUDA cross-check still pending** (Kaggle `chr1str/crispasr-orpheus-talker-cuda`
  end-to-end `orpheus_synthesize`).
- [ ] **chatterbox** (TTS) — 0-byte (~14 s) with `--voice <wav> --i-have-rights`;
  the #83 S3Gen GPU fix was Metal-validated — re-check the CUDA S3Gen path.
- [ ] **cosyvoice3** (TTS) — **dies in 0.1 s** even with a reference voice; passes
  on M1 Metal. Flow-matching + HiFT — earliest/cheapest to bisect. **Note
  (2026-06-21):** §205's mixed-radix FFT fix explicitly covers CosyVoice3's
  `n_fft=400` mel (same heap overflow as chatterbox). Likely resolved — needs
  Kaggle CUDA re-test to confirm.

**Method:** per-backend JSONs in the dataset have timing context; reproduce on a
CUDA worker (Kaggle T4/P100 or the A1000) with `CRISPASR_VERBOSE=1` +
`CRISPASR_<BACKEND>_DEBUG=1`. Remaining open: orpheus, cosyvoice3 — chatterbox
fixed in §205, lfm2-audio in §206, kugelaudio in §209 (lfm2 + kugelaudio shared
the §206 sched weight-less-first-op root cause). Several
pass on M1 Metal → CUDA-path-specific; cross-check Metal first. Small models also
fit the 8 GB CPU-only VPS, where the diff harness can drive the fix if the bug
reproduces on CPU. The fastpitch+speecht5 fix (§204) came exactly this way — the
HiFi-GAN layout bug reproduced on CPU locally; lfm2-audio (§206) likewise (the
backbone diverges identically on CPU-vs-Metal diffing).

---

## CosyVoice3-0.5B-2512 TTS — DONE

_Done — see HISTORY.md + git log._

## 51c. MiMo-V2.5-ASR F16 step decode — open

Base runtime + Q4_K + fused-QKV layout shipped → HISTORY §56 + §64.
Sub-items 51a (mmap loader → HISTORY §62) and 51b (step-decode KV
cache reuse → HISTORY §60) also DONE. Only this F16 step decode is
still open — blocked behind ≥32 GB RAM for end-to-end validation.

### 51c. F16 step decode

Q4_K dequant on every matmul is the largest single cost at decode
time. F16 weights are ~2× larger but skip the dequant loop
entirely.

**Status (May 2026): code path works, validation deferred to a
larger-RAM box.**

PLAN #51a's CPU mmap loader landed (commit `9710f80`) — Metal
mmap loader landed too (same commit) — and #60a added the
`posix_madvise(WILLNEED)` readahead hint (commit `f1f4bce`).
Together these mean **no code change is needed for 51c** — just
point `crispasr` at the F16 GGUF with `CRISPASR_GGUF_MMAP=1`. We
verified the load path works (no OOM, mmap'd weights at 1.9 GB
RSS on a 16 GB box, prefill compute starts).

What we couldn't validate end-to-end on this box:

- **JFK transcript byte-equality on F16**: prefill compute
  thrashes because the 16 GB F16 working set doesn't fit in 16 GB
  RAM. Pages get evicted as compute walks layers, every
  re-access faults from the disk5 external (99% full, often
  contended by other workers). One bench attempt ran for 51 min
  with 0.1% CPU and never finished prefill.
- **Decode speedup measurement**: same root cause — needs warm
  cache, which we can't achieve.

The ceiling is **hardware, not code**: 16 GB F16 weights need
≥20 GB RAM to comfortably fit + leave headroom for activations +
KV cache + audio tokenizer. On a 32+ GB box this should "just
work" and hit the work order's ≥1× realtime target.

Files **not** touched (no code change required):
- `src/mimo_asr.cpp` — the runtime is dtype-agnostic; F16 weights
  flow through the existing `core_attn::kv_self_attn` matmul kernels
  on Metal without modification.
- `src/core/gguf_loader.cpp` — already wired (60a + #51a).

Validation deferral notes:
- Run `CRISPASR_GGUF_MMAP=1 ./build-ninja-compile/bin/crispasr --backend mimo-asr -m /path/to/mimo-asr-f16.gguf --codec-model /path/to/mimo-tokenizer-q4_k.gguf -f samples/jfk.wav` on a 32+ GB box to validate transcript + bench.
- If F16 prefill hits ≥1× realtime as predicted, ship the F16
  GGUF as the recommended quant and demote Q4_K to a memory-tight
  fallback. Until then both are shipped on `cstr/mimo-asr-GGUF`
  with Q4_K as the default.

Effort: **0 LOC** (validation only). The originally-scoped
"Effort: Small" assumed code work that turned out to be unneeded
once the mmap loader landed.

---

## 52. Qwen3-TTS

User-requested follow-on to the VibeVoice TTS work. Apache-2.0
collection: [Qwen/Qwen3-TTS](https://github.com/QwenLM/Qwen3-TTS),
[HF collection](https://huggingface.co/collections/Qwen/qwen3-tts).

- **Six repos in the collection** (all BF16 safetensors, Apache 2.0):
  - `Qwen/Qwen3-TTS-Tokenizer-12Hz` — RVQ codec, 16 codebooks × 2048,
    12.5 FPS at 24 kHz. Non-DiT lightweight architecture (8L
    encoder + 8L decoder).
  - `Qwen/Qwen3-TTS-12Hz-{0.6B,1.7B}-Base` — base talker LM with
    voice clone (3s reference audio).
  - `Qwen/Qwen3-TTS-12Hz-{0.6B,1.7B}-CustomVoice` — fine-tuned,
    fixed speakers.
  - `Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign` — instruction-tuned
    (voice description → speech).
- **Architecture:** "Discrete Multi-Codebook LM" — Qwen3 backbone
  with a 16-codebook output head. No DiT; direct AR generation of
  RVQ codes. ~97ms end-to-end latency, 10 languages incl.
  en/de/zh/ja/ko/it.
- **Status (May 2026):** **base + CustomVoice + VoiceDesign 0.6B/1.7B all live** — talker forward, ICL prefill, code-predictor sampling, codec decoder, ECAPA speaker_encoder forward, codec encoder forward all DONE. ASR roundtrip word-exact across all variants. Open: only the **performance pass** below.
- **Shipped milestones** (commit references in HISTORY §57/§58 + per-model status table under #57):
  1. ✓ Talker forward (28L Qwen3 + Q/K-norm + flash-attn + F16 KV cache) — `talker_logits` cos=1.000000 (`2b85b78`).
  2. ✓ ICL prefill builder — `talker_logits_via_icl` cos=1.000000 (`b939d4f`).
  3. ✓ Code predictor with sampling — fixed silent-output trap (`9608202`, `69c135c`).
  4. ✓ TTS→ASR roundtrip on parakeet-v3.
  5. ✓ Codec decoder (Tokenizer-12Hz) — diff harness 8/8 PASS at cos≥0.999983 (`d1f47b1`, `48c6c1a`). Required a Metal `kernel_conv_transpose_1d` patch in our ggml fork (input-range tightening — see LEARNINGS, MUST RE-APPLY on every ggml bump).
  6. ✓ ECAPA speaker_encoder runtime forward — cos=0.999999 (`c0a9cb3`, `8a4c49e`, `38040b4`). C ABI: `qwen3_tts_compute_speaker_embedding(audio, n, sr)` + `qwen3_tts_set_voice_prompt[_with_text]`.
  7. ✓ Codec encoder runtime forward — diff 3 stages cos≥0.999 (`ef11c01`, `10302b4`). Closes the bake-script loop.
- **Performance pass (in progress, partial wins shipped).** Quiet-bench Q8_0 0.6B with all defaults: ~79 ms/frame (talker ~30 + cp ~49) on a quiet M1 — already under the 80 ms/frame real-time budget at 12.5 fps. Under normal system load ~129 ms/frame; talker and cp scale proportionally with Metal contention. Shipped: **`QWEN3_TTS_O15=1` is default-on** (commit `5e21e4a`) — cp graph reuse saves ~14 ms/frame on cp_pred under contention, ~2-3 ms/frame quiet, bit-identical WAV. Gated, byte-identical, kept default-OFF: `QWEN3_TTS_FUSED_QKV=1` (**Q8_0 bench done 2026-05-23: neutral — interleaved A/B on this M1 shows 129 vs 129 ms/frame; keep default-OFF for Q8_0**; **F16 case benched 2026-05-24: inconclusive — interleaved A/B (6 runs) on loaded machine (model DL + build concurrent) shows σ≈47 ms/frame exceeding any signal; mean baseline 212 vs mean fused 191 ms/frame, warm-up baseline 133 ms/frame consistent with Q8_0 quiet result; keep default-OFF for F16 same as Q8_0; clean quiet-machine bench still open**); `QWEN3_TTS_LK_BUCKET=1` (talker Lk bucketing, **net loss on M1 Metal Q8_0** — see LEARNINGS); `QWEN3_TTS_CP_STEP0_CACHE=1` (cp T=2 step-0 graph cache, claimed 1-3 ms/frame quiet savings — **loaded-machine bench neutral within noise; quiet-machine confirmation still pending**, bit-identical). Investigated: Q8_0 KV cache — blocked on Metal `cont(Q8_0)` source (only F32/F16/BF16 sources supported); needs Metal kernel patch or KV layout restructure to land. Still open: F16 FUSED_QKV clean quiet-machine bench (F16 GGUF now at `/Volumes/backups/ai/crispasr/qwen3-tts-12hz-0.6b-base.gguf`; rerun on a quiet machine with no background I/O); Q4_K talker fused QKV bench (needs Q4_K talker GGUF); the larger lift of fusing 15 cp steps into one graph (needs on-device top-k sampling, ~3 ms/frame upper bound after O15 since most overhead is already gone).
- Debug knobs: `QWEN3_TTS_{BENCH,DEBUG,DUMP_DIR}` env vars; diff harness via `tools/reference_backends/qwen3_tts.py` + `crispasr-diff qwen3-tts`.
- **Reuse:** the talker is essentially Qwen3-0.6B/1.7B with a
  multi-codebook output head — `core_attn::kv_self_attn` +
  `core_ffn::swiglu` again. The codec needs new code for RVQ
  decoding; that work is shared with MiMo (#51) and overlaps in
  shape with the VibeVoice σ-VAE decoder, so a `core_audio_decoder`
  helper is worth landing alongside the runtime (see #53).

**Effort:** Large. ~1500 LOC across runtime + codec + reference
backend. The two TTS targets (Qwen3-TTS and any future expansion)
share enough that landing one substantially de-risks the other.

---

## 96. voxcpm2-tts perf — switch to per-step ggml graph (Metal-ready) — DONE (graph-default flipped 2026-06-04)

Graph path is the default (`VOXCPM2_USE_GRAPH=0` opts out): 1.5–1.6×
on x86 CPU (VPS + Kaggle A/B, identical ASR roundtrips), Metal-ready.
Voice cloning structurally correct (diff cos ≥ 0.98 vs upstream under
`VOXCPM2_USE_REF=1`); CLI 24-vs-48 kHz header bug fixed (0321fa5e).
Detail → HISTORY / LEARNINGS (voxcpm2 entries).

## 54-follow-up. granite-speech-4.1 plus speaker labels + word timestamps — open

Variants 4.1 / 4.1-plus / 4.1-nar shipped bit-exact on JFK → HISTORY
§61. Remaining: speaker labels + word-level timestamps for the `plus`
variant via chat_template (~50 LOC, template-only).

---


## 56. Kokoro multilingual phonemizer (espeak-ng)

Kokoro/StyleTTS2 is multilingual at the model level — the 178-symbol IPA
vocab covers en, de, fr, ru, cmn, ja and more — but until this work the
runtime always shelled out to `popen("espeak-ng -q --ipa=3 -v LANG …")`,
which (a) cost ~30–50 ms per call on the shell-quoting + fork path,
(b) needed `espeak-ng` on `$PATH`, and (c) emitted U+200D ZWJ tie
characters and newline-separated sentence chunks that the GGUF
tokenizer then has to silently absorb.

This item replaces the popen path with in-process libespeak-ng calls
behind a CMake AUTO probe, while keeping popen as a runtime fallback
so existing builds don't regress.

### Done (this session)

- `src/CMakeLists.txt`: `CRISPASR_WITH_ESPEAK_NG` cache string
  (`AUTO`/`ON`/`OFF`, default `AUTO`). AUTO probes `pkg-config
  espeak-ng` first, then a Homebrew/Linux fallback
  (`/opt/homebrew`, `/usr/local`, `/usr`). When found, defines
  `CRISPASR_HAVE_ESPEAK_NG=1` and links `libespeak-ng` via PUBLIC so
  it propagates into `crispasr` / `libcrispasr.dylib`. `ON` makes a
  missing lib a hard error; `OFF` skips the probe entirely.
- `src/kokoro.cpp`:
  1. `kokoro_phoneme_cache` — bounded LRU (1024 entries,
     mutex-protected) keyed on `lang \0 text`, lives in
     `kokoro_context`.
  2. `phonemize_espeak_lib()` — gated on `CRISPASR_HAVE_ESPEAK_NG`.
     Lazy `espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, …,
     espeakINITIALIZE_PHONEME_IPA | espeakINITIALIZE_DONT_EXIT)`
     behind a process-global mutex; sticky-init-failure flag so we
     don't keep retrying. `CRISPASR_ESPEAK_DATA_PATH` env var
     overrides the data dir for sandboxed apps. Voice changes are
     sticky. Loops `espeak_TextToPhonemes` until `textptr==NULL`,
     joining chunks with spaces.
  3. `phonemize_popen()` — the old shell-out, kept as a runtime
     fallback. `kokoro_synthesize` now calls `phonemize_cached()`
     which tries cache → lib → popen.
- `examples/cli/crispasr_backend_kokoro.cpp`: maps `-l/--language`
  to `cp.espeak_lang`. `auto` keeps the default (en-us) since
  espeak has no auto-detect mode.
- Smoke-tested standalone against libespeak-ng: en-us, de, fr,
  cmn, ru, ja all produce IPA. Compared lib vs popen: see
  LEARNINGS.md "Kokoro phonemizer: libespeak-ng vs popen
  divergence" for the ZWJ + sentence-join behaviour.
- Build verified: `otool -L libcrispasr.dylib` shows
  `libespeak-ng.1.dylib`; `nm libkokoro.a` has the three espeak
  symbols.
- **End-to-end synth check** (against
  `/Volumes/backups/ai/crispasr-models/kokoro-82m-f16.gguf` +
  `kokoro-voice-af_heart.gguf`):
  | lang | phonemes | duration | peak | RMS | verdict |
  |---|---|---:|---:|---:|---|
  | en  | clean | 3.45 s | 11443 | 1545 | ✅ healthy |
  | de  | clean | 4.08 s |   541 |   44 | ❌ near-silence on long phrases (no German voice — see open #1) |
  | fr  | clean | 3.40 s | 12374 | 1434 | ✅ healthy |
  | ru  | clean | 3.38 s | 11375 | 1506 | ✅ healthy |
  | cmn | espeak tone numbers (`ni2χˈɑu2…`) | 3.20 s | 11731 | 1627 | ✅ **FIXED 2026-05-23**: `strip_cmn_tone_numbers` removes digits after phonemization |
  | ja  | kanji fallback (`(en)tʃˈaɪniːz(ja)…`) | 8.38 s | 15460 | 1581 | ⚠️ partial — kana works, kanji becomes English — needs MeCab/KaKaSi (open #3) |

  Short German phrases ("Hallo Welt.", "Guten Morgen.") synthesize
  fine with `af_heart`; the silence collapse only triggers on longer
  out-of-distribution phoneme sequences. See LEARNINGS.md "Kokoro
  phonemizer: libespeak-ng vs popen divergence" for full results.

### Open

1. **German voice pack — DE is a primary target language.** Kokoro-82M
   ships voices only for `a/b` (en US/UK), `e` (es), `f` (fr), `h` (hi),
   `i` (it), `j` (ja), `p` (pt), `z` (zh). No `d_*` (de), no `r_*` (ru),
   no Korean/Arabic. Three options ordered by effort:

   **Option 1 — Closer-language voice fallback (SHIPPED 2026-05-01).**
   Measured against the long German phrase ("Guten Tag, dies ist ein
   Test des deutschen Phonemizers."):

   | voice | peak | RMS | duration | verdict |
   |---|---:|---:|---:|---|
   | `af_heart` (English) |   541 |   44 | 4.08 s | silence collapse |
   | `ff_siwis` (French)  | 20577 | 2318 | 4.22 s | healthy, French-accented |
   | `ef_dora` (Spanish)  | 15036 | 1613 | 3.35 s | healthy, Spanish-accented |

   Wired into `examples/cli/crispasr_backend_kokoro.cpp` as an
   auto-fallback. Selection table:

   | `-l` value | preferred voice | rationale |
   |---|---|---|
   | `de`, `de-*`, `de_*` | `df_victoria` (Option 2b — kikiri-tts, Apache-2.0) → `df_eva` (Option 2a — Tundragoon, Apache-2.0) → `ff_siwis` | in-distribution to dida-80b backbone first; Tundragoon as second tier; French as last resort |
   | everything else without a native pack (ru, ko, ar, …) | `ff_siwis` (French) | non-silence baseline |

   Resolution: `--voice` (explicit) → cascade above → empty (helpful
   error). Explicit `--voice` always wins. Voice GGUFs live at
   `/Volumes/backups/ai/crispasr-models/kokoro-voice-{af_heart,
   ef_dora, ff_siwis, df_eva, dm_bernd, df_victoria, dm_martin}.gguf`.

   **Option 2a — Recovered Tundragoon's German voice packs (DONE,
   SHIPPED 2026-05-01).**
   The only public German Kokoro voice pack on HF was
   `Tundragoon/Kokoro-German` (Apache-2.0) — the user account was
   deleted in early 2026 and the HF repo is 404. **Voices recovered**
   from `r1di/kokoro-fastapi-german`'s Git LFS (`api/src/voices/v1_0/
   {df_eva,dm_bernd}.pt`, sparse + LFS pull). They are
   `[512, 1, 256]` F32 (vs the 510 of official Kokoro voices —
   Tundragoon's fine-tune used a slightly larger max_phonemes; the
   GGUF voice loader reads max_phonemes from the file so this is fine).

   End-to-end synth with the **official** Kokoro-82M model on the
   long German phrase ("Guten Tag, dies ist ein Test des deutschen
   Phonemizers."):

   | voice | peak | RMS | duration | note |
   |---|---:|---:|---:|---|
   | `df_eva` (German F)  | 14716 | 1648 | 3.50 s | healthy, German speaker |
   | `dm_bernd` (German M)| 19185 | 2374 | 3.88 s | healthy, German speaker |

   Both produce non-silent, German-timbred audio with the official
   Kokoro-82M weights — **the matching Tundragoon model fine-tune
   (`kokoro-german-v1_1-de.pth`) is not required.** That model is
   *unrecovered* (only available from the deleted HF repo per
   `r1di/docker/scripts/download_model.py`), but voices alone are
   sufficient for this fallback path. Caveat: predictor + decoder
   weights are still the official English-trained Kokoro-82M's, so
   prosody is not fully native German. Better than ff_siwis (German
   speaker timbre instead of French), worse than Option 2b.

   GGUF artefacts at
   `/Volumes/backups/ai/crispasr-models/kokoro-voice-{df_eva,dm_bernd}.gguf`.
   Wired as the German auto-fallback (Option 1 table above).

   **Option 2b — Native German backbone via dida-80b (SHIPPED 2026-05-01).**

   Sources (all Apache-2.0 weights + Apache-2.0 recipe + CC0 dataset):
   - Recipe: <https://github.com/semidark/kokoro-deutsch> — clone
     locally (recurse-submodules: `StyleTTS2/` + `kokoro/`).
     `scripts/extract_voicepack.py` is the tool for fresh per-speaker
     voicepacks; we did not need to run it (kikiri-tts ships
     pre-extracted voicepacks — see below).
   - Backbone: <https://huggingface.co/dida-80b/kokoro-german-hui-multispeaker-base>
     — `first_stage.pth` + `config.json`. Stage-1 multispeaker base
     fine-tune of Kokoro-82M on HUI-Audio-Corpus-German (51 speakers,
     51 h, 10 epochs A40, mel loss 0.583 → 0.326).
   - Pre-extracted voicepacks (kikiri-tts org, dida-80b maintainer):
     <https://huggingface.co/kikiri-tts/kikiri-german-victoria> +
     <https://huggingface.co/kikiri-tts/kikiri-german-martin>. Each
     ships `voices/{victoria,martin}.pt` extracted via the kikiri
     synthetic StyleEncoder which shares lineage with the dida-80b
     base — saves us from running `extract_voicepack.py` ourselves
     (the underlying HUI corpus is gated and would require a multi-step
     LibriVox-pulling pipeline to reproduce).

   What this adds over Option 2a:
   - **Predictor + decoder are German-trained.** Solves the root
     cause behind the af_heart silence collapse on long German
     phrases — voices alone (Option 2a) only cover the speaker
     timbre, not the prosody/duration distribution.
   - StyleEncoder is German-trained → kikiri voicepacks are in-
     distribution. Pairs cleanly with the dida-80b backbone.

   Steps taken:
   1. ✓ `models/convert-kokoro-to-gguf.py` extended for the modern
      `torch.nn.utils.parametrize` WeightNorm form
      (`parametrizations.weight.original0/original1`) used by dida-80b,
      tolerated the missing `module.` DataParallel prefix on bert keys,
      and added `--config` so the official Kokoro-82M `config.json`
      can be reused (dida-80b ships only a HF-hub stub config without
      vocab; the 178-symbol IPA vocab IDs are byte-identical per
      semidark's `training/kokoro_symbols.py`).
   2. ✓ Converted to
      `/Volumes/backups/ai/crispasr-models/kokoro-de-hui-base-f16.gguf`
      (163.7 MB at F16; 459 tensors mapped, 0 skipped — same byte size
      as `kokoro-82m-f16.gguf`, confirming identical architecture).
   3. ✓ Pulled kikiri voicepacks `voices/{victoria,martin}.pt`
      (510×1×256 F32) via `huggingface_hub.hf_hub_download` and
      converted them with the existing
      `models/convert-kokoro-voice-to-gguf.py` to
      `kokoro-voice-{df_victoria,dm_martin}.gguf` (~510 KB each,
      `[510,1,256]` F32 — direct passthrough, no converter changes).
   4. ✓ C ABI: new `crispasr_kokoro_resolve_model_for_lang()` and
      `crispasr_kokoro_resolve_fallback_voice()` in `src/kokoro.h` /
      `src/kokoro.cpp`, re-exported with the `_abi` suffix from
      `src/crispasr_c_api.cpp` so the dylib (and every wrapper that
      links against it) gets them.
   5. ✓ CLI: `examples/cli/crispasr_backend_kokoro.cpp` now delegates
      to the C ABI. When `-l de*` AND the user-passed model basename
      starts with `kokoro-82m`, the backend silently swaps to a
      sibling `kokoro-de-hui-base-f16.gguf` if present, then loads
      the German fallback voice from the new cascade
      `df_victoria → df_eva → ff_siwis`.
   6. ✓ Python wrapper: `crispasr.kokoro_resolve_for_lang(model, lang)`
      returns `KokoroResolved(model_path, voice_path, voice_name,
      backbone_swapped)`; surfaced from `crispasr/__init__.py`.

   End-to-end measurements on the long German phrase
   ("Guten Tag, dies ist ein Test des deutschen Phonemizers."), each
   ASR-roundtripped through `parakeet-v3 -l de` so we measure
   intelligibility and not just envelope:

   | model + voice | peak | RMS | sec | ASR roundtrip |
   |---|---:|---:|---:|---|
   | official + df_eva (Option 2a) | 14726 | 1648 | 3.50 | "...Phonemizer." (lost trailing 's') |
   | dida-80b + df_eva             | 23477 | 1830 | 3.50 | "...Phonemetzes." (1 word boundary error) |
   | dida-80b + df_victoria        | 12052 | 1177 | 4.22 | "...Tester des Deutschen Phonemizers." (1 word boundary error) |
   | dida-80b + dm_bernd           | 18948 | 2693 | 3.88 | "...Phonemetzers." (1 word boundary error) |
   | **dida-80b + dm_martin**      | 18100 | 1546 | 3.98 | **"...Phonemizers." (perfect)** |

   All four German voices clear the gate (peak ≥ 8000, RMS ≥ 1000)
   on the dida-80b backbone, and three of four are word-perfect except
   for one minor token-boundary error each. dm_martin is byte-perfect
   round-trip; df_victoria handles "Phonemizers" correctly which df_eva
   misses. This is the "fully native German signal path" the option
   promised: predictor + decoder + StyleEncoder distribution all
   German.

   For deployable single-speaker production quality, run Stage-2
   fine-tuning on one HUI speaker (~half-day on an A40) — out of
   scope of this PLAN item; track separately if needed.

   **Option 3 — Extract a style embedding via the English-trained
   StyleEncoder (only if 2a + 2b are blocked).**
   Same recipe as Option 2a's recovery effort but starting from a
   fresh German recording (Common Voice DE, public-domain
   audiobook). `[max_phon=510, 1, 256]` style tensor through
   StyleTTS2's StyleEncoder, save as `.pt`, convert. Strictly worse
   than Option 2b because the predictor/decoder aren't German-aware;
   keep as last-resort.

   **Status:**
   1. ✓ Option 1 shipped (auto-fallback table per-language).
   2. ✓ Option 2a shipped (df_eva + dm_bernd recovered from r1di's
      Git LFS, Apache-2.0; works with both backbones).
   3. ✓ Option 2b SHIPPED (dida-80b backbone + kikiri-tts voicepacks,
      all Apache-2.0; truly native German prosody on long phrases).
      Auto-routing kicks in when both `kokoro-82m-f16.gguf` and
      `kokoro-de-hui-base-f16.gguf` sit in the same directory.
   4. Option 3 not needed.

   **Follow-ups:**
   - ✅ HF GGUF mirrors published (2026-05-01):
     [`cstr/kokoro-82m-GGUF`](https://huggingface.co/cstr/kokoro-82m-GGUF),
     [`cstr/kokoro-de-hui-base-GGUF`](https://huggingface.co/cstr/kokoro-de-hui-base-GGUF),
     [`cstr/kokoro-voices-GGUF`](https://huggingface.co/cstr/kokoro-voices-GGUF)
     — F16 + Q8_0 backbones (Q4_K dropped — see LEARNINGS), 7 voicepacks.
   - ✅ Auto-download via `src/crispasr_model_registry.cpp` (PLAN #56).
     New `ExtraCompanion` mechanism in the registry — backends with >1
     auxiliary file (kokoro: English voice + German backbone + German
     voice) can list extras alongside the inline `companion_file`.
     `crispasr --backend kokoro -m auto -l de` now pulls all 4 files
     and auto-routes to the German backbone.
   - ✅ Wrapper TTS surface across Rust/Go/Java/JS/Ruby
     (commit `4f476c3`, 2026-05-01). Each binding gets
     `Session.{open,setVoice,setCodecPath,synthesize,close}` plus
     `kokoroResolveForLang(model, lang)` returning the same
     `KokoroResolved` shape as the Python wrapper.
   - Stage-2 fine-tune on one HUI speaker (~half-day A40) for
     deployable single-voice production quality. Out of scope here.
2. **Mandarin tone numbers.** espeak-ng outputs digit-suffixed
   tone markers (`ni2χˈɑu2`) that aren't in the kokoro-82m IPA vocab
   (178 symbols) and likely get dropped at tokenization, losing tone
   info. Investigate whether `--ipa=2` (without tone numbers) plus a
   separate tone embedding would work, or whether to switch to a
   different Mandarin G2P (e.g. `pypinyin`).
3. **Japanese kanji.** espeak-ng falls back to English pronunciation
   for kanji (e.g. 日本語 → "Chinese letter"), inserting `(en)…(ja)`
   voice-switch markers that aren't IPA. For full Japanese support,
   pre-process input with a Japanese frontend to convert kanji → kana
   before espeak. **MIT-clean approach (2026-06-16 survey):**
   - MeCab (BSD-3-Clause) + unidic-lite (MIT) for morphological
     analysis → kanji reading extraction
   - NO kakasi/pykakasi (GPL-3.0 — viral license, incompatible)
   - Feed kana output to existing espeak-ng `ja` voice for IPA
   - Implementation: either libmecab C API via dlopen (like espeak)
     or a pre-built kanji→kana lookup dictionary shipped as a flat
     file (avoids runtime MeCab dependency). The dict approach is
     simpler but less accurate on rare/compound words.
   - fugashi (MIT) is a Python MeCab wrapper; cutlet (MIT) does
     kanji→romaji. Either can generate the offline dict.
4. ~~**Diff harness reference backend.**~~ **DONE — phonemizer-step
   diff (May 2026).** The model-side reference dumper at
   `tools/reference_backends/kokoro.py` already covered the 16 model
   stages; the phonemizer step is now covered by a separate sibling
   tool `tools/check_kokoro_phonemizer_parity.py` that exercises the
   newly-exposed `kokoro_phonemize_text_{lib,popen}` C ABI on a fixed
   `(lang, text)` suite (en / de / fr / ru / cmn / ja / it / es / pt)
   and reports drift between the two paths. Default mode normalises
   away the documented benign U+200D ZWJ tie chars (LEARNINGS §6);
   `--strict` does byte-exact comparison. Initial run surfaces 1 real
   substantive divergence in cmn (`ni2χˈɑu2` vs `niɜχˈɑ‍u2`) — that's
   #56 #2's symptom, captured automatically now. No-model unit tests
   in `tests/test_python_session.py` cover the symbol export +
   null-args return path.
5. ~~**Optional polish.**~~ **DONE + CROSS-BINDING.**
   `kokoro_phoneme_cache_clear()` + session-scoped
   `crispasr_session_kokoro_clear_phoneme_cache()` ABI exports for
   long-running daemons that resynthesize across many speakers. Wrappers
   landed across all 7 bindings (Python `Session.clear_phoneme_cache()`,
   Rust `Session::clear_phoneme_cache()`, Dart `clearPhonemeCache()`,
   Go `Session.ClearPhonemeCache()`, Java `clearPhonemeCache()`, JS
   `Module.ttsClearPhonemeCache()`, Ruby `Session.clear_phoneme_cache()`).
   No-model unit tests cover the symbol export + null-handle return path.

### Effort

Small individually. Open items 2 + 3 are each an afternoon if we
go the pre-processing route. Open item 1 is "policy" — a one-line
fallback in the backend or a docs change. Open item 4 is ~150 LOC.
Open item 5 is ~20 LOC if asked.

---

## 57. Commercial-friendly TTS backend expansion

May 2026 sweep through high-traffic HF TTS models. Filter is **permissive
license + reusable architecture + reasonable effort**. Sequenced so each
phase unlocks a family of finetunes — finishing Phase 3 (Chatterbox stack)
also unlocks Phase 5's CFM solver, etc.

License triage that drives the ordering:

| ✅ Permissive (commercial OK) | ⚠️ Llama-3.2 community (commercial OK with attribution) | ❌ Non-commercial — defer |
|---|---|---|
| Qwen3-TTS-{Base,CustomVoice} (Apache 2.0) | Orpheus-3B family + Kartoffel_Orpheus (llama3.2) | SebastianBodza/Kartoffelbox-v0.1 (CC-BY-NC-ND) |
| ResembleAI/chatterbox base (MIT) | HumeAI/tada-3b-ml (llama3.2) | marduk-ra/F5-TTS-German (CC-BY-NC) |
| SebastianBodza/Kartoffelbox_Turbo (CC-BY-4.0, gated) | | mlx-community/fish-audio-s2-pro (Fish-Audio Research) |
| oddadmix/lahgtna-chatterbox-v0/v1 (MIT) | | amphion/Vevo1.5 (CC-BY-NC-ND) |
| openbmb/VoxCPM2 (Apache 2.0) | | mlx-community/Voxtral-4B-TTS-2603 (CC-BY-NC; upstream Mistral Apache OK) |
| FINAL-Bench/Darwin-TTS-1.7B-Cross (Apache 2.0) | | |
| AMAImedia Qwen3-1.7B-TTS-Cross-Darwin AWQ (Apache 2.0) | | |
| g-group-ai-lab/gwen-tts-0.6B (MIT) | | |
| kugelaudio/kugelaudio-0-open (MIT) | | |

License gaps to resolve before depending on a model: CosyVoice 3
(`FunAudioLLM/Fun-CosyVoice3-0.5B-2512` — model card silent;
v1/v2 were Apache 2.0 but v3 not yet confirmed).

### Phase 1 — DONE

_Done — see HISTORY.md + git log._

## 58. MOSS-Audio-4B-Instruct

[`OpenMOSS-Team/MOSS-Audio-4B-Instruct`](https://huggingface.co/OpenMOSS-Team/MOSS-Audio-4B-Instruct)
— Apache-2.0, ~4 B params, released 2026-04. First **audio-
understanding** model in the queue (not just ASR): speech, music,
environmental sounds, scene QA, time-aware ASR, multi-step
reasoning. Mandarin + English. The Instruct variant is the entry
point; the family also has 8B and Thinking (CoT) variants sharing
the same architecture.

### Architecture summary (from `config.json`)

- **Audio encoder** — 32-layer Whisper-style transformer trained
  from scratch (not a stock Whisper checkpoint). 1280 d / 20 heads,
  GELU FFN 5120 d, 128 mel bins, max 1500 source positions, sliding-
  window attention with window=100. Output rate 12.5 Hz after
  downsample (rate=8). The novel bit: **cross-layer feature taps**
  at layers 8, 16, 24 (in addition to the final 32) — these are
  carried through the adapter into the LM via DeepStack injection
  (see below).
- **DeepStack adapter** — adapter MLP (8192 d hidden) projects each
  of the 4 encoder taps into LM-embedding space (2560 d) with
  independent weights. The 4 projections are added as residuals
  into LM block inputs at indices 0, 1, 2, 3 (so the encoder's
  multi-resolution features inject continuously through the LM's
  early layers). This preserves low-level prosody / transients
  alongside high-level semantics in a way single-tap projectors
  (qwen3-asr / voxtral / granite-speech) can't.
- **Time-aware tokens** — explicit time-marker tokens are inserted
  between audio frame embeddings at fixed intervals. The LM learns
  "what happened when" natively; supports word-level + sentence-
  level timestamp ASR + time-based QA without a separate aligner.
- **LM** — 36-layer Qwen3 (hidden=2560, 32 Q / 8 KV head_dim=128,
  SwiGLU, RMSNorm, RoPE θ=1 M, max_pos=40 960, vocab=151 936,
  untied lm_head). No sliding window; full attention.

### Effort breakdown

| Component | LOC | Reuse |
|---|---:|---|
| Audio mel front-end (128-bin) | ~50 | `core_mel` |
| 32-layer Whisper-style encoder | ~150 | ~70 % from `qwen3_asr.cpp` encoder |
| Encoder sliding-window attention | ~50 | reuse pattern from `voxtral4b` |
| **DeepStack 4-tap output capture** | ~80 | **new** — needs encoder builder hooks at L8/16/24/32 |
| **DeepStack 4-projection adapter** | ~60 | **new** — 4× MLP, run once after encoder |
| **DeepStack injection into LM blocks 0–3** | ~120 | **new** — adds a fixed-shape residual at `cur` before block-N's first norm |
| Time-marker tokenization | ~100 | **new** — chat template builder + per-frame interval logic |
| Qwen3 LM body | ~50 | full reuse (`core_attn::kv_self_attn` + `core_ffn::swiglu`) |
| Greedy / sampler decode | ~80 | `core_bpe::tokenize_with_specials` + step builder pattern from `mimo_asr.cpp` |
| Converter (HF → GGUF) | ~250 | `models/convert-mimo-asr-to-gguf.py` template |
| Diff harness reference + 6 stages | ~200 | `tools/reference_backends/mimo_asr.py` template |
| Backend wrapper for main CLI | ~120 | `crispasr_backend_mimo_asr.cpp` template |
| **Total** | ~**1200–1500 LOC** | comparable to PLAN #51 |

Headline new helper: a **DeepStack injection block** (probably
`core_deepstack::inject(ctx, cur, projector_w, projector_b,
encoder_tap)`) that's reusable for any future model adopting this
pattern. The 4 projection heads are independent matmul + bias adds
applied to the captured encoder taps; injection is a residual add
at the input of LM blocks 0..3.

### What we'd need to dump from the Python ref

Stage taps for the diff harness:
- `mel_in` `[T_mel, 128]`
- `enc_l8` / `enc_l16` / `enc_l24` / `enc_l32` `[T_enc, 1280]`
  (the four DeepStack taps)
- `adapter_proj_{0,1,2,3}` `[T_enc, 2560]` (post-projection)
- `lm_inputs_embeds` `[T_total, 2560]` (pre-block-0)
- `lm_block_3_in` `[T_total, 2560]` (after the last DeepStack
  injection — this is where a multi-tap bug would show up)
- `lm_last_hidden` + `lm_logits_step0` (standard tail)

Six-to-eight stages, similar to mimo-asr's prefill captures.

### Risks / open questions

1. **DeepStack injection point semantics** — does the projection
   replace the LM block's input or get added as a residual? Need
   to read `processing_moss_audio.py` + the model's `forward()` to
   confirm. If it's a *replace* (not residual), the injection
   builder is simpler but the math is more sensitive.
2. **Time-marker token vocab** — are these dedicated special tokens
   in the Qwen3 BPE, or are they synthesized in the embedding
   space? The vocab=151 936 has slots beyond Qwen3's 151 643 BPE +
   30 special — likely the extra ~263 are time markers.
3. **Sliding-window encoder attention with mask=100** — already a
   pattern (`voxtral4b`), but interacts non-trivially with the
   12.5 Hz downsample. Confirm causal vs bidirectional via Python
   ref hook.
4. **Family extensibility** — 8B variant has the same architecture
   per the README, just bigger LM hidden + layer count. If we
   parameterize by config, all four (4B/8B × Instruct/Thinking)
   share one runtime. Worth doing up front.

### Why "audio understanding, not just ASR" matters here

The 24 ASR-style backends in CrispASR all map audio → text
transcription. None handle "describe the music in this clip", "is
the speaker happy", "summarise this 10-minute meeting", or
"transcribe with word-level timestamps". MOSS-Audio is the first
candidate that covers that ground with an open license (Apache-2.0)
and a reasonable size (4 B → ~2.5 GB Q4_K). Adding it expands
CrispASR's surface meaningfully — analogous to how qwen3-tts
expanded scope to TTS.

### Sequencing

Don't start until:
- mimo-asr perf follow-ups (51a/b/c) are at least scoped — they'll
  inform DeepStack's KV-reuse strategy.
- Orpheus / Qwen3-TTS-1.7B (PLAN #57 phases 1–2) finish — those are
  active sessions and the parallel-worker contention is high.

Probable kickoff: mid-to-late May 2026 if the queue clears.

---

## Ecosystem expansion (lower priority)

### New backends from PazaBench assessment (see HISTORY.md #30)

| Model | License | Approach | Priority |
|---|---|---|---|
| Wav2Vec2 Conformer | Apache-2.0 | Conformer attention variant | Medium |
| Qwen2-Audio 7B | Apache-2.0 | Whisper encoder + Qwen2 LLM | Medium |
| OmniASR larger (1B/3B/7B) | Apache-2.0 | Same converter, bigger models | Medium |
| NeMo Canary-Qwen-2.5b | Apache-2.0 | FastConformer + Qwen2.5 decoder | Medium |
| Paza / Phi-4 | MIT | 14B multimodal, defer to llama.cpp | Low |
| **XiaomiMiMo/MiMo-V2.5-ASR** | TBD (check) | LLM-style multimodal speech (similar to Qwen3-ASR pattern) | Medium — user-requested in #35 |
| **google/gemma-4-E2B** | Gemma terms | Conformer + Gemma 4 decoder; matches "Gemma 4 Audio" entry below | Medium — user-requested in #35 |

### From llama.cpp (MIT)

| Model | Architecture | Notes |
|---|---|---|
| Ultravox | Whisper encoder + Llama 3.2 1B/8B | Speech understanding |
| Gemma 4 Audio | Conformer, chunked attention | Streaming, multimodal |
| LFM2-Audio | Conformer variant | Position embeddings |

### Post-processing

| Model | License | Type | Priority |
|---|---|---|---|
| FireRedPunc | Apache-2.0 | BERT punct (zh+en) | **DONE** |
| fullstop-multilingual | MIT | XLM-R punct (en/de/fr/it) | **DONE** — runtime in fireredpunc.cpp |
| punctuate-all (kredor) | MIT | XLM-R-base punct (12 langs) | **DONE** — `--punc-model punctuate-all` |
| 1-800-BAD-CODE PCS | Apache-2.0 | XLM-R punc+truecase+SBD (47 langs) | **DONE** — `--punc-model pcs` |
| CT-Transformer (FunASR) | Apache-2.0 | SANM 3-layer (vocab 272727), Chinese+English; production-default in FunASR/RapidPunc | Medium — `modelscope/punc_ct-transformer_zh-cn-common-vadrealtime-vocab272727-pytorch`. SANM block primitives already in CrispASR (`src/core/sanm.h`, used by funasr + sensevoice). Adds a Chinese punc option distinct from BERT-style FireRedPunc; VAD-realtime variant emits punc per-segment for streaming. New backend alias `ct-punc`. |
| truecaser-lstm (BiLSTM) | Apache-2.0 | mayhewsw char-level BiLSTM (3.2 MB, 97.9% F1) | **DONE** — `--truecase-model lstm` (recommended) |
| truecaser-crf | MIT | CRF + context features (24 MB) | **DONE** — `--truecase-model crf` |
| truecaser-de (statistical) | MIT | Wikipedia word-freq (375K entries, 9 MB) | **DONE** — `--truecase-model auto` |
| bert-restore-punctuation | MIT | BERT punct+truecase (en) | Low |
| xashru/punctuation | Apache-2.0 | XLM-R+BiLSTM-CRF (40+ langs) | Low |

### Optimizations (cross-cutting, from survey + CrispEmbed comparison)

| # | Optimization | Applies to | Expected gain | Status |
|---|---|---|---|---|
| O1 | `ggml_soft_max_ext` fusion | wav2vec2, canary, fastconformer | -10% wav2vec2 | **DONE** |
| O11 | wav2vec2 CNN → ggml | wav2vec2 family | **10.8x** | **DONE** |
| O9/#44 | FireRed ggml Q4_K decoder | firered-asr | **6.3x** | **DONE** |
| O10 | Sliding window attention | voxtral4b | Already implemented | **DONE** |
| O2 | Fused QKV pre-merge | LLM decoders | ~10-15% attn (GPU) | API ready in core/attention.h; CPU gain <1%, defer to GPU |
| O3 | Temperature sampling | glm-asr, kyutai-stt | Feature parity | **DONE** |
| O5 | Pipelined mel+encode | LLM backends, CPU | ~15-20% | TODO |
| O4 | Beam search for LLMs | Audio-LLM backends | Quality | **DONE** — 18/24 backends via `core_beam_decode` (§139, §61h); only mimo-asr blocked on #115 |
| O6 | Batched encoder (GPU) | All + GPU | 3-5x | TODO |
| O7 | Speculative decoding | LLM backends | 2-4x decode | TODO |
| O12 | `ggml_conv_1d_cf` channels-first conv | vibevoice VAE | **-29% VAE, -15% total** | **DONE** |
| O13 | `ggml_conv_1d_group` + CNN cleanup | wav2vec2 family | **-12% total** (pos -12%, CNN -22%) | **DONE** |
| O14 | `--tts-steps` configurable DPM steps | vibevoice TTS | **-31% diffusion** | **DONE** |
| O15 | Remove redundant neg base LM | vibevoice TTS | Eliminated 60 LOC of wasted compute | **DONE** |

**From COMPARISON.md (llama.cpp patterns):**
- `ggml_soft_max_ext` with baked scale (O1) — already in llama.cpp, saves one `ggml_scale` op per attention layer
- Chunked window attention (O10) — llama.cpp uses for Gemma4A Conformer
- Conv2d subsampling via ggml ops — llama.cpp does this for Qwen3-ASR encoder

**From CrispEmbed (shared core patterns):**
- Fused QKV (O2) — CrispEmbed pre-merges Q/K/V weights at init, one matmul instead of 3
- SentencePiece Viterbi DP tokenizer — CrispEmbed has proper optimal tokenization
- Lazy graph allocation (`no_alloc=true` + scheduler) — reduces memory churn

**From LEARNINGS.md (FireRed decoder triage):**
- Small per-step ggml graphs are SLOWER than CPU loops (scheduling overhead)
- BUT: native Q4_K matmuls via ggml are 9.3x faster than F32 OpenMP (lesson: never dequant)

### Audio format support

- `.m4a`, `.mp4`, `.webm` crash with upstream ffmpeg integration — needs fix or robust fallback
- `.aiff`, `.wma`, raw PCM not supported without pre-conversion
- Consider bundling a lightweight M4A/AAC decoder or improving the ffmpeg path
- Only move LARGE, REUSED matmuls onto ggml/GPU
- Persistent subgraphs per decode step > one-off graphs

### Other

- **OmniASR-LLM beam search** — beam=2+ with N hypothesis KV caches
- ~~**TTS module** — VibeVoice-Realtime-0.5B text-to-speech~~ **DONE** — perfect ASR round-trip on all test cases. 17 bugs found via stage-by-stage diff. Uses DPM-Solver++, dual KV CFG, voice prompts, EOS classifier, text/speech interleaving.
- ~~**ggml_conv_1d_dw F16 im2col fix**~~ **DONE** — solved via `ggml_conv_1d_dw_cf` (direct F32, no im2col)

---

## Publish language wrappers to package registries

Today the Rust, Dart, and Python wrappers all live in this repo and (for
Python) require a `pip install -e .` from a clone. Move all three onto
their language-native registries so users can install with one command.

**Status (2026-04-25):** All three wrappers now have publishable
metadata + dry-runs pass. The CI workflow `release-wrappers.yml` is
wired up but cannot run until the **one-time registry setup** below
is complete.

| Wrapper | Pre-flight | Blocker |
|---|---|---|
| Python `crispasr` 0.5.4 | sdist + wheel build clean | PyPI trusted-publisher must be configured |
| Dart `crispasr` 0.5.4 | `dart pub publish --dry-run` passes (warnings only) | pub.dev automated publishing must be configured |
| Rust `crispasr-sys` 0.5.4 | `cargo publish --dry-run` clean (5.9 KiB) | needs `CARGO_REGISTRY_TOKEN` repo secret |
| Rust `crispasr` 0.5.4 | publish-order dependent on `crispasr-sys` | same |

### One-time registry setup (must happen before first tag)

1. **PyPI** — go to https://pypi.org/manage/account/publishing/ and add
   a "pending publisher": owner `CrispStrobe`, repo `CrispASR`,
   workflow `release-wrappers.yml`, environment `pypi`. Then push any
   `v*` tag.
2. **crates.io** — generate a token at https://crates.io/me, add it
   as the `CARGO_REGISTRY_TOKEN` secret on the GitHub repo.
3. **pub.dev** — go to https://pub.dev/packages/crispasr/admin (after
   first manual publish or claim) → enable automated publishing → set
   tag pattern `v{{version}}`. Alternatively for the first publish,
   run `dart pub publish` locally with the package owner's credentials.

### Pattern (matches crispasr approach)

All three wrappers are thin FFI/ctypes shims over the C ABI in
`src/crispasr_c_api.cpp`. They do **not** bundle the native library — the
user must have `libcrispasr.{so,dylib,dll}` installed (Homebrew, apt, or
built from source). This keeps the wheels/crates/pub packages tiny and
avoids a per-platform build matrix on every release.

| Wrapper | Registry | Effort | Notes |
|---|---|---|---|
| Python | PyPI | Low | Add `python/pyproject.toml`; pure-Python wheel; `_helpers.c` builds at install if a C toolchain is present, else falls back to ctypes-only path |
| Rust   | crates.io | Low | `crispasr-sys` then `crispasr` (two `cargo publish` calls); already has `Cargo.toml` |
| Dart   | pub.dev | Low | `flutter pub publish --dry-run` then `flutter pub publish`; already has `pubspec.yaml` |

### Library discovery (Python)

Update `_find_lib()` in `python/crispasr/_binding.py` to probe, in order:
1. `$CRISPASR_LIB_PATH` env var (explicit override)
2. `sys.prefix/lib/` (system or virtualenv install)
3. Standard Homebrew/Linux paths (`/opt/homebrew/lib`, `/usr/local/lib`, `/usr/lib`)
4. Existing repo-relative fallbacks (for `pip install -e .` from a clone)

If none found, raise `RuntimeError` with a helpful message linking to
install docs (the same pattern Tesseract / faster-whisper use).

### Release automation

Add a tag-triggered workflow `.github/workflows/release-wrappers.yml`
that, on `v*` tags, runs in parallel:
- `python -m build && twine upload` (PyPI, OIDC trusted-publishing — no API token)
- `cargo publish -p crispasr-sys && cargo publish -p crispasr` (crates.io, `CARGO_REGISTRY_TOKEN` secret)
- `dart pub publish --force` (pub.dev, OIDC publishing)

Trigger only on tag push, not on every commit. Version bumps stay
manual — bump `pyproject.toml` / `Cargo.toml` / `pubspec.yaml` together
in the same commit that creates the tag.

### Future: bundled wheels for Python

After the pure-Python release is out, add a follow-up release pipeline
using `cibuildwheel` to produce manylinux2014 + macOS arm64/x64 +
Windows wheels with `libcrispasr.*` bundled inside via `auditwheel` /
`delocate` / `delvewheel`. Same for Rust if we ever want
`crispasr-sys` to vendor the native build like `tch-rs` /
`onnxruntime-sys` do. Defer until pure-Python wheel is out and stable.


---

## 59. Cross-binding C-ABI parity

The Session API surface for TTS (incl. qwen3-tts Base / CustomVoice /
VoiceDesign variant routing) is fully wrapped across all 7 bindings as
of commit `65e0a61` + the Dart follow-up. **The non-Session ABI (~80
exports) is still C-ABI-only or partially-wrapped on most bindings.**
This entry tracks closing those gaps.

### Coverage matrix (May 2026, post-#107)

C-ABI exposes 136+ unique `crispasr_*` exports in
`src/crispasr_c_api.cpp` (9 new in #107 P6 — pluggable speaker
embedder, agglomerative clustering, pyannote cache). Coverage by
binding:

| Binding | Symbols wrapped | Approx % | ASR Transcribe | TTS Session | Variant detect | Align | Diarize | **Diarize embedder²** | LID | VAD | Streaming | Punc | Registry | Cache |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Rust (`crispasr-sys`) | 65 | ~48% | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Python (`_binding.py`) | 67 | ~49% | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Dart (`flutter/crispasr`) | ~39 | ~29% | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Go (`bindings/go`) | ~54 | ~40% | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Java (JNA) | ~42 | ~31% | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ | ❌ | ✅ | ✅¹ | ✅¹ | ✅¹ |
| Ruby (C ext) | ~30 | ~22% | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅¹ | ❌ | ❌ |
| JS (emscripten) | 18 | ~13% | ❌ | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |

¹ JNA declarations added, idiomatic Java wrapper methods pending.
² **Diarize embedder** column covers the #107 P6 surface:
  `crispasr_speaker_embedder_*_abi`, `crispasr_speaker_cluster_abi`,
  `crispasr_pyannote_cache_*_abi`. Adapters dispatch by spec string
  (`auto`/`titanet`/`indextts`/`ecapa`/.gguf path). Without this
  surface, callers can still run pyannote-only diarization via
  `crispasr_diarize_segments_abi`; the embedder adds globally
  stable speaker IDs across long files.

Rust + Python are the canonical / "full-coverage" wrappers. The other
five track the high-traffic surface (transcribe + TTS) and were swept
together in `4f476c3` (set_speaker_name) and `65e0a61` (set_instruct +
variant detect).

### Capabilities reachable only from C-ABI / Rust / Python

For each, ~3-12 exports + an idiomatic result type per binding:

- **Forced alignment** — `crispasr_align_words`, `align_words_abi`,
  `align_result_*`. Word-level timestamps from a transcript + audio.
- **Diarization (segment-level)** — `crispasr_diarize_segments[_abi]`.
  Speaker segment spans via energy / xcorr / vad-turns / pyannote
  methods. Missing in **Java, Ruby, JS**.
- **Diarization (embedder + clustering)** — `crispasr_speaker_embedder_*_abi`,
  `crispasr_speaker_cluster_abi`, `crispasr_pyannote_cache_*_abi` (#107
  P6). Pluggable speaker-embedding adapters (TitaNet, IndexTTS-BigVGAN
  ECAPA-TDNN) + agglomerative cosine clustering + pyannote-seg cache
  for globally stable speaker IDs across long files. Missing in
  **Java, Ruby, JS** (which also lack the segment-level surface above).
- **Language ID** — `crispasr_detect_language[_pcm]`,
  `crispasr_lid_free_cache`. Pre-transcribe LID for routing.
- **VAD** — `crispasr_vad_segments`, `crispasr_compute_vad_slices`,
  `crispasr_stitch_vad_slices`, `crispasr_vad_remap_timestamp`,
  `crispasr_vad_free`. Standalone VAD + slice stitching.
- **Streaming** — `crispasr_stream_open/feed/get_text/flush/close`,
  `crispasr_stream_run_decode`. Online ASR with a step buffer. (PR #112
  `--stream-punc` is a CLI-orchestration flag, not a library surface
  — wrappers using `crispasr_stream_*` inherit no change.)
- **Punctuation** — `crispasr_punc_init/process/free/free_text`.
  FireRedPunc post-processor.
- **Model registry** — `crispasr_registry_lookup[_abi]`,
  `registry_lookup_by_filename[_abi]`,
  `crispasr_detect_backend_from_gguf`. Backend / file resolution.
- **Cache** — `crispasr_cache_dir_abi`,
  `crispasr_cache_ensure_file_abi`. Auto-download dir + lookup.

### #107 diarize-pipeline binding follow-up (deferred)

The full diarization surface — both the segment-level
`diarize_segments` family and the new #107 P6 embedder + clustering
+ cache primitives — landed in Python, Rust, Dart/Flutter, and Go.
Three bindings still have nothing wired:

- **Java** (`bindings/java/`) — JNI binding currently exposes only
  `crispasr_session_*` (transcription) and `*speaker_name*` (TTS
  preset-voice). Adding diarize means JNI wrappers for
  `crispasr_diarize_segments_abi` + the 9 new `crispasr_speaker_*_abi`
  / `crispasr_pyannote_cache_*_abi` exports plus an idiomatic Java
  helper class. ~250 LOC.
- **Ruby** (`bindings/ruby/`) — only exposes `Session.transcribe`.
  Treats `Segment#speaker_turn_next?` as the only speaker field
  (whisper tinydiarize). Wider diarize surface needs Ruby FFI
  bindings. ~200 LOC.
- **JavaScript / WASM** (`bindings/javascript/`) — emscripten build,
  no speaker surface at all today. Closing the gap depends on the
  WASM build expanding what it links in (pyannote-seg, titanet,
  indextts_voc all need to be in the WASM target). Bigger effort —
  start with `crispasr_diarize_segments_abi` (no model deps beyond
  the existing wasm whisper) and defer the embedder primitives.

Same "when to do this" rule as the rest of #59 applies: open when a
concrete consumer asks. The Python / Rust / Dart / Go quartet covers
the active CrispASR usage today.

### Effort

Per binding ~150-300 LOC (extern decls + idiomatic methods + result
types + smoke test). Five trailing bindings × 9 capability surfaces ×
~30 LOC each ≈ 1.5 kLOC total. Each capability is independent — can
be staged.

Suggested ordering once a consumer asks:
1. Streaming (Go/Java first — common deployment shapes for ASR servers).
2. VAD + alignment (mobile use cases via Dart).
3. Diarization + LID + punctuation (transcription pipelines).
4. Registry + cache (CLI-style consumers).

### When to do this

Not now. The qwen3-tts sweep was justified because PLAN #57 Phase 2
unblocks needed it. Open this section when a concrete consumer shows
up asking for, say, "Java VAD" or "Go streaming". Reference commits
for the pattern: `4f476c3` (TTS surface sweep) and `65e0a61`
(variant detection sweep). Same shape applies to every other capability.

### Follow-up: Rust binding directory location (low priority)

The Rust crates live at the repo root as `crispasr/` (high-level) +
`crispasr-sys/` (FFI). The crate **names** are correct and idiomatic
(`crispasr` / `crispasr-sys`, the `-sys` split, published on crates.io)
— **do not rename them**. The smell is purely the top-level *directory*
`crispasr/`, which visually collides with the repo/project name (a
reader at the root can't tell it's specifically the Rust binding vs the
core). It is, however, consistent with the repo's per-ecosystem
top-level pattern (`python/`, `flutter/`).

Optional cleanup: relocate **both** dirs (they're siblings; the
inter-crate dep is a relative `path = "../crispasr-sys"` and there is no
Cargo workspace, so moving them together preserves the link) under
`bindings/rust/` to match the C-family bindings. **Consumer-safe**:
crates.io consumers resolve by name+version, not in-repo path, so a
move does not break them. Before moving, audit: (a) any downstream repo
using a `git` + `path` dependency on the subdir (e.g. CrispEmbed /
CrisperWeaver), (b) internal CI / `scripts/` / `build_go` refs, (c)
docs path references. Do it deliberately in one commit — never a blind
rename. Not worth churn unless the root-dir ambiguity actively bothers.

---

## 60o. MTLBinaryArchive Metal pipeline cache — open

Parent #60 (cross-backend perf tricks) shipped 60a–g → HISTORY §63 /
§64 / §71 / §75 (madvise WILLNEED, wrap_iface, preload, fused QKV,
KV Q8_0 on 9 backends, mlock, MADV_RANDOM). 60h–n parked. Only 60o
below is still open.

### 60o (OPEN). MTLBinaryArchive Metal pipeline cache

**Status:** OPEN. **Tier 1.** **Effort:** M (~half day source patch
in upstream `ggml/src/ggml-metal/`). **Source:** raised by CrisperWeaver
PLAN §5.18 — the highest-leverage perf item the Flutter app's CI
sweep is currently waiting on.

**Problem.** ggml-metal compiles MSL pipelines lazily for each unique
tensor shape on first use, then caches them in-memory only. Every
fresh process pays 30–60 s of MTLLibrary + MTLComputePipelineState
JIT before the first `ggml_metal_encode` lands. Affects:

* Every `flutter test` / `crispasr` CLI invocation on the dev box
  (~30–60 s startup tax per run).
* Every CI sweep — measured at ~25 min for the single-process
  multi-backend pass; projected ~5 min if pipelines were warm.
* Every end-user app launch on macOS / iOS / iPadOS where pipelines
  are recompiled across the whole loaded model on first transcribe.

**Fix.** Use Apple's first-party `MTLBinaryArchive` API to write
freshly-compiled pipeline state objects to a per-device disk cache
on shutdown and reload them on startup. Same pattern Apple's own
MPS / MLX use. Sketch:

* Patch `ggml/src/ggml-metal/ggml-metal-device.m`:
  - On `ggml_metal_device_init`, attempt
    `[device newBinaryArchiveWithDescriptor:]` from
    `${GGML_METAL_PIPELINE_CACHE}` (default
    `~/Library/Caches/ggml-metal/<device-name>.archive`).
  - When `ggml_metal_compile_pipeline` produces a new
    `id<MTLComputePipelineState>`, also call
    `[archive addComputePipelineFunctionsWithDescriptor:]` so the
    next process can rehydrate it.
  - On exit (or via an explicit `ggml_metal_pipeline_cache_save`),
    `[archive serializeToURL:]` flushes to disk.
* Joins the existing `// CrispASR patch` set in ggml-metal — same
  rebase discipline as the conv_transpose_1d perf patch.
* Cache invalidation: include device name + ggml-metal source
  hash in the archive path so a kernel change auto-busts the cache.

**Risk.** Low — Apple's API is stable since iOS 14 / macOS 11. Worst
case the archive fails to load and we fall back to the existing JIT
path, exactly today's behaviour.

**Why Tier 1.** The 30–60 s saved compounds across every consumer
of CrispASR (CLI, CrisperWeaver, the wrapper bindings, CI). Same
order-of-magnitude impact as 60a (madvise WILLNEED) had on cold-mmap
weight loads.

---

## 65-residual. JS / emscripten word-accessor surface — open

Parent #65 (session-API word-confidence parity) shipped → HISTORY §65
(main batch + vibevoice / moonshine-streaming + gemma4-e2b token-prob
API + Go/Java/Ruby parity in `5534588` + `d963e3a`). Only residual:
JS/emscripten word accessors — leaving until a JS consumer asks (the
current JS binding is TTS-focused).

---

## 61. Feature matrix uplift

The README "Feature matrix" was missing checkmarks for many cells
where the underlying model already supported the feature. Tracker
for closing the remaining gaps.

### 61a-f — **DONE → [HISTORY §65](HISTORY.md)**

| Sub-item | Outcome |
|---|---|
| 61a Auto-download for fc-ctc + wav2vec2 | 2 ✔ |
| 61b Per-token confidence × 7 backends | 7 ✔ (full row, 15/15) |
| 61c Kyutai native + word timestamps | 2 ✔ |
| 61d Best-of-N × 4 LLM-style decoders | 4 ✔ |
| 61e Temperature for omniasr-llm | 1 ✔ |
| 61f Punctuation toggle × 4 LLM-style decoders | 4 ✔ |
| **Subtotal** | **20 cells gained** |

### 61g. Audio Q&A (`--ask`) — DEFERRED

glm-asr is an ASR fine-tune (hardcoded prompt ids, no live
tokenizer for arbitrary instructions); omniasr-llm uses FLORES-200
language conditioning, not chat. Both would need empirical
validation showing the model honours an instruction prompt before
plumbing the toggle. Out of scope until a backend lands that's
actually instruction-tuned.

### 61h. Beam search for LLM family + enc-dec — DONE

_Done — see HISTORY.md + git log._

## 66. Wrapper publishing bootstrap — required before language registries can ship

**Status:** OPEN, auto-trigger silenced. The `tags: ['v*']` push
trigger on `release-wrappers.yml` is now COMMENTED OUT so future tag
pushes don't keep producing red runs while we're not ready to
bootstrap. Workflow stays in the repo on `workflow_dispatch` only —
manual dispatch still works for ad-hoc testing during bootstrap.
Failed on every release since v0.5.0; confirmed again on v0.5.4
(`gh run view 25248028443`).

The CI workflow pushes to three registries automatically on every
`v*` tag, but **none of the packages currently exist on those
registries**:

- crates.io: `crispasr-sys` and `crispasr` do not exist (404).
- PyPI: `crispasr` does not exist (404).
- pub.dev: `crispasr` does not exist.

All three registries require **manual bootstrap** — the first
version of any package can't be published by an OIDC / token CI
flow because the registry has no prior owner record to verify
against. After the first manual publish, automated publishing
takes over via the existing workflow.

### Bootstrap steps (one-time, requires repo admin credentials)

1. **crates.io** (Rust, simplest):
   ```bash
   cargo login   # paste API token from https://crates.io/me
   cargo publish --manifest-path crispasr-sys/Cargo.toml --allow-dirty
   sleep 30   # wait for crates.io index
   cargo publish --manifest-path crispasr/Cargo.toml --allow-dirty
   ```
   Then add `CARGO_REGISTRY_TOKEN` repo secret (Settings → Secrets
   → Actions). Subsequent tag pushes auto-publish.

2. **PyPI** (uses trusted publishing / OIDC):
   - Visit https://pypi.org/manage/account/publishing/ and create a
     pending publisher with:
     - Owner: `CrispStrobe` (or org owning the repo)
     - Repository: `CrispASR`
     - Workflow: `release-wrappers.yml`
     - Environment: `pypi`
   - Push a `v*` tag and the OIDC handshake creates the package.
     (No manual `twine upload` needed — the pending-publisher
     mechanism IS the bootstrap path.)

3. **pub.dev** (Dart, hardest — `dart pub publish` requires a
   logged-in interactive shell for the first version):
   ```bash
   cd flutter/crispasr
   dart pub get
   dart pub publish   # interactive: confirm, log in via browser,
                      # accept the package contents
   ```
   Then visit https://pub.dev/packages/crispasr/admin and enable
   "Automated publishing" with:
   - Repository: `CrispStrobe/CrispASR`
   - Tag pattern: `v{{version}}`

### Resilience improvements landed alongside this entry

`release-wrappers.yml` is updated so when we DO re-enable the
auto-trigger, a single registry's misconfiguration doesn't fail the
whole workflow:

- Auto-trigger on `tags: ['v*']` is currently **commented out**.
  Re-enable by un-commenting the two lines (`push:` /
  `tags: ['v*']`) after bootstrap completes.
- Each job runs a fast secret/config presence check at the top and
  echoes a clear "skipping: registry X not configured" instead of
  letting `cargo` / `twine` emit cryptic auth errors deep in the
  log.
- Each job uses `continue-on-error: true` so the others still try.
- Workflow comment block updated to reference this PLAN section.

After bootstrap + re-enabling the trigger, the next tag push should
publish all three wrappers cleanly.

---

## 67. Deferred follow-ups carry-over (mid-May 2026 session)

Captured here so they don't get lost between sessions.

### 60d F16 mimo-asr re-upload (HF)

The Q4_K fused-QKV file is on HF
(`cstr/mimo-asr-GGUF/mimo-asr-q4_k.gguf`, 4.2 GB). The F16 variant
on HF is still the legacy unfused layout — the runtime fallback
keeps it working but it doesn't get the 1.7× per-step decode that
fused QKV unlocks. Re-conversion needs a fresh BF16→F16 run,
which on this 16 GB / 99%-full-disk box sustained ~0.8 MB/min and
was killed at 22 min (PLAN #51c disk-thrash signature). Run on a
32+ GB box with non-99%-full external. Then
`tools/patch_mimo_asr_fuse_qkv.py` patches it to the fused layout
(~5 min vs hours for a fresh quantize).

### 60e per-backend Q8_0 KV cosine validation

Env wiring (`CRISPASR_KV_QUANT={f16,q8_0,q4_0}`) landed across 9
backends (mimo_asr, qwen3_asr, voxtral, voxtral4b, granite_speech,
gemma4_e2b, glm_asr, omniasr, orpheus, qwen3_tts) — defaults stay
F16 so it's bit-identical until opted in. **Only mimo-asr has been
diff-harness validated at q8_0** (last_hidden 0.963031 vs F16
0.963177; logits 0.981454 vs 0.981261, both ≥0.98 gate). The
remaining 8 backends need their own
`CRISPASR_KV_QUANT=q8_0 crispasr-diff <backend>` pass before any
default-flip per backend.

Effort: ~1 diff-harness run per backend, ~5 min each on warm
cache. Zero code work — wiring is in place.

### Vibevoice CUDA cache reuse re-test

`backend_needs_fresh_pred_graph()` defensively bypasses the
pred-head graph cache on Metal + Vulkan + CUDA (CUDA added on the
"shape suggests it's broken too" presumption). When a CUDA box is
available, run `CRISPASR_VIBEVOICE_REUSE_PRED_GRAPH=1` and confirm
TTS runs without `GGML_ASSERT(src_backend_id != -1)`. If the cache
works there, drop the `CUDA` prefix from the bypass list and
recover the ~30% per-synthesis caching speedup.

If the assert fires, the env hatch stays disabled by default and
the proper upstream-ggml fix (recompute view→backend mapping
from `view_src->buffer` in `ggml_backend_sched_split_graph`)
becomes the next step.

### SYCL / HIP / ROCm cache-bypass extension

Same shape as CUDA — these multi-backend GPU schedulers probably
need the bypass too but no user has reported. Extend
`backend_needs_fresh_pred_graph()` prefix list when a report comes
in or when a kernel maintainer audits the upstream
`ggml_backend_sched_split_graph` reset path on those backends.

### Per-backend `MADV_RANDOM` post-prefill wiring (PLAN #60g)

`core_gguf::mmap_advise_random()` is exposed but no backend calls
it yet. Add a single call between prefill and the decode loop in
`mimo_asr_transcribe`, `qwen3_asr_transcribe`, `voxtral_transcribe`,
etc. when a 32+ GB-box benchmark demonstrates measurable benefit
(on Q4_K the readahead delta is marginal; F16 is where it would
matter, and we can't reliably measure F16 on 16 GB).

### Disk5 cleanup

`/Volumes/backups` sits at 99% full, 30 GB free. The
`/Volumes/backups/ai/crispasr-models/mimo/mimo-asr-q4_k.gguf`
unfused (4.2 GB) is now superseded by `mimo-asr-q4_k.fused.gguf`
and the HF copy of the fused. Safe to delete the local unfused
once future A/B testing isn't needed.

### CI: legacy `build.yml`

`.github/workflows/build.yml` is the legacy whisper.cpp CI matrix
(triggers on `branches: [master]` which doesn't exist + `tags: v*`).
Has been failing on every tag push since v0.4.x. Doesn't block
releases (the new `ci.yml` / `release.yml` are the actual gates).
Either delete or repair when convenient — pending audit on whether
any build-matrix combination there isn't covered by the new
`ci.yml` matrix.

---

## 70. Streaming TTS via chunked VAE decode (latency win, vibevoice / qwen3-tts)

**Effort:** Medium-large.

**Background.** Issue #52 surfaced a chunked-VAE patch from
[`geneing`](https://github.com/CrispStrobe/CrispASR/issues/52#issuecomment-4366745018)
that re-runs the σ-VAE decoder on small chunks of the latent stream
instead of one big graph. Their measurement showed a speed regression
because re-running the ggml graph N times pays per-call setup
(`sched_reset` + `sched_alloc_graph` + the kernel-launch ramp-up) on
every chunk. So that patch isn't useful for the Intel-Arc Vulkan
workgroup-limit bug it was filed against — that's already fixed by
the CPU fallback in `31795a7` / `VIBEVOICE_VAE_BACKEND=cpu`.

**But chunking is the right shape for a latency feature, not a
throughput one.** If we ever want streaming TTS — the listener
starts hearing audio before AR completes — we'll need chunked VAE
*plus* the rest of the pipeline. A `--stream` mode for `--tts` would
look like: emit a 24 kHz PCM chunk every K AR steps, written to
stdout / streamed over HTTP, while the AR loop continues. Time-to-
first-byte drops from "full TTS wall-clock" to "K AR steps + one
chunked-VAE pass."

This is **not the same project as the Intel-Vulkan workgroup fix.**
We'd want a chunked VAE that's well-engineered for latency rather
than borrowed from a workgroup-limit workaround.

### Three pieces required

1. **Persistent VAE compute-graph reused across chunks.** The
   per-call `sched_reset` + `sched_alloc_graph` overhead is what
   killed geneing's prototype's speed. Pattern to mirror is
   qwen3-tts's `O15` graph reuse (see `src/qwen3_tts.cpp:1037`):
   build the graph once at `Lk = max_chunk_latents`, pin the
   tensor topology, reuse the cached gallocr plan across all
   chunk decode calls. Net cost is one `set_rows`-style
   "where to write this chunk's output" op per call, not a full
   rebuild.

2. **Causal padding on the σ-VAE conv stack.** The σ-VAE
   transposed-conv stack has receptive field that crosses chunk
   boundaries — naive chunking will produce phase artefacts at
   the boundaries. Causal padding (left-pad each chunk with the
   previous chunk's tail context, drop the first L padding samples
   from the output) makes the chunk decode equivalent to the full
   decode at chunk boundaries. Reference: kokoro and voxtral4b
   already use causal-conv1d padding for streaming-encoder paths;
   the σ-VAE side has a different topology but the math is the
   same.

3. **Chunked transfer in the HTTP TTS endpoint.** Once #58's
   `POST /v1/audio/speech` lands (vkrmch's PR), wire chunked-
   transfer-encoded audio output for clients with `Accept:
   audio/wav; chunked` (or a `stream=true` request field). cpp-
   httplib has chunked-transfer support out of the box. Without
   this piece the latency win can't reach the network — server
   would compute chunks fast but still wait until the last chunk
   to flush.

### Backends in scope

- **vibevoice TTS** (σ-VAE decoder) — primary target, the patch
  origin. Largest latency win because vibevoice is positioned as
  the realtime TTS backend.
- **qwen3-tts codec decode** — different architecture (12 Hz codec
  vocoder, not a σ-VAE) but the same chunked-decode-with-graph-
  reuse pattern applies. Already has graph reuse via `O15`; would
  extend that to chunked output.
- **kokoro iSTFTNet generator** — different shape again
  (deterministic vocoder, not a diffusion VAE). Chunking is
  cleaner here because the generator is straight-line; harder
  because the iSTFT inverse window has the same boundary
  artefact problem.

Skip out-of-scope: orpheus uses the SNAC codec which already
emits 24 kHz PCM in a single forward pass — chunking has no
latency win there.

### Approach

Pre-work: revisit geneing's
[chunked_vibevoice.patch](https://github.com/user-attachments/files/27326191/chunked_vibevoice.patch)
as a starting point — it nailed the chunking decomposition;
where it gave up was on the per-call overhead. Land the graph-
reuse fix first (mostly mechanical), benchmark to confirm the
regression is gone, then layer in the causal-padding and HTTP
chunked transfer.

### Files touched

- `src/vibevoice.cpp` (and `vibevoice_tts.cpp`) — chunked decode
  path with graph reuse + causal padding
- `examples/cli/crispasr_backend_vibevoice.cpp` — `--stream`
  output path: write each chunk's PCM to `stdout` as they
  complete, instead of buffering and writing one WAV at end
- `examples/cli/cli.cpp` — surface `--tts-stream` flag
- `examples/server/server.cpp` — chunked-transfer wiring for
  `/v1/audio/speech` (depends on #58 landing first)
- `docs/tts.md` — document the new flag + the streaming env
  var(s)
- `LEARNINGS.md` — document the per-call ggml graph overhead
  trap and the graph-reuse cure (geneing's patch is the
  cautionary tale)

### Out of scope for v1

- Multi-chunk look-ahead (lower latency at cost of slightly worse
  boundary behaviour) — a single look-ahead chunk is already a
  meaningful tuning knob; tuning past that adds complexity that
  isn't justified until we measure how good the v1 latency is.
- Non-vibevoice / non-qwen3-tts backends — kokoro / orpheus
  chunking is its own work item if anyone needs it.
- Any changes to AR decoding itself — the AR loop stays
  unchanged; only the post-AR codec / VAE side is chunked.

## 73-follow-up. Long-context cohere FA vs cast-on-read benchmark — DONE (2026-06-04)

Parent #73 (quant-safe KV cache write for canary / cohere / kyutai_stt)
shipped → HISTORY §79. #71 + #72 also there (test-runner under-invocation
+ cap-honesty audit; gemma4_e2b / mimo_asr GPU residency for Q4_K weights
— gemma4 2.2× on M1, mimo-asr -22 %, Linux/CUDA validation deferred).

**Benchmark result (VPS x86 CPU, 2 threads, cohere-transcribe-q4_k.gguf,
FLEURS EN):** flash wins by 26% on 300s audio (820s vs 1115s), loses by
13% on 60s audio (203s vs 179s). Crossover between 1-5 min. Flash
stays as default (`-fa`); short-clip users can opt out with `-nfa`.
Full results in PERFORMANCE.md §5.

## 74. Feature-matrix uplift round 2 — chatterbox family + matrix tooling ✓

After §79b shipped chatterbox + 3 sibling variants and the audit-drift cleanup brought test-all-backends.py to 39/39 backends, four follow-ups surfaced from re-reading the cap matrix. They cluster by user-visible value:

### 74a. Auto-route by `-l <lang>` for chatterbox family — DONE

_Done — see HISTORY.md + git log._

## 75-followups. /v1/audio/speech OpenAI round 2 — open

Parent #75 round 1 shipped → HISTORY §81 (PR #63 merged + corrective
batch + 75a/75b + 75d chunking + 75c-opt-1 server-side speed
resampler). Remaining follow-ups: 75c-opt-2 (native-backend duration
knobs) and 75e (streaming / mp3 / upload).

Remaining gaps documented in follow-up items: 75c-opt-2 (native-backend duration knobs), 75e (streaming response, mp3/opus encoding, voice upload/delete).

---

## 80. nano-cohere-transcribe-inspired perf + chunking tweaks

After studying [Deep-unlearning/nano-cohere-transcribe](https://github.com/Deep-unlearning/nano-cohere-transcribe)
(pure-PyTorch port of `CohereLabs/cohere-transcribe-03-2026`, 1.5–3.6×
faster than the native `transformers` path on CUDA), this entry surveys
which of its tricks port to CrispASR. The headline trick — CUDA-graph
capture of the per-token decoder step — turns out to be largely
redundant on Metal+ggml because PR #73's `sched_reserve` already paid
that bill. The smaller wins are still useful and are the focus here.

### 80a. Decoder graph-once, replay-N — measurement, parked

**Inspiration.** nano-cohere `_graph.py` pre-allocates fixed-shape KV
buffers `[B, H, max_kv, Dh]`, writes new K/V at a runtime `pos_idx` via
`index_copy_`, captures one CUDA graph per `(B, T_enc)`, and replays it
per token. Eliminates per-step kernel-launch overhead (3.62× win at
bs=1 on A100).

**Mapped to ggml.** The structural blocker in CrispASR is
`core/attention.h:148` — the F16 fast path bakes `n_past` into the
view byte-offset (`n_past * kv_k->nb[1]`), so each step rebuilds the
graph at a different topology. The quant path already uses
`ggml_set_rows(indices)` (line 173), the same indirection nano uses;
extending it to F16 is mechanical (verified: Metal/CUDA/Vulkan all
support `ggml_set_rows` with F16 destination). With that change, the
step graph could be built once and replayed N times, only updating
`embd`/`position`/`indices`/`sa_mask` via `ggml_backend_tensor_set`.

**Why this is parked.** Baseline measurement on cohere q4_k + Metal
(MTL0):

| clip            | total wall | enc compute  | dec build | dec alloc | dec compute | dec build+alloc / total |
| --------------- | ---------- | ------------ | --------- | --------- | ----------- | ----------------------- |
| JFK 11 s        | 1356.8 ms  | 1114.3 (82%) |   3.0 ms  |  14.8 ms  |  115.7 ms   | **1.31 %**              |
| JFK ×3 = 30 s   | 3838.6 ms  | 3295.7 (86%) |   5.1 ms  |  28.4 ms  |  293.7 ms   | **0.87 %**              |

The encoder dominates (82–86%). Per-step decoder CPU overhead is
already in the noise on Metal because PR #73's
`ggml_backend_sched_reserve(... max-size graph ...)` (cohere.cpp:2210)
pre-sizes the gallocr so `gallocr_needs_realloc` returns false on
every step. A graph-once-replay-N change would save at most ~1 % of
total wall, with substantial code disruption (constant-shape K/V
read views also need a per-step mask, since `sa_L = offset + n_tokens`
also currently scales the read view).

**Decision.** Park until a CUDA backend ships — at that point the win
flips (CUDA per-kernel-launch overhead is exactly what nano fights),
and the F16 `ggml_set_rows` path may be needed anyway. Cohere already
has the `gf_decode_1` field declared (line 502) for that future
change.

### 80b. Energy-minimum chunk boundaries — DONE

_Done — see HISTORY.md + git log._

## 81. Nemotron-Speech-Streaming-EN-0.6B — first cache-aware streaming-native ASR

[`nvidia/nemotron-speech-streaming-en-0.6b`](https://huggingface.co/nvidia/nemotron-speech-streaming-en-0.6b)
— NVIDIA Open Model License (NVOML), 600 M params, released
late 2025 / early 2026. **Cache-Aware FastConformer + RNN-T**, the
first model in the queue that's *streaming-native* rather than
batch-with-chunked-streaming-on-top. Mentioned by an outside reporter
in issue #85; `parakeet` and `kyutai-stt` are the closest
streaming-capable backends we ship today and neither targets
nemotron's latency/accuracy frontier.

### Why this is interesting

Quality-vs-latency curve from the published Open ASR Leaderboard
numbers (same eval set as our existing parakeet / canary / whisper):

| Chunk size | Right-context lookahead | Avg WER | Notes |
|---|---:|---:|---|
| 1120 ms | 13 frames (~1.04 s) | 6.93 % | best accuracy |
| 560 ms | 6 frames (~0.48 s) | 7.07 % | |
| 160 ms | 1 frame (~0.08 s) | 7.67 % | |
| 80 ms | 0 frames | 8.43 % | unique low-latency point |

Same `.gguf` for all four — `att_context_size=[70, R]` is a runtime
knob, not a retraining artifact. Reference points on our current
leaderboard:

| Model (batch) | Avg WER | RTFx |
|---|---:|---:|
| `parakeet-tdt-0.6b-v3` (we ship this) | 6.34 % | high |
| `nemotron-streaming-en-0.6b` (1.12 s chunk) | 6.93 % | streaming |
| `canary-1b-v2` (we ship this) | 7.15 % | 749 |
| `whisper-large-v3` (we ship this) | 7.44 % | 145 |

Headline read: nemotron is **0.6 pp worse than batch parakeet on
average but better than canary-1b-v2 and whisper-large-v3** — and
crucially gets there with a fixed-size step rather than reading the
whole utterance. On AMI (conversational meetings) it actually wins
all of those (11.73 % vs 11.31/16.01/15.95). The 80 ms / 0-lookahead
/ 8.4 % WER point has no equivalent in our current lineup; it's the
real reason to consider this model.

### License — NVIDIA Open Model License (NVOML)

Source-available, **not** OSI-open-source.

- Commercial + non-commercial use ✅
- Derivatives + fine-tunes ✅ (you own them)
- Redistribution ✅ with attribution: every copy must include a
  `Notice` file containing *"Licensed by NVIDIA Corporation under
  the NVIDIA Open Model License"*
- No explicit field-of-use, **but** subject to NVIDIA's external
  "Trustworthy AI" terms
- Patent litigation = automatic termination
- Bypassing safety guardrails (without a "substantially similar
  Guardrail") = automatic termination
- NVIDIA claims no rights in outputs

For our purposes (publishing `cstr/nemotron-speech-streaming-en-0.6b-GGUF`):
legally fine — same shape as Llama / Cosmos redistributions. Just
need the NVOML attribution `Notice` in the HF README. Less
permissive than `parakeet-tdt-0.6b` (CC-BY-4.0) so downstream users
inherit the NVOML, not Apache.

### Architecture summary

```
WAV (16 kHz mono)
  → log-mel (80 bins, NeMo per-feature norm)
  → 4× conv subsampling pre-encode
  → 24× Cache-Aware FastConformer block
       · macaron FFN
       · multi-head SA w/ rel-pos shift  ← cache-aware (cached K/V from prior chunks)
       · depth-wise conv (kernel 9)      ← cache-aware (8-frame overlap-cache per layer)
       · macaron FFN
  → predictor LSTM (1-layer)
  → joint network + softmax
  → RNN-T blank/non-blank greedy (no TDT durations)
```

Frame stride after the 4× subsampling is 80 ms. Native PnC
(punctuation + capitalization). 530 k hours of training data
(NVIDIA Riva ASR set + Granary, including YouTube-Commons 109 k h,
YODAS2 102 k h, LibriLight 49 k h, Mosel 14 k h plus the standard
LibriSpeech / Fisher / WSJ / VoxPopuli / MLS / Common Voice /
Earnings22 mix).

### What we already have (~60–75 % reuse)

| Piece | Status | Where |
|---|---|---|
| FastConformer encoder body (24L, macaron FFN, SA + rel-pos, DW conv) | ✅ ready | `src/core/fastconformer.h` (shared by parakeet, canary, canary_ctc) |
| 4× conv subsampling pre-encode | ✅ ready | `core_conformer::build_pre_encode` |
| RNN-T predictor LSTM + joint head + greedy decode | ✅ ready (as TDT, where pure RNN-T = `n_tdt_durations=0`) | `src/parakeet.cpp` |
| Log-mel preprocessor (NeMo-style, 80 bins, per-feature normalization) | ✅ ready | `src/core/mel.cpp` |
| KV-cache infrastructure (`kv_cache_write` + offset-indexed reads) | ✅ ready | `src/core/attention.h` |
| Streaming CLI pipeline (`--stream`, `--stream-json` after #84) | ✅ ready | `examples/cli/crispasr_run.cpp` |
| BPE tokenizer (NeMo SentencePiece) | ✅ ready (parakeet pattern) | `src/core/bpe.h` |
| Streaming-aware backend example (per-layer state, chunk-by-chunk graph rebuild) | ✅ partial | `src/moonshine_streaming.cpp` |
| GGUF converter for NeMo `.nemo` checkpoints | ✅ partial | `models/convert-parakeet-to-gguf.py` template |

### What's missing — the actual new work

1. **Cache-aware FastConformer encoder graph.** Existing
   `core_conformer::build_block` consumes the whole `T` and emits
   the whole `T`; for streaming we need a variant that takes
   `(cached_K, cached_V, conv_state)` per layer and emits new K/V +
   new conv state alongside the audio output. Probably lives next
   to `fastconformer.h` as `fastconformer_streaming.h` so we don't
   regress parakeet/canary's bit-identical batch graphs.
2. **Per-layer streaming state on the context.** Modeled on
   `moonshine_streaming`'s pattern — per-layer K/V tensors persisted
   across `transcribe()` calls.
3. **`att_context_size` runtime knob.** Trivial — controls how many
   frames feed the encoder per step and the left-cache trim policy.
   No retraining, just masking + cache management differ.
4. **`.nemo` → GGUF converter.** ~80 % shared with parakeet's
   converter; cache-aware blocks have the same tensor names plus
   new metadata (`encoder.streaming.att_context_left_frames`,
   `encoder.streaming.dw_conv_state_size`, etc.).
5. **`examples/cli/crispasr_backend_nemotron.cpp`.** Thin adapter
   driving the streaming encoder per chunk and running the existing
   RNN-T greedy decode on each chunk's output. Shape is between
   `crispasr_backend_parakeet.cpp` and
   `crispasr_backend_moonshine_streaming.cpp`.
6. **`--live` integration.** Once the backend exists, `--stream` and
   `--stream-json` (#84) Just Work. The new backend would be a much
   better fit for `--live` than the current chunked-batch backends
   because per-chunk cost is constant rather than `O(window²)`.

### Effort breakdown

| Component | LOC | Reuse |
|---|---:|---|
| 80-bin log-mel front-end | ~30 | full reuse from parakeet |
| Cache-aware SA layer (K/V append + trim, rel-pos shift on `[cache | new]`) | ~120 | **new** — conceptually small but graph-topology-sensitive |
| Cache-aware DW conv layer (last 8 frames cached) | ~80 | **new** — pattern is identical to RNN state, just per-layer |
| Per-layer streaming state struct + lifecycle | ~60 | **new** — `nemotron_stream_state` on context |
| `att_context_size=[70,R]` runtime knob | ~30 | **new** — masking + trim policy |
| RNN-T predictor + joint + greedy (no durations) | ~50 | full reuse from parakeet |
| `.nemo` → GGUF converter | ~200 | ~80 % parakeet template + new streaming metadata |
| Diff harness reference (mel, enc per chunk, predictor, joint) | ~150 | parakeet template |
| Backend wrapper for main CLI | ~250 | midway between parakeet + moonshine_streaming |
| HF README + NVOML notice + auto-download registry entry | ~80 | template |
| **Total** | ~**1000–1300 LOC** | smaller than #58 (no LM body, no DeepStack injection) |

Headline new helper: a **streaming-aware FastConformer block builder**
that's reusable for any future cache-aware NeMo model (the same
streaming protocol covers Parakeet-Streaming, Canary-Streaming,
FastConformer-CTC-streaming if NVIDIA ever ships those — they're
rumored in the NeMo 26.x roadmap).

### What we'd need to dump from the Python ref for the diff harness

Stage taps:
- `mel_in` `[T_mel, 80]` — first chunk only
- `enc_pre_encode` `[T_enc, 512]` — after 4× subsampling
- `enc_block_0_post_sa` `[T_enc, 512]` — after first SA block
  (validates rel-pos + cache concat)
- `enc_block_0_post_conv` `[T_enc, 512]` — after first DW conv
  (validates conv-state cache)
- `enc_out` `[T_enc, 512]` — final encoder output for the chunk
- `predictor_state_after_blank` `[1, 640]`
- `joint_logits_step0` `[1, vocab+1]`

Six stages — a touch lighter than parakeet's diff harness because we
share the mel front-end. Multi-chunk validation needs the diff
harness to run twice with carried state (or a third entry point that
takes a list of chunks); the current `crispasr-diff` is single-shot
so this is a small extension to the harness itself.

### Risks / open questions

1. **Cache layout in the GGUF.** NeMo's `.nemo` archive embeds the
   streaming config but not the runtime cache shape. Need to pin
   the cache tensor layout (per-layer `[Dh, 70, n_heads]` for SA K
   and V, `[8, d_model]` for DW conv state) and check it survives
   the converter round-trip.
2. **Rel-pos shift on `[cache | new]`.** ~~The existing
   `core_conformer::rel_shift` operates on a square `(2T-1, T)`
   tensor~~ **SOLVED 2026-06-15.** The asymmetric rel_shift uses the
   same stride-trick formula as the symmetric case:
   `view_3d(BD_raw, T_full, T_new, H, s1-s0, s2, (T_new-1)*s0)`.
   Implemented in `nemotron_build_block_streaming`.
3. **Decoder state across chunks.** RNN-T predictor is autoregressive
   over emitted tokens, not over time; its LSTM state carries
   forward across chunks naturally. Confirm with the Python ref
   that emit-token-then-blank-to-advance still works at chunk
   boundaries (the joint sees encoder output for the current
   chunk; the predictor sees its state from the last emitted
   token, which may be from a previous chunk).
4. **Runtime tradeoff at 80 ms.** A 5-chunk window of left context
   + 1 frame new = 71 frames per encoder forward. On Apple Silicon
   Metal the per-launch overhead at that small a `T` may dominate
   — measure before claiming the 8.4 % WER point is actually usable
   live. CUDA path likely fine (graph capture amortizes the launch
   cost the way nano-cohere relied on, see #80a).
5. **PnC-stripped vs PnC-emitting WER comparison.** Open ASR
   Leaderboard scoring strips PnC; the 6.93 % avg is on stripped
   text. Our `firered_punc` post-processor adds PnC to backends that
   don't emit it natively — for nemotron we'd want to *not* run it
   (the model emits PnC already and re-punctuating can hurt). Wire
   a backend capability flag so `--punctuation` is a no-op when the
   backend is PnC-native.

### When to do this

**Un-deferred 2026-06-12** — dictation use case is the concrete
demand. The streaming pipeline has had weeks of real use since
`--stream-json` (#84) landed. The 80 ms / 0-lookahead / 8.4 % WER
operating point has no equivalent in our lineup and is the right
fit for live dictation where sub-200 ms latency matters.

**Realistic estimate 3–5 days of focused work** for someone who's
already touched parakeet/canary, plus benchmarking against the
upstream Open ASR Leaderboard table to confirm parity on the
published WERs. The model is `nemotron-3.5-asr-streaming-0.6b`
(multilingual, 39 langs incl. de-DE, OpenMDW-1.1 license).

**2026-06-13 progress:** Scaffold done (converter, runtime, CLI, registry).
Pre-encode validated cos=1.0. Root cause of empty output identified: the
model is **streaming-only** (`att_context_style=chunked_limited`). Running
full bidirectional attention produces out-of-distribution activations
where blank wins at every frame. The next step is implementing the
cache-aware chunked encoder: per-layer K/V cache (56 frames left context)
+ conv state (8 frames) + attention masking to limit context window.
Kaggle diff harness (v8) uploaded ref GGUF to `cstr/nemotron-3.5-asr-streaming-GGUF`.

**2026-06-14 progress:** Fixed tensor name mismatches, prompt_id mapping,
mel frame count, pre-encode causal padding. Confirmed via Kaggle NeMo
ground truth that encoder output range [-0.91, 0.51] matches exactly
and NeMo's decoder produces correct text. Per-frame values diverged
due to the conformer conv module bug.

**2026-06-15 — WORKING.** Root cause found and fixed: **GLU gate/value
swap** in the conformer conv module. `ggml_siglu` does
`sigmoid(first) * second` but NeMo's `glu` does `first * sigmoid(second)`.
Fix: `ggml_siglu` → `ggml_siglu_swapped`. One-line change.

Output: "And so my fellow Americans ask not what your country can do for
you. Ask what you can do for your country." — matches NeMo exactly.

Additional fixes along the way:
- Tensor names: conv.bn→conv.ln, prompt_kernel.linear1→prompt_kernel.0
- Prompt_id: NeMo uses en-US=0 (not alphabetical index 7)
- Mel: drop_last_frame=false (NeMo keeps all frames)
- Pre-encode: ggml_pad_ext for true causal padding, F32 weights in converter
- Chunked_limited attention mask (chunk-based, not banded)

**Cleanup done (2026-06-15):**
- Debug prints removed, Q4_K dequant support added to tensor_to_f32
- F16 and Q4_K both produce correct text (Q4_K ~2x faster: 14s vs 24s)
- Fixed GGUFs uploaded to HF, broken ref GGUF deleted
- Streaming block GLU also fixed (ggml_siglu → ggml_siglu_swapped)
- Bidirectional+mask path is now the default (no NEMOTRON_BATCH needed)
- Worktree cleaned up, HF README updated with Q4_K recommendation
- German tested (works on DE audio; EN audio with DE prompt gives
  expected degradation)

**2026-06-15 streaming + sched:**
- Sched migration done (gallocr → ggml_backend_sched) — see §168
- Streaming encoder architecture complete: `nemotron_build_block_streaming`
  with cache_last_channel, asymmetric rel-pos bias (stride-trick rel_shift),
  Q-from-new / KV-from-context, causal conv padding
- `CRISPASR_NEMOTRON_STREAMING=1` + `CRISPASR_NEMOTRON_CONTEXT_PRESET=N`
  env vars for A/B testing (0=R3/chunk4, 3=R13/chunk14)
- Quality problem: streaming output diverges from full-sequence by frame 10;
  preset 3 gives recognizable text, preset 0 gives blank. See §168 for
  root cause analysis.
- 3 live integration tests added (init, JFK, F16/Q4_K parity)

**2026-06-15 — STREAMING FIXED (`7f4feff9`).** Root cause: conv module
zero-padded K-1=8 frames instead of prepending cached pre-DW-conv signal.
NeMo's `CausalConv1D.update_cache` does `torch.cat([cache, x])`.
All 4 presets now produce correct text:
- Preset 0 (R=3, 160ms): "And so, my fellow Americans..."
- Preset 3 (R=13, 1120ms): "And so, my fellow Americans..."
- F16 preset 0: "And so my fellow Americans..."

**Remaining:** WER benchmarking on standard test sets, GPU perf testing,
consider making streaming the default path.

---

## 168. GPU scheduler migration — gallocr-only backends

**Status:** partially done (4/7 migrated, streaming encoder incomplete).

### Completed (2026-06-15, commits `f3a57d7c`, `d393b506`, `8f481aa2`)

Four backends migrated from `ggml_gallocr` to `ggml_backend_sched`:
- **nemotron** — encoder, chunked encoder, pre-encode graph
- **paraformer** — encoder, decoder
- **dia_tts** — encoder, cross-attn, decoder loop, DAC decode
- **outetts_wavtok** — decode graph

All verified: identical output on CPU, ASR roundtrip for TTS, 435 unit tests pass.

### Second batch (2026-06-15, commit `f748b94d`)

Two more backends migrated:
- **audioseal** — generator encode + detect graphs
- **lfm2_audio** — encoder, backbone, adapter graphs; F16 verified

**lfm2 Q4_K finding:** the published `lfm2-audio-1.5b-q4_k.gguf` was quantized
with an older quantizer that Q4_K'd `lfm.embed_tokens` (should be F16). Even
after re-quantizing with correct rules (embed_tokens F16), Q4_K still produces
0 tokens — the hybrid conv+attention backbone is too precision-sensitive. **Q5_K
works** and gives identical output to F16. Need to re-publish GGUFs on HF with
Q5_K as minimum quant.

### Remaining gallocr backends

| Backend | gallocr | init_best | Status |
|---------|:-------:|:---------:|--------|
| `voxcpm2_tts` | 27 | ✅ | persistent gallocr with reserve — complex migration |

### Nemotron streaming encoder — QUALITY PROBLEM

The streaming (chunked) encoder path is architecturally complete:
- `nemotron_build_block_streaming`: FFN1 new-only, Q-new/KV-context,
  causal conv, FFN2+LN new-only, cache_last_channel
- Asymmetric rel-pos bias via stride-trick rel_shift (same formula as
  symmetric, offset = T_new-1 instead of T-1)
- Gated by `CRISPASR_NEMOTRON_STREAMING=1`

**FIXED (2026-06-15, `7f4feff9`).** Root cause was hypothesis #3: the conv
module zero-padded the left by K-1=8 on every chunk instead of prepending
the cached pre-DW-conv signal from the previous chunk. NeMo's
`CausalConv1D.update_cache` does `torch.cat([cache, new_x], dim=-1)` — our
code was using `ggml_conv_2d_dw_direct(..., pad_left=K-1)` which fills zeros.

Fix: added `conv_cache_in` / `conv_cache_out` to the streaming block. Each
layer stores the last K-1 frames of the post-GLU signal and prepends them
on the next chunk. All 4 context presets now produce correct text.

### Env vars added

| Var | Effect |
|-----|--------|
| `CRISPASR_NEMOTRON_STREAMING=1` | Enable streaming chunked encoder |
| `CRISPASR_NEMOTRON_CONTEXT_PRESET=N` | Attention context preset (0-3) |
| `CRISPASR_NEMOTRON_NO_WINDOW_MASK=1` | Disable banded attention mask |
| `CRISPASR_NEMOTRON_DEBUG=1` | Encoder/decoder debug prints |

---

## 86. Per-backend flash-attention wiring (CrisperWeaver-driven)

**Status:** open. Plumbing complete in 0.6.2 (commit `ff5536a6`),
kernel-level wiring per backend is the remaining work.

### Context

CrisperWeaver's *Settings → Performance → ASR flash-attention*
toggle ships via `crispasr_session_open_with_params` (open params
struct v2 added in 0.6.2). The toggle threads through to a thread-
local `g_open_flash_attn_tls` that every backend's init arm can
read to set `cparams.flash_attn` on its context_params struct.

**Today only whisper actually consumes the flag at the kernel
level** — `whisper_context_params` already has a `flash_attn` field
that whisper.cpp branches on internally (it switches to
`ggml_flash_attn_ext` for the QKV → softmax → V product when set).

Other backends accept the toggle but their compute graphs still
build the historical `ggml_soft_max_ext(KQ)` path. For users on
Metal, that's a measurable perf gap on every long-running LLM
backend (orpheus, voxtral, qwen3 ASR, qwen3-tts, granite-speech,
chatterbox-T3, gemma4-e2b).

### Per-backend status (updated 2026-05-23)

**RESOLVED.** All backends now route through shared core modules
(`core_attn::kv_self_attn`, `core_attn::encoder_self_attn`,
`core_sanm::build_block`, `core_conformer::build_block`) that
unconditionally use `ggml_flash_attn_ext`. The table below was
written before the core helpers were consolidated; by the time
individual wiring was attempted (May 2026), every backend already
had flash attention via its core module.

| Backend | Core module | Flash attn | Status |
|---|---|---|---|
| whisper | upstream whisper.cpp | ✅ | DONE (upstream) |
| parakeet | `core_conformer::build_block` | ✅ | DONE |
| canary | `core_attn::kv_self_attn` | ✅ | DONE |
| qwen3 (asr) | `core_attn::kv_self_attn` | ✅ | DONE |
| cohere | `core_attn::kv_self_attn` | ✅ | DONE |
| granite_speech | `core_attn::kv_self_attn` + `core_conformer_ibm` | ✅ | DONE |
| voxtral | `core_attn::kv_self_attn` | ✅ | DONE |
| voxtral4b | `core_attn::kv_self_attn` + `encoder_self_attn` | ✅ | DONE |
| vibevoice | `core_attn::kv_self_attn` | ✅ | DONE |
| qwen3_tts | `core_attn::kv_self_attn` | ✅ | DONE |
| orpheus | `core_attn::kv_self_attn` | ✅ | DONE |
| kokoro | `core_attn::encoder_self_attn` | ✅ | DONE |
| chatterbox | `core_attn::kv_self_attn` | ✅ | DONE |
| funasr | `core_sanm::build_block` + `core_attn::kv_self_attn` | ✅ | DONE |
| sensevoice | `core_sanm::build_block` | ✅ | DONE |
| paraformer | `core_sanm::build_block` + manual cross-attn | ✅ | DONE |
| t5_translate | manual (T5 rel-pos bias) | ❌ | N/A — T5 additive bias incompatible with fused kernel |

No further work needed. The original "recommended order" is moot.

### Effort estimate

~4–6 hours per LLM backend (orpheus/voxtral/qwen3/granite/chatterbox),
~2 hours per Conformer (parakeet/canary/cohere). Total realistic
sweep: 2–3 focused days, can be split across separate PRs since
each backend is independent.

---

## 87. `gpu_backend` runtime selector (multi-backend ggml build)

**Status: DONE** (2026-07-02, issue #214, commit 9a26976a).

ggml's `ggml_backend_load_all()` + device registry made `#ifdef` chains
unnecessary. Created `src/core/gpu_backend_pref.h` with
`crispasr_init_gpu_backend()` as drop-in for `ggml_backend_init_best()`.
Process-global preference set at CLI startup via
`crispasr_set_gpu_backend_pref()`, filters registered devices by
case-insensitive name prefix ("vulkan" → "Vulkan0"). Falls back to
`ggml_backend_init_best()` when no preference or no match.

Replaced all 60+ `ggml_backend_init_best()` call sites mechanically.
Also patched `whisper_backend_init_gpu()` in `crispasr.cpp`.
C API: `crispasr_set_gpu_backend()` in `crispasr_session.h`.

Kaggle P100 validation: 7/7 ASR backends pass, `--gpu-backend cpu`
3.6× slower than CUDA with zero CUDA library references.

---

## 90. Session-API beam_size — per-backend wiring

May 2026:
  * Shipped `crispasr_session_set_beam_size` (commit 958e6bd7).
  * Whisper consumes it natively (switches sampling strategy to
    BEAM_SEARCH with the supplied width).
  * **Five backends wired** via runtime `<backend>_set_beam_size`
    setters in their per-backend C API:
    - kyutai-stt (also kyutai / moshi-stt aliases) — setter
      pre-existed; just needed to be called from
      `transcribe_single`.
    - moonshine — same.
    - omniasr (LLM variant; CTC ignores) — same.
    - **glm-asr** — added `glm_asr_set_beam_size` (new public
      symbol) + dispatch wire.
    - **firered** — added `firered_asr_set_beam_size` (new
      public symbol) + dispatch wire.
  * CrisperWeaver's batch worker pool drives all six (whisper +
    five) end-to-end via its sticky-setter protocol; beamSearch
    is pool-eligible across that whole set.
  * `nm libcrispasr` confirms all five `<backend>_set_beam_size`
    plus the unified `crispasr_session_set_beam_size` are
    exported.

**DONE (2026-05-23, commit `0c24178e`).** All three remaining backend families wired:

| Backend | Approach |
|---|---|
| qwen3-asr | Beam branch in `crispasr_c_api.cpp` session path: replay via `qwen3_asr_embed_tokens` + `qwen3_asr_run_llm_kv`; `kv_reset` after beam search. |
| granite / granite-4.1 / granite-4.1-plus / granite-4.1-nar | Same pattern: `granite_speech_embed_tokens` + `granite_speech_run_llm_kv`; `kv_reset` after beam. |
| voxtral | `run_voxtral_family` gained a `beam_size` param; decode-piece logic factored into a shared `decode_piece` lambda (U+2581→space detokenisation) used by both beam and greedy paths. |

`voxtral4b` uses a streaming path, not `run_voxtral_family` — not in scope for this item.

**Parakeet TDT/RNNT beam search shipped 2026-06-01 (`b3cdcebd`, issue #136).**
Label-looping beam with per-beam LSTM state snapshots and per-beam
hotword trie tracking. Wired via `parakeet_set_beam_size()` C API
and `--beam-size` / `-bs` CLI flag. Overhead: ~3 % at beam=2, ~12 %
at beam=4 (encoder-dominated pipeline). See §139 for remaining gaps.

`s->beam_size == 1` (default) keeps the existing greedy path bit-identical; no regression.

**Functional regression test added (2026-05-30).**
`tests/test-session-beam.cpp` — Catch2 test with two tiers:
  - `[unit][beam]` — setter API (null guard, width clamping). No model.
  - `[beam][.live]` — end-to-end via session API. Gated on
    `CRISPASR_MODEL_WHISPER` / `CRISPASR_MODEL_GLM_ASR` env vars.
    Verifies: beam_size=1 byte-identical to default (no-regression),
    beam 2–4 produce non-empty well-formed output on jfk.wav.

---

## 91. CrispASR CLI features missing from CrisperWeaver

While auditing the feature matrix for §90 we found a handful of
CLI knobs CrisperWeaver doesn't expose. Tracked here so the
gap is visible:

* `--offset-t MS` / `--duration MS` — process only a time window
  of the audio. Useful for "transcribe minute 5–10" workflows.
  Needs an engine-side `audio[t0:t0+d]` slice + timestamp shift
  (similar mechanics to the existing resume-offset routing).
  Estimate: 1 day end-to-end (CrispASR Dart binding + UI).
* ~~`--alt N` / `--alt-n N`~~ — alternative-candidate tokens —
  **shipped May 2026 (0.5.13 + CrisperWeaver §5.8)**. Whisper
  internals carry a parallel `alts` vector on `whisper_segment`
  (mirrored at every `tokens.{clear,push_back,resize}` site
  through the fallback-temperature loop, `result_len`
  truncation, and `max_len` wrap-segment splitter). New
  `wparams.alt_n` (default 0 = off). Capture happens inside
  `whisper_sample_token` — beam search is excluded because
  siblings are beam-conditional rather than greedy
  alternatives. Six new public getters
  (`whisper_full_get_token_n_alts` / `_alt_id` / `_alt_p` +
  `_from_state` variants); new C-ABI for both the low-level
  (`crispasr_token_n_alts` / `_alt_id` / `_alt_p` /
  `_alt_text`) and the unified session result
  (`crispasr_session_result_word_n_alts` / `_alt_text` /
  `_alt_p`); sticky session setter
  `crispasr_session_set_alt_n`. The whisper session-transcribe
  path now also populates `seg.words` via
  `emit_words_from_tokens` (it previously emitted only
  segment text — closing a long-standing gap with the parakeet
  / canary backends as a side benefit). Dart 0.5.13 + smoke
  test pinned. CrisperWeaver surfaces this as
  `AdvancedOptions.altN` (0..5 slider in the Whisper-only
  section) and a tap-to-pick chip row in the segment edit
  dialog.

  Deferred follow-ups (low priority — v1 covers the common
  case):
  - **Beam-search alt capture.** Siblings ≠ greedy
    alternatives; needs a different capture path and a
    different chip-walk UX. Defer until a user actually asks.
  - **Full word-level alt enumeration.** Sub-word BPE means
    multi-token words ("kubectl" → `["kub","ect","l"]`) only
    surface alts for the first content token. Whole-word
    alternates would need a per-word token-tree expansion —
    not free; v1's first-token alts already catch most real
    mishears.
  - **Widget test for the alt-picker popover** (CrisperWeaver
    side; Riverpod + l10n scaffolding nontrivial).
  - ~~**Live end-to-end test**~~ — **shipped** as
    `flutter/crispasr/test/alt_tokens_live_test.dart`. Opens a
    session against `ggml-tiny.en.bin`, sets `altN=3`,
    transcribes `samples/jfk.wav`, and asserts ≥1 word has
    alts, p ∈ [0, 1] and descending, chosen token excluded,
    `setAltN(0)` actually clears on the next decode. Tagged
    `live` so model-less CI still passes. On the dev box
    whisper-tiny gives 22/22 words runner-ups on JFK with
    real morphological alternatives like "Americans →
    America / americ / American".
* Whisper decoder fallback knobs (`--word-thold`,
  `--entropy-thold`, `--logprob-thold`, `--no-speech-thold`,
  `--no-fallback`, `--temperature-inc`) — already in the Dart
  binding's TranscribeOptions, just not exposed in
  CrisperWeaver's Advanced Options widget. Trivial — half a day
  to add the UI rows + localised strings.
* Subtitle line formatting (`--max-len`, `--split-on-word`,
  `--split-on-punct`) — whisper context-params field today;
  CrisperWeaver formats post-hoc instead. Quality-of-life win
  for SRT export. ~1 day.
* Token suppression (`--suppress-nst`, `--suppress-regex`) —
  niche; whisper-specific.
* `--carry-initial-prompt` — sticky vs reset behaviour for the
  initial prompt across segments. Edge case, ~1 hour.
* `--print-confidence` — per-token confidence in JSON / WTS
  exports. Segments already carry a `confidence` field;
  exporters could surface per-token.

None of these are blocking; they're listed so the next
parity-pass audit doesn't have to re-discover them.

---




## 92. All-backend regression suite (nightly CI)

**Status:** 32 ASR + 21 TTS regression entries, 0 PLACEHOLDERs.
CI workflow at `.github/workflows/regression.yml` runs nightly + on PR
(smoke-only). Matrix: 22 ASR + 7 TTS = 29 backends on GH free tier.
**First nightly run (2026-06-16):** smoke+preflight+14 backends PASS.
6 failures diagnosed and fixed: 4 decode-drift backends got WER
tolerance (`transcript_tolerance`), 1 bad revision SHA corrected,
1 HF rate limit (transient). Re-triggered; expecting full green.
Next: flip `skip_diff` on backends where ref archives exist, promote
to release gating.

**Why:** the ggml-assertion-hardening regression in 0.6.x cycle
demonstrated that we silently inherit upstream behaviour changes —
both in ggml and in HF-hosted weights — without any test catching
them. A nightly regression that pins every artifact to a specific
revision SHA closes that gap.

**Architecture (shipped, see `tests/regression/`):**

```
manifest.json    per-backend pins: GGUF revision + reference path
                 + expected transcript + cosine thresholds
run_one.py       driver: HF download (pinned) → crispasr (assert
                 transcript) → crispasr-diff (assert cos_min)
regression.yml   nightly cron at 04:00 UTC, matrix per backend
```

Reference dumps live in
[`cstr/crispasr-regression-fixtures`](https://huggingface.co/cstr/crispasr-regression-fixtures);
the `fixtures.revision` SHA in `manifest.json` pins the whole
fixture set together so a re-dump can't silently shift CI's
expectations.

**Next steps (each ~1 hour per backend):**

1. **Add parakeet-tdt-0.6b-v3** (English). Need to cache the
   `.nemo` source locally first; `nvidia/parakeet-tdt-0.6b-v3`
   isn't downloaded on the dev box right now.
2. **Add canary** + **cohere** + **kyutai-stt** + **moonshine**.
   All have reference modules in `tools/reference_backends/`.
3. **Add the TTS family** (kokoro, indextts, qwen3-tts, chatterbox,
   vibevoice). These need WAV-output checksums or an
   ASR-roundtrip rather than transcript equality.
4. **Promote to release gating.** Once stable, hook into
   `release.yml`'s pre-publish job so a regression aborts the
   tag.

**Time budget per nightly run:** ~5-15 min per backend (download
+ build cache hit + run). With matrix fan-out, wall time stays
under 30 min for the full suite.

**Storage:** fixtures repo grows by ~1-50 MB per backend. After
20+ backends still well under 1 GB. The GGUF cache lives in
`$RUNNER_TEMP` and is discarded with the runner — no GitHub
Actions cache eviction concerns.

**Cost on free tier:** public-repo Actions minutes are unlimited,
so the only constraint is wall-clock fairness on the shared
runner pool. Nightly cadence is the sweet spot — catches
regressions within 24 h without burning capacity that would
delay PR feedback.




## 93. CMake target rename: `crispasr` → `crispasr-lib`

**Status:** DONE (commit `11148b23`).

**Why:** the CMake target `crispasr` produces the **library**
(`libcrispasr.so`), while the CLI **binary** is produced by target
`crispasr-cli` (which outputs `bin/crispasr`). The target name vs.
output binary name divergence is a long-standing trap that has now
caused two CI regressions this session:

  - GH regression workflow run 25735206584 — built only the library,
    found `bin/crispasr` absent (fixed in commit `08d1872f`).
  - Kaggle `crispasr-regression-suite` run on 2026-05-12 14:34 —
    same root cause (fixed in this commit, applied to
    `tools/kaggle/crispasr-regression.py`).

Both fixes are one-line and obvious **after** you know — but the
trap reliably re-bites anyone writing a new CI workflow because
the natural mental model is "target `crispasr` builds the
`crispasr` binary." Renaming the library target makes the
distinction explicit.

**Plan:**

1. `src/CMakeLists.txt`: rename `add_library(crispasr ...)` →
   `add_library(crispasr-lib ...)`. Update every
   `target_link_libraries(... crispasr ...)` and any
   `target_*(crispasr ...)` call elsewhere in the tree.
2. Preserve the **output binary name** `libcrispasr.{so,dylib,a}`
   via `set_target_properties(crispasr-lib PROPERTIES OUTPUT_NAME
   crispasr)` — no .so/.dylib filename change, no ABI break.
3. Update consumers that use the CMake target name:
   - `bindings/go/whisper.go` cgo LDFLAGS (currently `-lcrispasr`,
     unchanged since OUTPUT_NAME stays `crispasr`).
   - `bindings/ruby/ext/dependencies.rb` graphviz walk (queries by
     target name `crispasr` — needs a 1-line update).
   - `.github/workflows/{ci,release,regression}.yml` `--target`
     args (mostly already use `crispasr-cli` for the CLI binary,
     but any `--target crispasr-lib` referring to the library needs
     the rename).
   - `tools/kaggle/crispasr-regression.py` similarly.
4. Add a CMake alias for one release cycle:
   `add_library(crispasr ALIAS crispasr-lib)` so external repos
   that depend on `target_link_libraries(... crispasr)` keep
   working while they migrate.

**Effort:** ~2 hours including consumer audit + CI re-runs.
Drop-in once green on every workflow.

**Don't do this in a patch release.** Even with the alias the
churn is visible to anyone bisecting a build issue. Schedule for
the next minor (0.7.0).




## 94. Auto-generate Go bindings `#cgo LDFLAGS` from CMake graphviz — DONE

_Done — see HISTORY.md + git log._

## 95. IndexTTS-1.5 Chinese TN — binary alternative to the Python `wetext` hook

**Status: open.** Today CrispASR ships `INDEXTTS_TEXT_NORMALIZER=<shell
cmd>` plus `tools/wetext-normalize.py` (commit `1bfe7c5a`,
2026-05-19). That covers users who already have Python + wetext
installed. Some deployments (single-binary distribution, Windows
without Python, embedded) need a no-Python path. This section
catalogs the realistic options so the next person doesn't have to
re-do the survey.

### The actual functionality gap

Default in-process `preprocess_indextts_text()` handles:
- CJK char split (port of upstream `tokenize_by_CJK_char`)
- Subset of `char_rep_map`: `，。：；！？、…“”‘’（）《》【】「」—～·` → ASCII
  before the CJK splitter
- ASCII upper-case

What it does NOT handle (full `wetext.Normalizer(lang='zh', operator='tn')`):
- Arabic numeral → hanzi (`2025年` → `二零二五年`, `123` → `一百二十三`)
- Pinyin tone-digit restoration (`xuan4`, `受不liao3` patterns)
- Dates (`2025/01/11` → `二零二五年一月十一日`)
- Times (`8:00` → `八点`)
- Currency (`¥12999` → `一万二千九百九十九元`)
- Phone numbers (`13800001234` digit-by-digit)
- Math / measurements / fractions / percent
- English contractions inside Chinese text

The model itself can't pronounce raw digits cleanly — it often fails
to emit `stop_mel_token` on un-pronounceable inputs and burns through
`max_mel_tokens=600` before giving up. So this isn't pure cosmetics:
TN is what makes digit-containing Chinese prompts actually work.

### Options, in order of pragmatic preference

**95a. Hand-roll the high-leverage rules in C++** (recommended first
step). Target the failures that actually break TTS — start with
digit-string → hanzi for the 1-billion range (`零一二三四五六七八九`
plus units `十百千万亿`), the `年/月/日` pattern, the `点/分` time
pattern, and pinyin tone-digit lookup. Estimated 300-600 LOC of
focused C++, no external dependency. Covers ~90 % of real prompts.
Edge cases (currency formats, mixed math, address parsing) silently
stay un-normalized. Lives in `src/indextts.cpp:preprocess_indextts_text`
or a sibling helper.

Files:
- `src/indextts.cpp` — extend the preprocessor with a
  `normalize_chinese_numbers()` pass that runs before the existing
  CJK split.
- `tests/` — golden inputs for the digit/date/time cases (just
  string-in, string-out; no model needed).
- `LEARNINGS.md` — new sub-section once the first user-reported
  edge-case lands so the next contributor knows what's covered.

When to do it: when an issue lands like "我有 3 个苹果 → weird audio"
or `2025年` produces a hang. Don't start it speculatively.

**95b. Vendor `kaldifst` (the C++ WFST runtime) + OpenFST + ship the
compiled `.fst` rule data.** The byte-identical-to-upstream path.

Ingredients:
- `kaldifst` C++ class `TextNormalizer` (k2-fsa/kaldifst on GitHub,
  a few thousand LOC of C++ wrapped around OpenFST). Apache-2.0.
- `OpenFST` (~30-50 K LOC of well-defined C++). Apache-2.0.
  Builds cleanly as a CMake subproject but the build profile cost
  is real — CrispASR today has no WFST dependency.
- Chinese TN rule data from `pengzhendong/wetext`:
  `fsts/zh/tn/tagger.fst` (812 KB) + `fsts/zh/tn/verbalizer.fst`
  (88 KB) + optionally `verbalizer_remove_erhua.fst` (88 KB).
  Plus the orchestration glue from `wetext.utils.normalize` (the
  preprocess → tagger → token-parser → verbalizer → postprocess
  flow) — `token_parser.py` is ~200 lines of recursive-descent
  parsing of the tagger output that would need to be rewritten in
  C++.

Estimated effort: 3-5 days of focused work for someone comfortable
with OpenFST. Result: same output as `wetext.Normalizer` byte for
byte, no Python at runtime.

When to do it: only after #95a has grown past ~5 hand-rolled cases
and the maintenance burden becomes visible — or if a downstream
deployment specifically can't take a Python dependency. Don't
speculate.

Files:
- `third_party/openfst/` — submodule (~50 K LOC, large diff).
- `third_party/kaldifst/` — submodule (~5 K LOC).
- `models/indextts-zh-tn.fsts` or co-distributed via `-m auto` —
  the ~1 MB of compiled FST data.
- `src/indextts.cpp` — new `normalize_chinese_wetext()` function
  invoked when `INDEXTTS_TEXT_NORMALIZER=wetext` (a sentinel
  value) is set, alongside the existing shell-command form.
- CMakeLists.txt — `add_subdirectory(third_party/openfst)` +
  link against kaldifst.

**95c. Static-bundle the Python sidecar via PyInstaller /
cibuildwheel.** Ship Python + wetext + dependencies as a
~50-80 MB single binary co-distributed with `crispasr`. Solves
the "no Python on the box" use case without porting any code.

Major downsides:
- Build system gets meaningfully more complex (a separate Python
  bundling pipeline per platform).
- ~50-80 MB of bloat per release.
- Not idiomatic for a C++ project; CrispASR's distribution story
  today is "one binary + model files".

Almost certainly **not** worth doing. Listed only so future
contributors don't spend time discovering it independently.

**95d. Tiny FST reader in our own C++ — consume OpenFST's
binary `.fst` directly without linking OpenFST.** Middle ground
between #95a (hand-roll rules) and #95b (vendor 30-50 K LOC of
OpenFST + kaldifst). Same upstream rule data as #95b — `.fst`
files from `pengzhendong/wetext` — but skip the libraries.

Why this might be the right shape: the wetext / kaldifst rule
chain is small (one tagger + one verbalizer, no runtime
composition needed if we pre-compose at build time), and
OpenFST's `.fst` binary format is documented in the OpenFST
manual. A single-pass interpreter that reads StdVectorFst
(the variant the wetext compiler emits) and does deterministic
traversal is genuinely small.

Sketch:

- **Parser** (~200 LOC). OpenFST `.fst` header is fixed-layout
  (magic, version, fst-type, arc-type, properties, n_states,
  flags); per-state records carry `final_weight` + arc lists;
  per-arc records carry `ilabel`, `olabel`, `weight`,
  `nextstate`. The wetext rule files use `StdVectorFst`
  (TropicalArc) — uniform fixed-size records. Read once at
  startup into a flat `std::vector<State>` with arc slices.
- **Traverser** (~150 LOC). For each input UTF-8 token,
  consume from the current state along matching `ilabel` arcs,
  emitting `olabel` symbols and tracking weights. Use the
  Tropical semiring (sum of weights along the path; pick the
  min-weight path on ambiguity). Wetext's TN rules are
  deterministic in practice — most input strings have a unique
  matching path — so the traversal is essentially a DFA lookup
  with epsilon transitions, not a full lattice search.
- **Symbol table** (~50 LOC). Wetext ships `tokens.txt` /
  `chars.txt` alongside the `.fst` files mapping symbol IDs to
  UTF-8 strings. Load once, look up by ID during emission.
- **Token-parser glue** (~200 LOC). Port wetext's
  `token_parser.py` (the `tagger output → struct → verbalizer`
  flow) into C++. This is independent of the FST library — same
  ~200 LOC of recursive-descent parsing whether we use OpenFST
  or roll our own.
- **Build-time pre-composition** (optional, ~0 LOC at runtime if
  we ship a pre-composed `.fst`). The tagger ∘ verbalizer
  composition can be done once offline using upstream OpenFST
  on the dev machine and the result checked in alongside the
  rule data. Run-time then only needs the StdVectorFst
  interpreter, not composition.

Total: ~500-800 LOC of C++, zero new dependencies, ~1 MB of
checked-in or auto-downloaded `.fst` data (same as #95b).

Trade-offs vs #95b:

- **Pro:** No third-party submodule. Build profile unchanged.
  Easier to reason about (it's small enough to fit in one
  reviewer's head). Cross-compilation footprint identical to
  the rest of CrispASR.
- **Con:** Not byte-identical to upstream wetext on edge cases
  involving FST features we don't implement (composition
  shortcuts, special weight semirings, on-the-fly relabeling).
  Acceptable iff we pin to the specific wetext rule files and
  treat them as a frozen artifact.

Estimated effort: 2-4 days for someone who reads the OpenFST
binary format spec end-to-end and can verify against
upstream's `fstprint` output on a dozen small inputs. Strictly
less effort than #95b (3-5 days + ongoing OpenFST submodule
maintenance) and strictly more correct than #95a (hand-rolled
rules will never catch up to wetext's coverage).

Files:

- `src/indextts_zh_tn.{h,cpp}` — the parser + traverser +
  symbol-table loader; ~500-800 LOC.
- `src/indextts.cpp` — invoke `normalize_chinese_wetext_native()`
  when `INDEXTTS_TEXT_NORMALIZER=native` is set (third sentinel
  alongside the existing shell-command form).
- `models/indextts-zh-tn-{tagger,verbalizer}.fst` — checked in
  (1 MB) or auto-downloaded via the model registry.
- `tools/dump-openfst-text.py` — dev-time helper that reads a
  `.fst` via upstream `pynini`/`openfst` Python bindings and
  dumps the byte layout for verification.
- `tests/indextts_zh_tn_test.cpp` — golden inputs (the same set
  used in #95a) matched against `wetext` Python output byte for
  byte.

When to do it: after #95a's hand-rolled list passes 2-3 entries
(confirming the use case is alive) but before contemplating
#95b's OpenFST submodule. #95d is the "we want byte-stable
behaviour without the dependency tax" sweet spot.

### What looks like an alternative but isn't

- **ICU `Transliterator`** — does Unicode normalization (NFC, NFKC,
  case folding) and pre-defined transforms like `Han-Latin`. No
  number → hanzi, no date parsing, no rule set comparable to wetext.
- **`libnumber2chinese` / `cn2an` / `pypinyin`** — Python libraries
  with no coherent C/C++ port. Fragments exist, no drop-in.
- **HuggingFace `tokenizers` normalizers** — tokenizer-side
  normalization (lowercase, NFC, strip accents), nowhere near
  wetext-equivalent rule coverage.

### Trigger to start work

This section sits idle until **one of**:

1. A user files an issue with a digit/date/pinyin-tone-digit prompt
   that produces broken audio. Then go to #95a; pick the smallest
   rule set that fixes the reported case.
2. The hand-rolled list reaches 2-3 entries (signal: the use case is
   actually alive). At that point #95d (tiny FST reader, ~500-800
   LOC, zero deps) becomes the right next step rather than letting
   #95a grow into a pile of one-off rules.
3. Only if #95d turns out to be wrong (FST features we don't
   implement keep biting), fall back to #95b (vendor OpenFST + kaldifst).
   Don't go to #95b speculatively.

Don't pre-emptively vendor OpenFST. CrispASR's clean "ggml + minor
deps" profile is a feature.

---

## 100. MeloTTS + OpenVoice2 — multilingual TTS with native CJK + voice cloning — Phase A DONE

_Done — see HISTORY.md + git log._

## 101. OmniVoice — single-stage NAR diffusion TTS with voice cloning

Surveyed via RapidAI/RapidSpeech.cpp ("single-stage non-autoregressive
diffusion TTS, multilingual + voice cloning"). RapidSpeech ships a
`convert_omnivoice_to_gguf.py` that merges an LLM component + audio
tokenizer into one GGUF — same packaging pattern as our Fun-ASR-Nano
and MiMo-ASR ports.

### Open questions before scoping

**Upstream source unclear.** RapidSpeech.cpp's README doesn't link
the upstream OmniVoice repo or HF model card; my websearch hit a wall
(the name collides with several other "Omni" projects). Before
sizing the port we need:

1. Confirm the upstream model identifier (likely on ModelScope or HF —
   search for "OmniVoice" + "diffusion TTS"; check
   `FunAudioLLM`/`ZAI`/`Beijing-Academy-of-AI` namespaces).
2. License — Apache-2.0 / MIT / non-commercial? Skip if non-commercial.
3. Parameter count and codec choice (RVQ? CFM target like voxcpm2?
   raw mel like MeloTTS?).
4. **Differentiation vs voxcpm2.** voxcpm2 is also CFM-diffusion with
   voice cloning (Apache-2.0, 30 langs, native 48 kHz). If OmniVoice
   doesn't bring something distinct — a different codec, better CJK,
   smaller footprint, faster inference — it's redundant.

### Conditional port plan (only if the upstream survey clears)

Assume the model is **non-autoregressive diffusion** (per RapidSpeech's
description). The likely shape based on the conversion script hint
("LLM component + audio tokenizer"):

- A text-conditioning LLM (likely 0.5-1B, similar to qwen3-tts talker)
- An NAR diffusion head over a discrete audio codec (likely 12-25 Hz
  RVQ like qwen3-tts or 48 Hz CFM-mel like voxcpm2)

Reuse map:
- Diffusion solver: voxcpm2's CFM solver (`src/voxcpm2_tts.cpp`) or
  Chatterbox-S3Gen's CFM solver (HISTORY §82) — pick the one that
  matches OmniVoice's solver type (DPM-Solver++ vs Euler vs HuangEuler)
- Codec: if RVQ, reuse mimo-tokenizer (`src/mimo_audio_tokenizer.cpp`);
  if mel-CFM, reuse voxcpm2 VAE
- Talker: another qwen3-tts-style talker port (#52 family); no new
  primitives needed

### Triggers

- Survey clears with a permissive license + measurable advantage over
  voxcpm2 on one of {CJK quality, model size, latency}.
- Otherwise this stays in survey-only mode — voxcpm2 + qwen3-tts +
  the (planned) melotts/openvoice2 already cover the same space.

---

## 102. RapidTP-Aligns — dedicated NN timestamp predictor (survey)

RapidAI ships `RapidTP-Aligns` ("语音的时间戳预测") as a standalone
**timestamp-only model** that runs on raw audio independently of any
ASR. CrispASR currently produces word-level timestamps via either:

- **Native** (whisper, parakeet TDT, canary, cohere, kyutai-stt) —
  emitted by the decoder
- **CTC forced aligner** (`-am canary-ctc-aligner.gguf` or
  `-am qwen3-forced-aligner.gguf`) for LLM-style backends that lack
  native timestamps (granite, voxtral, qwen3, glm-asr, omniasr-llm,
  funasr, etc.) — requires the *text* as input and aligns it back
  to the audio

A dedicated NN predictor would be a **third path**: predict timestamps
**from audio alone**, without needing the ASR's text. Useful when:

- We want timestamps independent of which ASR ran (cross-backend
  consistency on the same audio).
- The ASR's text is wrong but we still want trustworthy segment
  boundaries — useful for diarization / VAD post-processing.
- Streaming: predict end-of-utterance / silence boundaries one
  forward pass instead of running a full CTC alignment.

### Open questions

1. **What architecture** does upstream RapidTP-Aligns ship? The repo
   description is one line in Chinese; the README doesn't surface
   model identifier or arch. Likely a small Conformer-CTC over
   raw audio outputting frame-level boundary labels.
2. **License + upstream weights.** ModelScope-hosted? FunASR derivative?
   Confirm before starting.
3. **Quality vs our CTC aligners.** Our `canary-ctc-aligner-q4_k.gguf`
   (~80 MB) is fast and accurate enough that we haven't seen
   complaints — without a clear improvement margin a dedicated NN
   timestamp predictor is incremental.

### Trigger

- User reports CTC aligner failing on a specific audio class
  (e.g. heavily code-switched, multi-speaker overlap, music behind
  speech) that a dedicated timestamp predictor might handle better.
- OR: we add streaming ASR endpoint that needs sub-100-ms
  end-of-utterance prediction (currently we use VAD silence heuristic).

Until then: stays survey-only. Existing CTC aligners are good enough
for the dominant use cases.

---

## 103. Silero VAD version bump — verify and align with v6 — DONE

_Done — see HISTORY.md + git log._

## 104. Stateful frame-streaming TDT decode for parakeet long-form (issue #89)

**Priority: HIGH** — the auto path (no `--vad`, no `--chunk-seconds`) tops
out at ~82 % coverage on 60 s Japanese audio. Users expect >95 %.

### Problem

The current chunking approach (`kLongAudioFallbackChunkSeconds = 30`)
processes each chunk independently: fresh mel + z-norm + fresh TDT
decoder LSTM state. The decoder cold-starts each chunk and loses 5-20 s
of content from each chunk's interior (not just at boundaries). Sweep of
chunk sizes 15-30 s × overlap 3-8 s on the issue #89 reporter's 60 s
Japanese audio (parakeet-tdt-0.6b-ja, CPU-only):

| chunk | overlap | chars | coverage% | max_gap |
|-------|---------|-------|-----------|---------|
| 20s   | 3s      | 278   | 82.4%     | 3.4s    |  ← best without VAD
| 15s   | 5s      | 278   | 70.9%     | 5.3s    |
| 30s   | 3s      | 195   | 59.7%     | 19.4s   |  ← current default
| 60s   | 0s      | 294   | 99.5%     | 0s      |  ← single-pass (fails on Vulkan/AMD)
| VAD silero | —  | 281   | 93.1%     | 3.7s    |

Counterintuitively, more overlap hurts (extends z-norm window →
distribution shift). The ceiling is the decoder cold-start, not boundary
stitching.

### NeMo's approach

NeMo's `FrameBatchASR` / `BatchedFrameASRTDT` uses a fundamentally
different architecture:

| | NeMo streaming | CrispASR chunking |
|---|---|---|
| Frame step | 1.6-4 s | 15-30 s |
| Buffer | 4 s rolling | chunk + overlap |
| Z-norm | Over 4 s buffer | Over 30-33 s chunk |
| Decoder | **Stateful** across frames | Independent per chunk |
| Overlap ratio | 60-150 % of frame | 10-20 % of chunk |

Key: NeMo keeps the TDT LSTM predictor state between frames. Each 1.6 s
step feeds the decoder with the previous hidden state, so it never
cold-starts. CrispASR reinitializes the LSTM (`lstm_init_state`) for
every chunk.

### Implementation plan

**Phase 1: Stateful TDT decode (the core change)**

The split API already exists (`parakeet.h`):
```c
float* parakeet_encode(ctx, samples, n_samples, &T_enc, &d_model);
parakeet_result* parakeet_decode_frames(ctx, enc_frames, T_enc, d_model, t_offset_cs);
```

Changes needed:

1. **Add `parakeet_decode_frames_stateful`**: like `parakeet_decode_frames`
   but accepts/returns the LSTM hidden state (`parakeet_lstm_state`).
   The TDT decode loop in `parakeet.cpp:1003` already uses `state` — just
   need to make it an in/out parameter instead of initializing to SOS.

2. **Add streaming mel with running z-norm**: instead of computing z-norm
   per chunk, maintain running mean/variance across frames (NeMo's
   `get_norm_consts_per_frame` approach). Add a
   `parakeet_mel_streaming_context` that tracks the statistics.

3. **Wire into `crispasr_run.cpp`**: when the long-audio fallback
   triggers and the backend is parakeet/canary, use the streaming
   decode path instead of independent chunk transcription:
   ```
   for each 4s frame:
     mel = compute_mel(frame, streaming_mel_ctx)  // running z-norm
     enc = parakeet_encode(mel)
     result = parakeet_decode_frames_stateful(enc, &lstm_state)
     merge results with LCS
   ```

**Phase 2: Tuning**

4. Frame size sweep (1.6s, 2s, 4s, 8s) on the benchmark corpus.
5. Running z-norm warmup: first frame gets per-frame z-norm, subsequent
   frames use exponential moving average (NeMo's approach).
6. LCS delay tuning: `lcs_delay = (buffer - frame) / model_stride`.

### Files to modify

- `src/parakeet.h` — add `parakeet_decode_frames_stateful`, `parakeet_mel_streaming_context`
- `src/parakeet.cpp` — expose LSTM state in/out, add streaming mel helper
- `src/core/mel.h` / `mel.cpp` — add running z-norm mode
- `examples/cli/crispasr_run.cpp` — streaming decode path in `process_one_input`
- `examples/cli/crispasr_backend_parakeet.cpp` — streaming transcribe method
- `tests/test-issue-89-long-audio-fallback.cpp` — pin streaming path activation

### Effort

Medium-Large. Phase 1 is ~200-300 LOC of new code (the hot loop is
<50 lines — the LSTM state threading is the main work). Phase 2 is
benchmarking and tuning.

### Success criteria

`python tests/benchmark_asr.py --audio yt_60s.wav --backend parakeet-ja --settings auto`
reports **coverage ≥ 95 %** on the issue #89 reporter's Japanese audio,
without `--vad`.

### Trigger

Immediate — issue #89 is open and the current fix is a partial
mitigation (prevents 0-output catastrophe but doesn't match NeMo quality).

---

## 105. WhisperX word alignment models — wav2vec2 CTC zoo

WhisperX does word alignment as a post-process: ASR text first, then a
language-keyed wav2vec2/CTC aligner refines timestamps at the word
level. The shipped defaults include:

- Torchaudio bundles: `WAV2VEC2_ASR_BASE_960H` (`en`),
  `VOXPOPULI_ASR_BASE_10K_FR` (`fr`),
  `VOXPOPULI_ASR_BASE_10K_DE` (`de`),
  `VOXPOPULI_ASR_BASE_10K_ES` (`es`),
  `VOXPOPULI_ASR_BASE_10K_IT` (`it`)
- Hugging Face checkpoints:
  `jonatasgrosman/wav2vec2-large-xlsr-53-japanese`,
  `jonatasgrosman/wav2vec2-large-xlsr-53-chinese-zh-cn`,
  `jonatasgrosman/wav2vec2-large-xlsr-53-dutch`,
  `Yehor/wav2vec2-xls-r-300m-uk-with-small-lm`,
  `jonatasgrosman/wav2vec2-large-xlsr-53-portuguese`,
  `jonatasgrosman/wav2vec2-large-xlsr-53-arabic`,
  `comodoro/wav2vec2-xls-r-300m-cs-250`

CrispASR now has three CTC aligner families wired into `-am`:

- `canary-ctc-aligner.gguf`
- `qwen3-forced-aligner.gguf`
- `wav2vec2-aligner` / `wav2vec2-aligner-en` / `wav2vec2-aligner-de`
  aliases, plus any raw wav2vec2 / HuBERT / data2vec CTC GGUF path

That means the runtime family support exists, but not all of WhisperX's
language models are converted/uploaded yet. The remaining path splits
into two cases:

- Torchaudio bundle models need a direct runtime path or explicit
  converter support.
- Hugging Face wav2vec2 checkpoints are the easy case: add a generic
  wav2vec2-aligner family, register the common language aliases, and
  reuse the existing CTC alignment plumbing.

### Implementation plan

1. DONE: Add a generic `wav2vec2-aligner` family in the aligner registry
   and C-ABI path so `-am` can dispatch beyond canary/qwen3.
2. DONE: Add initial aliases for `en` and `de` using the already-hosted
   wav2vec2 GGUFs.
3. DONE: All 10 WhisperX common languages converted, quantized, and
   uploaded to `cstr/*-GGUF` HF repos: `fr`, `es`, `it`, `ja`, `zh`,
   `nl`, `uk`, `pt`, `ar`, `cs`. Registry entries and auto-download
   wired. Verified 2026-05-23.
4. DONE: All models are native HF wav2vec2 checkpoints converted via
   `models/convert-wav2vec2-to-gguf.py`. Torchaudio bundles not needed.
5. TODO: Benchmark the new aligners against `canary-ctc-aligner.gguf`.
6. TODO: Document in docs/cli.md which `-am` aliases are available.

### Trigger

- A user wants WhisperX-style word alignment parity in CrispASR.
- A backend needs better language coverage than the current canary/qwen3
  aligners provide.
- We want to close the gap between `whisperX.load_align_model(...)` and
  CrispASR's current `-am` model surface.

---

## 106. TEN-VAD — low-latency cross-platform VAD

TEN-VAD looks technically feasible as an additional VAD backend. The
upstream repo ships cross-platform C bindings, prebuilt libs, and an
ONNX path, and its documented runtime target is 16 kHz audio with
10/16 ms hop sizes. That fits CrispASR's existing VAD surface well
enough to add as a fourth backend, alongside Silero, FireRedVAD,
MarbleNet, and Whisper-VAD-EncDec.

Why it is a good fit:

- C-compatible API and native libs already exist for Linux, macOS,
  Windows, Android, iOS, and Web.
- The model is lightweight relative to Silero and is explicitly aimed at
  low-latency streaming turn detection.
- The upstream README already documents Python, C, Java, Go, and JS
  usage, so the packaging shape is familiar.

Main caveat:

- The upstream license is Apache 2.0 with additional no-compete / own-
  app-only conditions, so we should treat distribution as blocked until
  legal review or a clear internal-only use case is approved.

### Implementation plan

1. Keep the technical integration plan ready, but do not wire or ship
   the backend until the license decision is explicit.
2. Decide whether to use the prebuilt native lib path, the ONNX path,
   or both.
3. Add a `ten-vad` backend alias in the VAD registry and CLI so
   `--vad -vm ten-vad` works, if approved.
4. Add auto-download metadata for the chosen artifact(s) and keep the
   existing Silero default unchanged.
5. Run the usual boundary benchmark against Silero and FireRedVAD on
   the same short-gap / sentence-end test set.
6. Document sampling-rate handling clearly: 16 kHz in, resample other
   inputs before inference.

### Trigger

- A user wants a lower-latency or lower-footprint VAD than Silero.
- We want a fourth backend option with native cross-platform coverage.
- The license review confirms the additional no-compete / own-app-only
  conditions are acceptable for our distribution model, or we confine it
  to an internal-only path.

---

## 114. Long-form transcribe — make chunking/streamed the default for all ASR backends (issue #89 follow-up)

**DONE / superseded by the #89 close-out.**

Parakeet streamed-chunking default landed 2026-05-24 (33f9a162); the
rest of the per-backend long-audio matrix was carried to completion by
the #89 hardening arc (VAD slice cap + gap-fill, 2026-07-04) and the
#218 arc (LLM backends). Current per-backend state → PERFORMANCE.md
"Long-audio coverage" + "Long-form single-pass (#218)"; history →
HISTORY 2026-07 entries.

## 115. mimo-asr baseline broken — silent empty on short, segfault on long

**DONE (2026-06-08).**

GPU default since a429bb45; Option B (prefill-graph reuse, embed tables
CPU-resident via `load_weights_split`) is the production path — 2.4× RT
on RTX 3090 + P100. k-quant CUDA GET_ROWS fix (3bf9a599) as safety net.
Option C (step-graph) measured 3× slower than B — not worth pursuing.
`CRISPASR_MIMO_FORCE_CPU=1` for debugging. Detail → HISTORY.

## 125. Issue #125 — multi-backend bug sweep from montvid (12 findings)

External user `montvid` ran every available backend on the issue #89 reporter's 50:47 EN FLAC plus the project's own `samples/jfk.wav` smoke fixture, all on **CrispASR v0.6.10 commit `eaee2319`** (the 2026-05-25 morning build, just after the per-backend opt-out fix train), hardware NVIDIA RTX PRO 6000 Blackwell sm_120 + CUDA 12.6. Reference build for the regression bisect: **commit `f23d9485`** (v0.6.9, 2026-05-21 "fix(paraformer): suppress cppcheck invalidPointerCast"). 12 report files attached to the issue; all 12 cached at `/Volumes/backups/code/issue125-attachments/`. The reporter's analysis is high-quality — each finding pins the failing file:line and proposes a concrete fix shape.

This PLAN section reproduces the priority ordering, status, and fix shape for each finding so the next contributor can pick any item off the list independently.

### P0 — Regression I shipped: mimo-asr segfault on Blackwell sm_120 (report 12) — bisect reattributed 2026-05-26

**TL;DR.** v0.6.9 `f23d9485` works on `samples/jfk.wav`. v0.6.10 `eaee2319` segfaults during decode (`rc=139`). Reporter bisected to `6b492b2b` (FA per-head mask) but the bisect grep filtered to `ggml/src/ggml-cuda/` + mimo files and missed `ggml/src/ggml-backend.cpp`, which has a second behaviour-changing commit between the two builds. After review on 2026-05-26 the FA-mask attribution is ruled out and the real suspect is **`0f0f0793` "fix(#83): ggml sched src-mutation log + UNet input pin"**.

**Why FA per-head mask is NOT the cause.**

- Every line of `6b492b2b` is wrapped in `#ifdef GGML_CUDA_CRISPASR_FA_PERHEAD_MASK ... #endif` (both `fattn-mma-f16.cuh` and `fattn.cu`).
- The CMake option `GGML_CUDA_CRISPASR_FA_PERHEAD_MASK` defaults `OFF` (`ggml/CMakeLists.txt:211`).
- No CI / release script anywhere in the repo sets the flag ON — `grep -rn "FA_PERHEAD"` returns only the two CMakeLists.txt entries.
- With the macro undefined the compiled binary is byte-identical to upstream for that path. A self-built `eaee2319` (which is what the reporter has, `/opt/crispasr-main/build/bin/crispasr`) gets OFF by default.

**Why `0f0f0793` is the real suspect.**

The patch adds an *unconditional* src-mutation log + restore in `ggml-backend.cpp` (the generic scheduler, used by every backend including CUDA). Intent: fix chatterbox CFG's cond+uncond gf reuse (chatterbox's S3Gen UNet runs the same gf twice per CFM step, the second call lost the rewired inputs). Bug in the original patch:

- The restore loop ran only on the *success* path of `compute_splits`. If any compute step returned early on a non-`GGML_STATUS_SUCCESS`, the mutation log was retained.
- `split_graph` did not reset `n_src_mutations` at its start, and neither did `sched_reset`. So a next call appended on top of stale entries.
- On the next successful `compute_splits`, the restore loop walked stale `(node, j)` pairs from the *previous* gf and wrote `m->orig_src` to `m->node->src[m->j]`. If the previous gf had been re-laid-out (which mimo-asr does — it builds a fresh cgraph per AR-decoded token), `m->node` is a dangling pointer → write to freed memory → segfault.

mimo-asr's profile fits this exactly: many compute calls per transcribe (one per AR token after the audio adaptor encoder pass). Any one early-return cascades into the next call.

**Hardening shipped: `a5a518c8` "fix(ggml-backend): restore src[j] on every exit path of compute_splits".**

- Extracted the restore loop into `ggml_backend_sched_restore_src_mutations()`.
- Called on every exit path of `compute_splits` (the two early-error returns + the success path).
- Called defensively at the start of `split_graph` and inside `sched_reset` so stale entries from any prior aborted compute are dropped *before* `ggml_free(sched->ctx)`. The restore writes to the user's gf (`orig_src` pointers captured before the rewire), not to `sched->ctx`, so it is safe to call before the free.
- Tightened the realloc-on-grow to assign-after-success.
- Stripped the `// CrispASR patch (#83 r9 follow-up #4)` and `MUST RE-APPLY` markers per `feedback_strip_local_markers.md`.

**Local M1 Metal verification (2026-05-26).**

- `crispasr --backend parakeet ... samples/jfk.wav` — JFK transcribes correctly via the hardened scheduler.
- `crispasr --tts "Hello world test." --tts-output /tmp/tts-smoke.wav -m chatterbox-t3-q8_0-regen.gguf` — CFM solver runs cond+uncond, produces `vocoder mel rms=4.625` (ref rms=5.115; broken-baseline before `0f0f0793` was rms=13.938). The original chatterbox fix is preserved.

**Outstanding: Blackwell sm_120 validation.**

We have no CUDA hardware to confirm the fix end-to-end on the reporter's failing case. **Ask reporter (montvid) to rebuild from `95d74455` or later** and rerun:

```bash
cd /opt/crispasr-main && git pull && cmake --build build -j
/opt/crispasr-main/build/bin/crispasr --backend mimo-asr -m auto --auto-download \
    -f /opt/crispasr-main/samples/jfk.wav -l en -np -nt
```

Expected on the hardened scheduler: transcript matches v0.6.9 reference (`ANd so, my fellow Americans, ask not what your country can do for you.. ASk what you can do for your country..`).

If the segfault persists, next debug step is: `gdb --args` + capture the backtrace + share with us. The other plausible suspects in that case are (a) the `42d1e011` / `db5e22a7` Metal-debug commits leaking into CUDA via shared infrastructure (unlikely — Metal-only files); (b) a separate ggml-cuda bug exposed by Blackwell sm_120 specifically.

**Followups outside this commit.**

1. **Wider validation matrix for any future `ggml-cuda/fattn*` change.** Reporter's request: add mimo-asr, glm-asr, gemma4-e2b, voxtral, granite (multi-head audio-LLM backends) to the GPU validation matrix before landing a kernel change. The 2026-05-24 validation block in HISTORY mentions only a single CTC backend on a single GPU — not enough coverage.
2. **Hold the upstream FA-mask PR draft** at `tools/upstream-prs/06-cuda-fa-perhead-mask.md`. The kernel correctness story still depends on a wider validation matrix; do not submit until that lands.

Cross-refs: `a5a518c8` hardening commit; HISTORY 2026-05-26 follow-up; PLAN #115 (existing JFK-empty-on-Apple-Silicon entry; same root cause, different symptom).

### P1 — funasr / fun-asr-mlt-nano produce `!` loops at every length (reports 01, 07, 08, 09) — DONE `f72d3db1` 2026-05-26

**Shipped:** degenerate-loop guard in the AR decode (bail after the same token id repeats > 20× in a row, with a clear "decode degenerated" message); `frames_spliced` and `fake_token_len` surfaced at `CRISPASR_VERBOSE=1` so the next reporter can confirm/rule out the "encoder collapsed" hypothesis; registry entries added for funasr, fun-asr-mlt-nano, sensevoice, paraformer in `tools/test-all-backends.py` so future regressions get caught loud by the default JFK assertion. Local M1 Metal smoke shows funasr produces the canonical JFK transcript — the `!`-loop is platform/CUDA-specific. **Still open (longer-term):** root-cause the audio adaptor / encoder collapse on Blackwell CUDA; honour `-l` on the funasr prompt template. Both are followups, not blockers, since the loop guard prevents the 60 KB `!` symptom regardless.



**TL;DR.** Both funasr variants emit 60 KB of `!` regardless of audio content or `-l` value. Reproduces on **`samples/jfk.wav` in 3 seconds** (report #07 control test), so this is not a long-audio bug — the model is dead at any length. Report #08 ruled out the "Chinese-prompt vs English-audio mismatch" hypothesis by showing `-l zh` produces byte-identical output to `-l en`. Greedy argmax in `src/funasr.cpp:1375-1400` has no rep-penalty / temperature / degenerate-loop guard; once the joint logits collapse the decoder locks on token id ~5 (`!` in Qwen3 vocab) until `max_new_tokens`.

The remaining suspects are the audio adaptor (`audio_adaptor.*` tensors) and the encoder. Diagnostic the reporter requests: log `frames_spliced` in `funasr_init_from_file` — if it's 0 on an 11 s JFK clip, the encoder/adaptor is the failure.

Report #09 separately notes that **funasr and fun-asr-mlt-nano have no entry in `tools/test-all-backends.py`** — so this regression has been silently shipping since the 2026-05-20 port landed. Sensevoice and paraformer are also absent. The reporter proposes the smallest possible regression guard: assert `"merica"` (case-insensitive) appears in the JFK output text — that catches `!`-loops, language flips, and any obvious decode collapse.

**Fixes:**

1. **Add funasr + fun-asr-mlt-nano + sensevoice + paraformer to `tools/test-all-backends.py`.** Each needs a registry entry with the published GGUF location at `cstr/*-GGUF` (sample skeleton in report #09). The default JFK assertion (`werv < threshold`) at `tools/test-all-backends.py:670` catches the `!`-loop cleanly because `wer("…", "!!!!…") ≈ 1.0`.
2. **Add a degenerate-loop guard in the funasr argmax loop.** Bail after the same `next_id` repeats > 20× in a row. Cheap, model-agnostic stop-loss. Either separately or as part of the broader `core_greedy_decode` work.
3. **Honour `-l` on the funasr path.** — **DONE** `1b491c3d` 2026-06-12. Added `funasr_set_language()` C API + dynamic prompt builder matching upstream `get_prompt(language=...)`. Wired from CLI backend adapter (`params.language`) and session API (`s->source_language`). Kaggle-verified on CUDA: all 4 language configs (default, -l en, -l English, -l zh) produce correct JFK transcripts.
4. **Diagnose the audio adaptor.** Print `frames_spliced` on init at verbose mode; ideally diff the adaptor output against the upstream FunASR Python reference on JFK. If the adaptor is the bug, the prompt fix above is window-dressing.

Cross-refs: HISTORY 2026-05-20 "funasr: FunAudioLLM/Fun-ASR-{Nano,MLT-Nano}-2512 port lands"; 2026-05-21 "funasr: fix MLT-Nano hallucination (PLAN #99)" — only addressed a different failure mode (Chinese-prefix tail drift on the first ~20 correctly-decoded tokens); the present `!`-from-step-0 case is new.

### P2 — firered-asr declares `CAP_UNBOUNDED_INPUT` but pe_maxlen ≈ 50 s (report 04) — DONE `72b74486` 2026-05-26

**Shipped:** `CAP_UNBOUNDED_INPUT` dropped from `crispasr_backend_firered_asr.cpp`; defensive length check in `firered_asr_transcribe_impl` aborts with a clear "input too long (T_sub=X > pe_maxlen=Y; ~Z s of audio after subsampling)" message when callers bypass the VAD dispatcher. Local M1 Metal smoke: JFK still produces the canonical transcript. Long-audio routes through the per-VAD-segment dispatch path. The "JFK silence-only output without `--vad`" subtask (vocab/blank-id mismatch in the auto-downloaded GGUF) was not reproduced in local testing — keeping it on the followup list if a new report surfaces it.



**TL;DR.** firered-asr's encoder has `pe_maxlen = 5000` (relative positional encoding window, ≈ 50 s at 10 ms hop after subsampling). The backend declares `CAP_UNBOUNDED_INPUT`, which tells the dispatcher to bypass per-segment VAD dispatch and pass the whole audio buffer in one call. On a 50 min file `T_sub ≈ 300 000` frames, way past the PE window; the relative-shift attention reads past the PE buffer with no bounds check, producing silent OOB / numerically degenerate output. JFK on the same backend produces a byte-perfect transcript (report #07), so the model itself is fine.

**Fixes:**

1. **Drop `CAP_UNBOUNDED_INPUT` from firered-asr's registry entry.** One-line fix in `examples/cli/crispasr_backend_firered_asr.cpp`. Each VAD segment is well under 50 s; the existing `--vad` path will then dispatch per-segment correctly.
2. **Add an explicit length check** in `firered_asr_transcribe`: return an error with a clear message if `T_sub > pe_maxlen` instead of silently OOB.
3. **Investigate the JFK silence-only output without `--vad`.** Reporter notes that short clips also sometimes return `<Sil>!` only — likely a vocab/blank-id mismatch in the auto-downloaded GGUF, separate from the cap bug.

### P3 — omniasr-llm's `is_streaming` guard prevents chunking on non-streaming GGUFs (report 03) — DONE `5f0aefc0` 2026-05-26

**Shipped:** `src/omniasr.cpp` chunking decision rewritten as `(is_streaming || force_seg) && T_enc > 1`. The segment-marker injection at L1334 stays gated on `is_streaming` (non-streaming variants chunk without the marker — each chunk is decoded as if it's a complete utterance, which is what the model was trained on). Local M1 Metal smoke: JFK still produces the canonical transcript with the known 1-word "americas"→"americans" slip. The wallclock concern (6.8× RT means ~30 min wall for a 50 min file on CPU) is unchanged — chunking dodges the OOM but the realistic remedy for speed is GPU offload for the LLM head.



**TL;DR.** `src/omniasr.cpp:1356-1368` gates the per-segment chunking on `is_streaming` (set from `hp.n_special_tokens == 3` at L1331). For GGUFs without the streaming-mode flag, `n_segments` stays at 1 and the entire 50-minute audio gets fed to a 512-token LLM. The model produces correct text on JFK (report #07), so this is purely a long-audio dispatcher bug.

**Fix:**

Drop the `is_streaming` gate for the chunking decision — always segment past `segment_secs`. Keep the streaming gate only for the segment-marker token injection at L1450 (which actually depends on the special-token vocab). Reporter's patch sketch:

```cpp
const int seg_frames = (int)(hp.segment_secs * 16000.0f) / total_stride;
const bool force_seg = (seg_frames > 0 && T_enc > seg_frames);
if ((is_streaming || force_seg) && T_enc > 1) { ... }
```

Even with this fix, the JFK measurement at 6.8× RT means a 50-minute file would need ~30 min wall on CPU — chunking dodges the OOM, but the realistic remedy for wallclock is GPU offload for the LLM head.

### P4 — gemma4-e2b hallucinates on long audio, works on short (reports 02, 07) — DONE `8bfaff23` 2026-05-26

**Shipped:** defensive 30 s training-window guard in `crispasr_backend_gemma4_e2b.cpp::transcribe()`. Inputs > 30 s abort by default with a clear error message ("input is N s (> 30 s training window). Use --vad to segment, chunk externally, or set CRISPASR_GEMMA4_AUTO_CHUNK=1 to chunk internally"). This stops the symptom — silently-wrong LLM commentary on long audio — and forces the user to route via the segmenter instead. **`CRISPASR_GEMMA4_AUTO_CHUNK=1`** opt-in (`9b5a0a2a`) chunks at 30 s boundaries internally with the same `t_offset_cs` arithmetic as kyutai-stt P6b; off by default because we haven't validated quality at chunk boundaries on long gemma4-e2b output. Local M1 Metal smoke: JFK still produces "ANd so my fellow Americans ask not what your country can do for you, ask what you can do for your country..". **2026-06-13: added `prefers_vad()` override** — auto-enables VAD for >30 s audio (same pattern as parakeet-ja `f950bd1e`). This gives the model silence-bounded segments matching its ~30 s training window, which is the correct approach vs hard-chunking at arbitrary 30 s boundaries. The `CRISPASR_GEMMA4_AUTO_CHUNK` env var path is kept as a fallback but VAD is now the default recommendation. **Kaggle P100 sweep v5 validated 2026-06-13:** 60s audio (5× JFK concatenated) now produces **471 chars** correct transcript via auto-VAD (was 10 chars `<Eos>n0t.!` with hard-chunk). Both default and `CRISPASR_GEMMA4_AUTO_CHUNK=1` paths produce identical 471-char output (VAD fires before the internal chunker). **Still open (longer-term):** Audit the prompt-wiring sanity logs the reporter originally asked for (`audio_soft_token_id`, `proj_dim` vs `d_model`).



**TL;DR.** JFK 11 s transcribes verbatim ("ANd so my fellow Americans ask not what your country can do for you, ask what you can do for your country.."). On the 50 min file the model emits unrelated LLM commentary ("…a holistic view of the self and the concept of the energy body…") starting with `<Eos>!` — meaning it emitted `<end_of_sequence>` immediately after the prompt and then continued into a generic response. So this is a **chunking / long-context bug**, not the audio-soft-token-id mismatch hypothesised in report #02 before the control test in #07.

**Fixes:**

1. **Audit the chunking path** — most likely the dispatcher hands the entire file to the LLM in one prompt without segmenting; the model hits `<eos>` after the first chunk's worth of audio and then continues in an "I see you started a sentence, let me complete the topic" mode.
2. **Sanity log** at init: `audio_soft_token_id`, `proj_dim` vs `d_model`, "audio projection weights found" — even though the report #07 control test ruled these out, the original report #02 asked for them and they're cheap to surface.

### P5 — mimo-asr tokenizer GGUF not in auto-download manifest (report 06) — DONE `b936b488` 2026-05-26

**Shipped:** `src/crispasr_model_registry.cpp` mimo-asr entry now declares `mimo-tokenizer-q4_k.gguf` from `cstr/mimo-tokenizer-GGUF` as the companion file, so `--auto-download` fetches both LM and tokenizer into `~/.cache/crispasr/` where `discover_audio_tokenizer()` finds it without further configuration. Error message in `crispasr_backend_mimo_asr.cpp` now spells out three options for resolving a missing tokenizer (`--auto-download`, `--codec-model`, or manual `hf download`). `docs/architecture.md` mimo-asr section documents the tokenizer-is-a-separate-file requirement and the `--codec-model` override. Confirmed that `discover_audio_tokenizer`'s candidate list already includes the canonical filename, so the auto-download lands where the runtime looks.



**TL;DR.** `--auto-download` fetches the 36-layer Qwen2-based LM but **not** the separate `mimo-tokenizer-q4_k.gguf` (~395 MB audio tokenizer) required to actually transcribe. The user gets exit code 1 with "no audio tokenizer GGUF found. Pass --codec-model PATH or place mimo-tokenizer-q4_k.gguf next to the LM" and a 0-byte output. The `--codec-model` flag is undocumented; `docs/cli.md` doesn't mention it under the mimo-asr section.

Reporter's verified workaround (report #06):

```bash
hf download cstr/mimo-tokenizer-GGUF --local-dir ~/.cache/crispasr/mimo-tokenizer
crispasr --backend mimo-asr -m auto --auto-download \
  --codec-model ~/.cache/crispasr/mimo-tokenizer/mimo-tokenizer-q4_k.gguf \
  -f samples/jfk.wav -l en -np -nt
```

This works on v0.6.9 (per report #12). On v0.6.10 it still hits the P0 segfault above; needs P0 to land first before this is testable.

**Fixes:**

1. **Add `mimo-tokenizer-q4_k.gguf` to the auto-download manifest** for the mimo-asr backend. Source: `cstr/mimo-tokenizer-GGUF`.
2. **Document `--codec-model`** under the mimo-asr section of `docs/cli.md`. Include `discover_audio_tokenizer()`'s search-path convention (the three filenames it tries next to the LM).
3. **Improve the error message** to include a concrete `huggingface-cli download` line — the current message tells the user the flag exists but not how to populate the file.

### P6 — kyutai-stt: three separate issues (reports 05, 10, 11)

**TL;DR.** kyutai-stt has three distinct bugs, all visible on short audio.

#### P6a. Drops the final word on the 11 s JFK clip (report 10) — DONE `ba0e388e` 2026-05-26

**Shipped:** `crispasr_backend_kyutai_stt.cpp::transcribe()` constructs a padded buffer of `n_samples + 8000` (500 ms @ 16 kHz of zeros) and feeds *that* to `kyutai_stt_transcribe_ex`. Tokens emitted during the silence-tail keep their `t_offset_cs` arithmetic and land a few cs past the original input end; word timestamps stay correct since the model is causal. Local M1 Metal smoke: `samples/jfk.wav` now produces "And so, my fellow Americans, ask not what your country can do for you, ask what you can do for your country." (full final word + sentence-end punctuation, was truncated to "...your c" before).

#### P6b. 0.07× RT on 50 min file (reports 05, 11) — DONE `043b3ae5` 2026-05-26

**Shipped:** `crispasr_backend_kyutai_stt.cpp::transcribe()` extracted the per-call logic into a `transcribe_one()` helper and wraps with a chunking loop that splits inputs > 30 s into 30 s windows. Each chunk gets its own silence-tail flush from P6a so boundaries close cleanly; per-chunk `t_offset_cs` = `caller_offset + chunk_start / 16 kHz * 100 cs`, so token + word timestamps land in the right global window. Local M1 Metal validation on `first90.wav` (the 90 s Japanese clip from issue #89, the original long-audio failure case) produced a coherent three-chunk transcript in 568 s wall = 6.3 s/s — finite and linear, vs the previously-reported 14 s/s degradation that grew worse with input length. The single-chunk path (n_samples ≤ 30 s) is unchanged, so the JFK regression test still passes. GPU offload (`docs/architecture.md:248` TODO) remains the longer-term remedy for absolute wallclock on CPU.

#### P6c. Streaming model on a batch dispatcher fundamentally mismatched — DEFERRED

The architecture entry at `docs/architecture.md:176` lists kyutai-stt as "Mimi codec + causal LM | CPU". The reporter's evidence supports treating this backend as **streaming-only** in the CLI — the batch path is a footgun. With P6b shipped, the wallclock is bounded and linear (no more "hung" appearance), so the defensive `--force-long-audio` cap is now a UX nicety rather than a correctness fix. Deferred until a user reports it; we have higher-value followups for the next session.

### Cross-finding observations from the issue

- **JFK as the universal control test.** Reports #07 and #12 both demonstrate that running the project's own 11 s fixture isolates "model is dead" from "long-audio dispatcher is broken" in <2 minutes — the two failure classes have completely different fix sites. This is the reporter's most actionable methodology observation. Future "broken backend" reports should run JFK first.

- **Backend registry coverage.** Reporter found four backends missing from `tools/test-all-backends.py` (funasr, fun-asr-mlt-nano, sensevoice, paraformer). The script advertises itself as the source of truth in `docs/regression-matrix.md`; a gap there means a backend can ship broken indefinitely. Worth a parallel audit for the 18-backend registry the script does cover, to confirm nothing else has silently grown out of date.

- **Capability-flag honesty.** firered-asr declaring `CAP_UNBOUNDED_INPUT` while having a 50 s PE window is the same class of failure as the previous voxtral / cohere / gemma4-e2b / glm-asr / kyutai-stt cases that drove the opt-out fix train (`dc2295b2` etc.). Worth a defensive sweep of every `CAP_UNBOUNDED_INPUT` declaration to confirm the *encoder* actually is unbounded, not just the dispatcher's input shape.

- **GPU validation matrix.** The 2026-05-24 `#81 #06 FA per-head mask` patch was validated only on parakeet-tdt-0.6b-v3 on A1000 sm_86; the present mimo-asr regression on Blackwell sm_120 is the proximate cost of that narrow validation. The `tools/upstream-prs/06-cuda-fa-perhead-mask.md` PR draft should not be submitted upstream until validated on a wider matrix.

### Priority for this PLAN section

Reporter's classification + ours:

| # | finding | reporter severity | our action priority |
|---|---|---|---|
| 12 | mimo-asr segfault on Blackwell (regression from `6b492b2b`) | regression | **P0** |
| 01/07/08/09 | funasr / fun-asr-mlt-nano `!`-loop + no CI coverage | broken backend | P1 |
| 04 | firered-asr `CAP_UNBOUNDED_INPUT` + 50 s PE window | broken on long audio, model OK | P2 |
| 03 | omniasr-llm `is_streaming` chunking gate | broken on long audio, model OK | P3 |
| 02 | gemma4-e2b long-audio hallucinations | broken on long audio, model OK | P4 |
| 06 | mimo-asr auto-download manifest gap | UX bug, blocked by P0 segfault | P5 |
| 05/10/11 | kyutai-stt batch path slow + final-word truncation | partly design-limit, partly bug | P6 |

PLAN #115 (existing) folds into P0 + P5 here.

### Files (tentative)

- `ggml/src/ggml-cuda/CMakeLists.txt` — default `GGML_CUDA_CRISPASR_FA_PERHEAD_MASK` to OFF for non-parakeet builds, or gate it on a backend capability declaration
- `ggml/src/ggml-cuda/fattn-mma-f16.cuh`, `fattn.cu` — kernel gate tightening (P0 option 2)
- `tools/test-all-backends.py` — add funasr / fun-asr-mlt-nano / sensevoice / paraformer registry entries (P1)
- `src/funasr.cpp` — degenerate-loop guard + per-variant prompt selection + `params.language` wiring (P1)
- `examples/cli/crispasr_backend_firered_asr.cpp` — drop `CAP_UNBOUNDED_INPUT` (P2)
- `src/firered_asr.cpp` — length check + clear error past `pe_maxlen` (P2)
- `src/omniasr.cpp` — `is_streaming || force_seg` chunking gate (P3)
- `examples/cli/crispasr_backend_gemma4_e2b.cpp` — chunking audit (P4)
- `examples/cli/crispasr_backend_mimo_asr.cpp` + auto-download manifest — fold tokenizer in (P5)
- `docs/cli.md` — document `--codec-model` for mimo-asr (P5)
- `examples/cli/crispasr_backend_kyutai_stt.cpp` — silence tail or `finalize` (P6a), 30 s chunking (P6b)
- `tests/test-*` — regression assertions per fix
- `tools/upstream-prs/06-cuda-fa-perhead-mask.md` — do not submit upstream until P0 wider-matrix validation lands

### Trigger conditions for completion

- mimo-asr v0.6.11 (next release) does not segfault on Blackwell, validated by montvid or by us on the same GPU class.
- funasr + fun-asr-mlt-nano produce a non-degenerate transcript on JFK; CI regression assertion in place.
- firered-asr, omniasr-llm, gemma4-e2b all transcribe a 5-min EN clip without hangs, dropped content, or hallucinations.
- mimo-asr `--auto-download` fetches both LM and tokenizer; `docs/cli.md` documents `--codec-model`.
- kyutai-stt JFK transcript ends on `country.`; 5-min EN clip completes in linear wall-time.

Reporter contact: `montvid` on GitHub issue #125. The 12 reports are reproducible verbatim; their environment is well-documented enough that we can re-run on our VPS to cross-check before claiming any fix.

---

## 127. Coverage gaps from the 2026-05-26 overlap-save sweep close-out

Three small holes the sweep + #115 bisect surfaced. None are urgent; recording so the next contributor doesn't rediscover the same gaps.

### a. omniasr-llm — overlap-save bug status unknown

The original 5 min sweep and the 90 s rerun both came back `BOTH_EMPTY` for `omniasr-llm-300m-v2-q4_k.gguf`: default and `--chunk-overlap 0` both hit the 20 min per-pass wallclock on M1. Probably *slow*, possibly *also has the truncation bug like its sibling backends*. Can't tell without a faster box.

**Fix shape.** Re-run `./tools/check-overlap-save-bug.sh omniasr-llm` with `PER_RUN_TIMEOUT=2400` on a Linux x86 host (the VPS) or a Kaggle CPU kernel. If default produces materially less output than no-overlap, add to the opt-out list in `examples/cli/crispasr_chunk_context_gate.h`. If both produce the same content, mark VERIFIED-OK in the harness comment.

### b. mimo-asr — local test coverage is in place but doesn't run in CI

`tools/test-all-backends.py` has had a `mimo-asr` registry entry since 2026-05-02 (commit `2aeaf4c4`); the `test_transcribe` function explicitly handles `EMPTY` output (line 693-695). Locally the test would have caught PLAN #115's silent-empty regression at runtime — but CI doesn't run `test-all-backends.py` against large-model backends (mimo Q4_K is 4.2 GB, doesn't fit in the standard runner disk budget per pre-release), so the regression shipped in v0.6.10 anyway.

**Fix shape.** Either (a) Kaggle scheduled-CI workflow that runs the full `test-all-backends.py` against the 4 LLM-class backends (mimo, voxtral, gemma4-e2b, granite-4.1-2b) on each main push — patterns in `tools/kaggle/crispasr-regression.py` already handle the model-download + heartbeat parts; (b) cheaper, a documented `make smoke-llm-backends` target that release scripts run before tagging. (a) is more reliable; (b) is one afternoon of work.

### c. cohere-asr-ja-v0.1 — no benchmark numbers in PERFORMANCE.md

Issue #123 added the JA variant to the registry + README, but no row in any of `PERFORMANCE.md`'s cohere tables (the long-form coverage at line 1374, the cross-backend matrix at line 1535, the per-length wall-time table at line 1555). The English `cohere-transcribe` is benchmarked across multiple Japanese / English / multilingual clips; the JA fine-tune isn't.

**Fix shape.** Run the JA variant on the same fixture set the English one used (TedX / JSUT clips per the model card; `samples/jfk.wav` is English so won't exercise the JA tuning). Drop one extra row into each cohere table with the JA numbers. ~30 min of inference + table updates once the fixtures are downloaded.

---

## 128. Piper TTS — lightweight VITS runtime (MIT)

Native C++ runtime for [rhasspy/piper](https://github.com/rhasspy/piper)
VITS models. MIT-licensed. Fills a gap in the TTS lineup: tiny models
(~15-50 MB Q4_K per language) with zero-shot latency, useful on mobile
(CrisperWeaver) and for fast previews.

### Why

Current smallest TTS is Kokoro at ~75 MB. Piper voices are **~15 MB**
Q4_K per language and run in single-digit ms per sentence on CPU.
250+ community voices across 30+ languages. Especially strong for
German (thorsten-medium 6.1% MOS, karlsson, kerstin, pavoque, ramona,
eva_k). The "just works" option when download budget is tight.

### Architecture

VITS (Variational Inference with adversarial learning for end-to-end
Text-to-Speech):
- Text encoder: small transformer (6 layers, 192-d)
- Duration predictor: 2-layer conv (already in `core/conv.h`)
- Flow: 4 affine coupling layers (inverse autoregressive flow)
- HiFi-GAN decoder: 4 upsample blocks + multi-receptive-field fusion

Phoneme frontend: **espeak-ng** — already vendored in the tree for
Kokoro (#56). The `kokoro.cpp` espeak-ng integration
(`espeak_TextToPhonemes`) is directly reusable; Piper's phoneme
alphabet is espeak-ng IPA, same as Kokoro's.

### Reuse from existing code

| Component | Reuse source | New code needed |
|---|---|---|
| espeak-ng phonemizer | `kokoro.cpp` `espeak_TextToPhonemes` | None — same API |
| Text encoder (transformer) | `core/attention.h` `core_attn::kv_self_attn` + `core/ffn.h` | Minimal glue |
| 1D convolutions | `core/conv.h` (`core_conv_1d`, `core_conv_1d_dw`) | None |
| Duration predictor | `core/conv.h` | ~50 LOC adapter |
| Affine coupling flow | **NEW** | ~200 LOC — `core/affine_coupling.h` |
| HiFi-GAN decoder | `chatterbox_s3gen.cpp` HiFT vocoder (4 upsample + MRF) | Adapt from chatterbox; ~300 LOC delta |
| iSTFT / audio output | `core/fft.h` + `chatterbox_s3gen.cpp` istft | None |
| GGUF loader | `core/gguf_loader.h` | None |
| Audio resampler | `core/audio_resample.h` | None |

The affine coupling layer is the only truly new primitive. It's a
simple invertible transform: `y = x * exp(s(x)) + t(x)` where `s`
and `t` are small conv nets. ~200 LOC including forward + inverse.
**This should go into `core/affine_coupling.h`** per DRY — it will
also be needed by MeloTTS (#100 Phase A, same VITS family) and any
future normalizing-flow TTS.

### Concrete steps

1. **Converter** — `models/convert-piper-to-gguf.py`. Read the `.onnx`
   + `.onnx.json` config. Export text encoder, duration predictor, flow
   coupling layers, HiFi-GAN decoder as GGUF tensors. Embed the
   phoneme-to-id map as GGUF KV metadata.
2. **`core/affine_coupling.h`** — forward-pass affine coupling layer.
   Input: (B, C, T) → split channels → compute s,t via conv stack →
   apply transform → concat. ~200 LOC. Reusable by #100 MeloTTS.
3. **`src/piper_tts.cpp` + `src/piper_tts.h`** — backend runtime.
   - `piper_tts_init_from_file(path)` → load GGUF, build encoder +
     flow + decoder graphs
   - `piper_tts_synthesize(ctx, text)` → espeak-ng phonemize → encoder
     → duration → flow → HiFi-GAN → float32 PCM
   - Wire into Session API (`crispasr_c_api.cpp`)
4. **Registry** — add `piper` backend to `crispasr_model_registry.cpp`.
   Host converted GGUFs at `cstr/piper-*-GGUF` on HF.
5. **Test** — ASR roundtrip: piper synth → parakeet transcribe → verify.
   Add to `tools/test-all-backends.py`.

### Effort

**Small-Medium.** espeak-ng and HiFi-GAN are already in the tree.
The affine coupling is small. Main work is the converter + wiring.
~1-2 days for a working EN/DE prototype, +1 day per additional
language voice.

### Trigger

Immediate — Piper's size/speed makes it the best candidate for
CrisperWeaver mobile and HF Space demos. Also the simplest new
TTS architecture to add (no LLM, no codec, no diffusion).

---

## 129. F5-TTS — DiT flow-matching TTS (MIT)

Native C++ runtime for [SWivid/F5-TTS](https://github.com/SWivid/F5-TTS).
MIT-licensed. High-quality zero-shot voice cloning from 5-15s of
reference audio.

### Why

F5-TTS's architecture (Diffusion Transformer + flow matching) is
distinct from everything currently in the tree. It produces noticeably
higher quality than the current AR-based engines (orpheus, qwen3-tts)
for voice cloning tasks, and the MIT license is cleaner than the
llama3.2-derived Orpheus weights. ~330M params, ~660 MB F16.

### Architecture

- **Text encoder**: char-level ConvNeXt blocks (not a transformer) —
  novel in the tree
- **DiT backbone**: 22-layer Diffusion Transformer with AdaLN-Zero
  conditioning. Each layer: LayerNorm → self-attention → cross-attention
  → FFN with adaptive scale/shift from the diffusion timestep embedding
- **Flow matching**: conditional OT path (rectified flow). The ODE
  solver is Euler (simplest) or midpoint
- **Vocoder**: Vocos (iSTFT-based, ConvNeXt stack → STFT magnitudes +
  phases → iSTFT). ~14M params, separate checkpoint

### Reuse from existing code

| Component | Reuse source | New code needed |
|---|---|---|
| Self-attention | `core/attention.h` `core_attn::kv_self_attn` | None |
| Cross-attention | `core/attention.h` | None |
| FFN (SwiGLU) | `core/ffn.h` `core_ffn::swiglu` | None |
| AdaLN-Zero | **NEW** — `core/adaln.h` | ~100 LOC |
| ODE solver (Euler/midpoint) | `chatterbox_s3gen.cpp` `cfm_euler_solve` | Adapt — ~50 LOC delta |
| iSTFT | `core/fft.h` + `chatterbox_s3gen.cpp` | None |
| Mel spectrogram | `core/mel.h` | None |
| ConvNeXt block | **NEW** — `core/convnext.h` | ~150 LOC |
| GGUF loader | `core/gguf_loader.h` | None |
| Reference audio embedding | `core/mel.h` + concat | ~50 LOC |

**New `core/` primitives** (both reusable by other future models):
- `core/adaln.h` — Adaptive Layer Norm with zero-init scale/shift.
  Used by all DiT-family models. ~100 LOC. Would also serve any
  future DiT image/video model if one is ever ported.
- `core/convnext.h` — ConvNeXt V2 block (depthwise conv → LayerNorm →
  pointwise up → GELU → pointwise down + residual). ~150 LOC. Vocos
  vocoder and F5's text encoder both use this. MeloTTS (#100) could
  also benefit if its text encoder is ConvNeXt-flavored.

### Concrete steps

1. **Converter** — `models/convert-f5-tts-to-gguf.py`. Export DiT
   (22 layers), text encoder (ConvNeXt), Vocos vocoder as separate or
   combined GGUF. Embed char-level vocabulary as KV metadata.
2. **`core/adaln.h`** — AdaLN-Zero: `scale, shift = linear(timestep_emb)`;
   `out = (1 + scale) * layernorm(x) + shift`. Zero-init at construction.
3. **`core/convnext.h`** — ConvNeXt V2 block.
4. **`src/f5_tts.cpp` + `src/f5_tts.h`** — backend runtime.
   - Reference audio: load WAV → mel → concat with text embeddings as
     conditioning (masked infilling: text tokens mark where to generate,
     ref mel provides voice identity)
   - DiT forward: 22 layers × N ODE steps (default 32)
   - Vocos: mel → magnitude + phase → iSTFT → 24 kHz PCM
5. **Registry + C API + Session wiring.**
6. **Voice cloning API**: `session.set_voice("ref.wav", ref_text="...")` —
   same API as qwen3-tts base. Mel from ref audio is the conditioning.

### Status — **DONE** (2026-05-30)

Full native C++ runtime operational. End-to-end pipeline: WAV ref →
mel spectrogram → text tokenization → ConvNeXtV2 text encoder →
InputEmbedding + ConvPosEmbed → 32-step Euler ODE with CFG (22-layer
DiT) → Vocos vocoder (8× ConvNeXt + ISTFTHead) → 24 kHz WAV.

All 22 DiT layers match PyTorch reference at cos=1.000. ASR roundtrip
verified (whisper transcribes generated audio correctly). CLI wired
(`--backend f5-tts`), C API wired, model registry entry added.

Key bug found during port: x_transformers `RotaryEmbedding` interleaves
frequencies (`stack + rearrange`, not `cat`), so paired RoPE elements
share the same frequency — a standard rotation. Original analysis in
the handover was incorrect. See `handover-prompts/f5-tts-129-continuation.md`
Bug 10 for details.

Performance: unified ggml graph (ggml_rope_ext + ggml_flash_attn_ext)
gives 5x speedup (DiT forward ~2 min → ~24 sec per ODE step).

Quantization: F16 (953 MB) is the only viable precision. Q8_0/Q4_K
tested with arch-specific conditioning-pathway skip rules — still
produce unintelligible output because QKV/FFN error compounds through
1408 iterative forward passes. `crispasr-quantize` skips F5-TTS.
HF repo `cstr/f5-tts-GGUF` has the F16 GGUF only.

---

## 130. Zonos TTS — transformer + DAC codec (Apache 2.0)

**DONE (2026-06-09).**

E2E verified (ASR roundtrip verbatim on Q8_0 + selective Q4_K). Key
fixes: RoPE NEOX→NORMAL (x_transformers consecutive-pair), GatedMLP
gate-chunk order, delay-pattern `step >= k`. Plain Q4_K is unusable
(EOS logit inflated) — ship F16/Q8_0/selective-Q4_K only. Detail →
HISTORY / LEARNINGS.

## 131. OuteTTS — LLM + WavTokenizer codec (CC BY 4.0)

Native C++ runtime for [OuteAI/OuteTTS](https://github.com/OuteAI/OuteTTS).
CC BY 4.0 (commercial OK with attribution). Zero-shot voice cloning
from a brief reference clip.

### Why

OuteTTS is architecturally closest to what CrispASR already runs well:
a GPT-style LLM generating discrete audio tokens decoded by a learned
codec. The pattern is nearly identical to orpheus (Llama → SNAC) and
indextts (GPT-2 → BigVGAN). This makes it the **lowest-effort new TTS
backend** among the four — mostly converter + registry work, minimal
new runtime code.

### Architecture

- **LLM backbone**: Llama-style 1B transformer (OuteTTS-0.3-1B uses
  a custom 1B arch; OuteTTS-0.2-500M uses a 500M variant)
- **Audio tokenizer**: WavTokenizer — single-codebook VQ at 40 or 75 Hz.
  Encoder: conv stack → quantize. Decoder: conv stack → 24 kHz waveform.
  Structurally simpler than SNAC (1 codebook vs 3) but similar decode
  pattern
- **Voice cloning**: reference audio → WavTokenizer encode → prepend
  tokens to LLM context. Same in-context-learning pattern as
  qwen3-tts-base

### Reuse from existing code

| Component | Reuse source | New code needed |
|---|---|---|
| Llama-style AR forward | `orpheus.cpp` / `qwen3_tts.cpp` | Minimal — same core_attn + core_ffn |
| KV cache + greedy/beam | `core/greedy_decode.h` / `core/beam_decode.h` | None |
| Conv-stack codec decoder | `orpheus_snac.cpp` / `indextts_voc.cpp` | Adapt for WavTokenizer strides — ~200 LOC |
| Reference audio encoding | `core/mel.h` → WavTokenizer encoder | ~300 LOC for the encoder conv stack |
| BPE tokenizer | `core/bpe.h` | None |
| GGUF loader | `core/gguf_loader.h` | None |
| Audio resampler | `core/audio_resample.h` | None |

No new `core/` primitives needed. WavTokenizer's decoder is a
standard conv-upsample stack — put it in `src/outetts_wavtok.cpp`
(backend-specific, not generic enough for `core/` since it's a
single-codebook design unlike the multi-codebook RVQ in `core/rvq.h`).

### Concrete steps

1. **Converter** — `models/convert-outetts-to-gguf.py`. Export LLM
   weights + WavTokenizer encoder/decoder. Embed vocab + audio config
   as KV metadata.
2. **`src/outetts_wavtok.cpp`** — WavTokenizer encode (for ref audio)
   + decode (for generated tokens). ~400 LOC total.
3. **`src/outetts.cpp` + `src/outetts.h`** — backend runtime.
   - Load GGUF, build LLM + WavTokenizer graphs
   - Voice cloning: ref WAV → WavTokenizer encode → token sequence
   - AR decode: text tokens + ref tokens → generate audio tokens
   - WavTokenizer decode → 24 kHz PCM
4. **Registry + C API + Session wiring.**
5. **Test** — ASR roundtrip + voice similarity check.

### Status — DONE

_Done — see HISTORY.md + git log._

## Priority update — new TTS backends

(Merged into the main priority table above. Piper §128 DONE. F5 §129
in progress. Zonos §130 and OuteTTS §131 queued.)

Sequencing: **§128 → §129 → §130 → §131**. Piper first (smallest,
most reuse, biggest size-class gap to fill). F5 second (introduces
`core/adaln.h` + `core/convnext.h` that §130 may need). Zonos third
(introduces `core/dac_decoder.h`). OuteTTS last (least new value
given orpheus/indextts coverage).

---

## §136 — funasr CUDA !-loop fix (issue #125)

**Status:** DONE — fix confirmed on Kaggle P100 (v16, 2026-06-01).

**Root cause:** `ggml_backend_sched` with `[CUDA,CPU]` misroutes funasr's
Qwen2-0.6B LLM decoder on CUDA. Produces Inf at layer 2, all-NaN by
layer 3. Not caused by the Q/K/V split pattern specifically — v15 proved
QKV fusion alone does NOT fix it. The exact sched bug is upstream/unknown.

**Fix (commit `f94fec90`):** weight-split (encoder GPU, LLM+KV CPU) +
QKV fusion + KV zeroing. `FUNASR_LLM_GPU=1` overrides to all-GPU.
See LEARNINGS §136 for the 16-version Kaggle investigation.

**Future:** file upstream ggml issue with the minimal repro (funasr Q8_0
model, dual-backend sched, all weights on GPU → Inf at LLM layer 2).
If fixed upstream, revert the weight split via `FUNASR_LLM_GPU=1`.

---

## §138 SpeechT5 + Dia + Parler + FastPitch TTS stubs → working backends

**Status (2026-06-01):**

### SpeechT5 TTS (microsoft/speecht5_tts)
- **Encoder**: cos > 0.999 all 12 layers ✅
- **Converter**: `models/convert-speecht5-to-gguf.py` — F16 weights, F32 biases
- **Runtime**: `src/speecht5_tts.cpp` — encoder + decoder w/ KV cache + postnet + HiFi-GAN
- **GGUF**: `/mnt/storage/speecht5/speecht5-tts-f16.gguf` (300 MB)
- **Status**: Pipeline runs e2e, produces audio. Decoder content mismatch needs investigation.
- **Next**: validate decoder per-layer against Python reference

### Dia 1.6B TTS (nari-labs/Dia-1.6B)
- **Encoder**: cos = 1.000000 all 12 layers ✅
- **Decoder layer 0**: cos = 0.999 ✅
- **Decoder step 0 argmax**: channel 0 = 568, matches Python ✅
- **Converter**: `models/convert-dia-to-gguf.py` — F32 weights (scale=1.0 attention sensitive)
- **Runtime**: `src/dia_tts.cpp` — encoder + cross-attn + AR decoder (18L GQA CFG) + DAC decode
- **DAC**: `models/convert-dac-to-gguf.py` + `/mnt/storage/dia/dac-44khz.gguf` (104 MB)
- **GGUF**: `/mnt/storage/dia/dia-1.6b-f16.gguf` (3.2 GB F16)
- **Status**: 11 bugs fixed. Audio produced (2.15s) but ASR says music/noise. Full 18-layer decoder precision needs validation. CFG filtering now matches Python blueprint.
- **Next**: validate decoder layers 1-17, test with F32 GGUF, investigate DAC decode fidelity
- **Key insight**: Dia's `scale=1.0` attention (no 1/sqrt(d)) makes softmax extremely sensitive to precision. Every computation must match Python exactly or codes diverge.

### Parler TTS / FastPitch
- Not started yet — Dia and SpeechT5 took priority
- FastPitch has ~1000 LOC stub, converter exists, NeMo model needed
- Parler has ~857 LOC stub with 3 TODOs, T5 encoder + DAC decoder

**Files changed**: `src/dia_tts.cpp`, `src/speecht5_tts.cpp`, `src/core/hifigan.h`, `src/funasr.cpp`, `models/convert-dia-to-gguf.py`, `models/convert-speecht5-to-gguf.py`

## §139 Beam search — remaining ASR backends (issue #136 follow-up)

Parakeet TDT/RNNT beam search shipped in `b3cdcebd` (2026-06-01).
Whisper, glm-asr, kyutai-stt, moonshine, firered-asr, granite,
qwen3, voxtral, omniasr already had beam search via `core_beam_decode.h`
or native runtime support. This section tracks the remaining gaps.

### Current coverage

| Backend | Beam search | Mechanism |
|---|---|---|
| whisper | ✔ | native upstream |
| parakeet | ✔ | TDT/RNNT label-looping beam (`b3cdcebd`) |
| granite / granite-4.1 / granite-4.1-plus | ✔ | `core_beam_decode` replay-from-prefix |
| qwen3-asr | ✔ | `core_beam_decode` replay-from-prefix |
| voxtral | ✔ | `core_beam_decode` replay-from-prefix |
| glm-asr | ✔ | `core_beam_decode` branched KV snapshots |
| kyutai-stt | ✔ | `core_beam_decode` branched KV snapshots |
| moonshine | ✔ | native `moonshine_set_beam_size` |
| firered-asr | ✔ | native beam (default beam=3) |
| omniasr | ✔ | wired; CTC variant ignores |
| gemma4-e2b | ✔ | `core_beam_decode` replay-from-prefix (`f5b28564`) |
| canary | ✔ | `core_beam_decode` branched KV snapshots (§90 runtime + `f5b28564` adapter) |
| cohere | ✔ | `core_beam_decode` branched KV snapshots (§90 runtime + `f5b28564` adapter) |
| m2m100 | ✔ | `core_beam_decode` replay-from-prefix (`84d86a99`) |
| madlad/t5 | ✔ | `core_beam_decode` replay-from-prefix (`84d86a99`) |
| moonshine-streaming | ✔ | `core_beam_decode` branched KV snapshots (`61136713`) |
| funasr | ✔ | `core_beam_decode` replay-from-prefix (`206e6e2a`) |
| voxtral4b | ✔ | `core_beam_decode` replay + audio adapter injection (`f4d9b803`) |

### Done — shipped 2026-06-02

**gemma4-e2b** — DONE (`f5b28564`). Replay-from-prefix beam via
`core_beam_decode`. `gemma4_e2b_set_beam_size()` API. No local model
to benchmark (auto-download is ~3.3 GB); validated compilation.

**canary** — DONE (`f5b28564`). Runtime beam existed from §90;
adapter wiring added (`CAP_BEAM_SEARCH` + `canary_set_beam_size()`
call). Fixed pre-existing bug: beam path skipped `spiece_to_text()`.
Benchmarked: beam=4 +84 % user time, identical text.

**cohere** — DONE (`f5b28564`). Same as canary — runtime existed,
adapter wiring added. Benchmarked: beam=4 +30 % user time, identical
text. OOMs on longer audio at beam=4 with the F16 model (KV snapshot
size).

**m2m100** — DONE (`84d86a99`). Replay-from-prefix beam for text
translation. Benchmarked: beam=4 is 3.4-6.4× user time; identical
output on clean inputs.

**madlad/t5** — DONE (`84d86a99`). Same pattern as m2m100.

**moonshine-streaming** — DONE (`61136713`). Extracted per-step
decode into a lambda, wired `run_with_probs_branched` with CPU-side
KV snapshot/restore (self-attention only). Benchmarked: beam=4
+56 % user time on tiny Q4_K, identical text.

**funasr** — DONE (`206e6e2a`). The "monolithic API" assessment was
wrong — `funasr_embed_tokens` + `funasr_run_llm_step` were already
factored out. Standard `core_beam_decode::run_with_probs` replay.

**voxtral4b** — DONE (`f4d9b803`). Replay lambda injects audio
adapter frames at the correct offsets during suffix replay, matching
the streaming pre_hook's behavior. No local model to benchmark.

### Done — shipped 2026-06-15 (§167 batch)

**nemotron** — DONE (`641c701e`). RNNT beam search ported from
parakeet's `parakeet_rnnt_beam_decode`. Also MAES (`ec6507d2`).
Benchmarked: beam=4 2.16× decode time, identical text. MAES removes
spurious `<en-US>` tags and improves punctuation.

**moss-audio** — DONE (`5c437124`). Full `core_beam_decode::run_with_probs`
replay wired. 4B model needs Kaggle for A/B testing.

**granite-nle** — DONE (`fe94e976`). CTC beam via
`core_ctc::prefix_beam_search` with gamma in the BPE-CTC decode step.
Compile-verified; no local model.

**mimo-asr** — API STUB (`5c437124`). `set_beam_size` setter + field;
actual beam decode blocked on PLAN #115 runtime stability. 9-stream
architecture needs KV snapshot/restore.

**lfm2-audio** — API STUB (`9511dd5b`). Hybrid Mamba+attention needs
both KV and conv state save/restore. Deferred to Kaggle session.

### Not applicable

| Backend | Reason |
|---|---|
| wav2vec2, hubert, data2vec | CTC beam via `core_ctc` (already has gamma) |
| fastconformer-ctc | CTC-only |
| sensevoice | CTC beam via `core_ctc` (already wired: `sensevoice_set_beam_size`) |
| paraformer | Non-autoregressive (CIF-based) |

### Priority (remaining)

1. ~~**gemma4-e2b**~~ — **DONE**
2. ~~**canary**~~ — **DONE**
3. ~~**cohere**~~ — **DONE**
4. ~~**m2m100**~~ — **DONE**
5. ~~**madlad/t5**~~ — **DONE**
6. ~~**moonshine-streaming**~~ — **DONE**
7. ~~**funasr**~~ — **DONE** (was easier than expected)
8. ~~**voxtral4b**~~ — **DONE** (adapter injection in replay lambda)
9. ~~**nemotron**~~ — **DONE** (RNNT beam + MAES, §167a/b)
10. ~~**moss-audio**~~ — **DONE** (replay, §167g)
11. ~~**granite-nle**~~ — **DONE** (CTC beam + gamma, §167e)
12. **mimo-asr** — API stub, blocked on PLAN #115
13. **lfm2-audio** — API stub, needs KV+conv save/restore

**Score: 21 of 24 ASR backends now support beam search** (was 18 at
§139 close). mimo-asr and lfm2-audio have API stubs, blocked on
runtime issues. paraformer is NAR (not applicable).

## §140 GPU / ggml_backend_sched for CPU-only TTS backends — MOSTLY DONE

**Status (2026-06-08):** 6/6 backends now have `ggml_backend_sched` wired.
speecht5/piper/parler-tts/outetts were already migrated (discovered during
audit). pocket-tts migrated this session: `core_gguf::load_weights` (mmap) +
`ggml_backend_sched` + `ggml_backend_init_best`. All 6 have `use_gpu`
passed via `g_open_use_gpu_tls` from the C API.

| Backend | GPU status | Notes |
|---|---|---|
| **fastpitch** | **DONE** | sched + init_best + core_gguf::load_weights. CUDA fix §204 (`69dc1789`): decoder+vocoder graphs must `sched_graph_compute`, not `backend_cpu` compute (see step 6) |
| **speecht5** | **DONE** | sched + init_best (use_gpu default=true). Shared `core_hifigan` CUDA fix §204 (`69dc1789`) |
| **piper** | **DONE** | sched + init_best (use_gpu default=false in params, overridden by C API) |
| **parler-tts** | **DONE** | sched + init_best (use_gpu default=false in params, overridden by C API) |
| **outetts** | **DONE** | sched + init_best (use_gpu default=false in params, overridden by C API) |
| **pocket-tts** | **DONE** | migrated 2026-06-08: core_gguf::load_weights + sched + init_best |

### Pattern (from FastPitch)

1. Add `#include "core/gguf_loader.h"` + `#include "ggml-alloc.h"`
2. Replace raw `gguf_init_from_file` weight loading with `core_gguf::load_weights(path, backend, ...)`
3. Add `ggml_backend_t backend` + `ggml_backend_sched_t sched` to context
4. Init: `backend = params.use_gpu ? ggml_backend_init_best() : backend_cpu`
5. Create sched: `ggml_backend_sched_new(backends, nullptr, n_be, graph_size, false, false)`
6. Each sub-graph: `sched_reset` → `sched_alloc_graph` → set inputs → `sched_graph_compute`
   **Gotcha (§204):** once a graph is `sched_alloc_graph`'d it MUST be run with
   `ggml_backend_sched_graph_compute(sched, gf)`, never
   `ggml_backend_graph_compute(backend_cpu, gf)`. The latter dereferences
   GPU-resident weights on the CPU backend — a no-op on unified-memory Metal but
   an illegal access on CUDA. fastpitch shipped with this mistake on its
   decoder+vocoder graphs (the encoder/pitch graphs were correct).
7. Free: `sched_free` → `buffer_free` → `backend_free` (GPU before CPU)
8. Wire `p.use_gpu = g_open_use_gpu_tls` in crispasr_c_api.cpp open dispatch

---

## 155. CONV_TRANSPOSE_1D GPU optimization (issue #155)

**Status (2026-06-10):** Core decomposition landed in `5f600f25` (PR #160
by @Rafa00127, cleaned up). New `GGML_OP_COL2IM_1D` op with CPU (F32) and
CUDA (F32/F16/BF16) kernels. Qwen3-TTS codec: **1200 ms → 130 ms** (9×).

**Approach:** Decompose `conv_transpose_1d(w, x, stride)` into:
1. Pre-permute `w[K, OC, IC]` → `w_perm[IC, K*OC]` at load time
2. `col = ggml_mul_mat(w_perm, x)` — highly-optimized GEMM
3. `y = ggml_col2im_1d(col, stride, OC, p0)` — lightweight gather
4. Crop + transpose → channels-first output

The old `ggml_conv_transpose_1d` stays as fallback when `w_perm == NULL`.

### Phase 1: Generalize conv.h helpers — IN PROGRESS

Add `convt1d_decomp()` to `src/core/conv.h` — general-purpose version of
`convt1d_causal_decomp()` that supports symmetric cropping (crop_left =
crop_right, used by most decoders) not just causal right-trim.

Add `permute_convt1d_weight()` utility — de-duplicates the inline permutation
lambda from qwen3_tts.cpp for reuse across all backends.

### Phase 2: Wire into shared decoder headers

**2a. `src/core/hifigan.h`** — HiFi-GAN vocoder (SpeechT5, FastPitch,
      Kokoro, MeloTTS, OpenVoice2, Piper-TTS)
      Symmetric crop: `pad = (K − stride) / 2`.
      Add `up_w_perm` field, branch in `conv_transpose_1d()`.

**2b. `src/core/seanet_decoder.h`** — SEANet family (SNAC/Orpheus, future
      CSM/Bark/Mimi).
      Symmetric crop: `crop_left = crop_right = stride / 2`.
      Add `up_w_perm` to `BlockSlots`, branch in `build_decoder_block()`.

**2c. `src/core/dac_decoder.h`** — DAC decoder (Zonos, Parler, Dia).
      Symmetric crop: `pad = stride / 2`.
      Add `up_w_perm` to `DacDecoderBlock`, branch in `convt1d()`.

### Phase 3: Wire into standalone runtimes

**3a. `src/kokoro.cpp`** — HIGH PRIORITY. Has CPU-pinning workaround for
      Metal `conv_transpose_1d` hang. Decomposition eliminates the hack.
**3b. `src/indextts_voc.cpp`** — BigVGAN v2 upsample blocks.
**3c. `src/chatterbox_s3gen.cpp`** — S3Gen vocoder.
**3d. `src/audioseal.cpp`** — decoder + detector.
**3e. Remaining** — csm_tts, vibevoice, voxcpm2_tts, tada_codec,
      pocket_tts, kugelaudio (single ConvTranspose1d each, lower priority).

### Phase 4: Metal kernel for `GGML_OP_COL2IM_1D`

Port the CUDA gather kernel to Metal. Simple kernel (1 thread per output
element). Files: `ggml-metal.metal` (shader), `ggml-metal-impl.h` (kargs),
`ggml-metal-ops.cpp` (dispatch), `ggml-metal-device.cpp` (pipeline lookup),
`ggml-metal-device.m` (supports_op), `ggml-metal-ops.h` / `ggml-metal-device.h`
(declarations).

### Phase 5: Remove Kokoro Metal workaround

Once Phase 4 lands, remove the CPU-pinning hack in kokoro.cpp (the
`conv_transpose_1d` pin loop ~line 2330).

### Implementation order

Phase 1 → 2a–2c → 3a → 4 → 5 → 3b–3e (incremental).

**Applies to:** qwen3-tts codec (done), orpheus SNAC, outetts WavTokenizer,
pocket-tts Mimi, Zonos DAC, Parler DAC, Dia DAC, Kokoro HiFi-GAN,
SpeechT5 HiFi-GAN, FastPitch HiFi-GAN, MeloTTS, OpenVoice2, Piper-TTS,
IndexTTS BigVGAN, Chatterbox S3Gen, AudioSeal, CSM Mimi, VibéVoice,
VoxCPM2, TADA codec, KugelAudio — every TTS backend with strided
upsampling conv.

---

## §TTS-PROV: TTS AI-provenance compliance (watermark, C2PA, consent, disclaimer)

**Status:** Phases 1-5 merged to main. AudioSeal needs diff-harness validation.

**Motivation:** EU AI Act Article 50 requires machine-readable marking of
AI-generated content. Voice cloning adds deepfake-specific duties: deployer
disclosure and consent. This plan covers all five layers.

### Phase 1: Watermark + file metadata ✅ MERGED (c4da639c)

- Spread-spectrum frequency-domain watermark (`crispasr_watermark.h`)
- WAV LIST/INFO chunk (ISFT="CrispASR (AI-generated audio)", ICMT)
- MP3 ID3v2 TXXX frames (AI_GENERATED=true, GENERATOR=CrispASR)
- C API: `crispasr_watermark_embed()`, `crispasr_watermark_detect()`
- Wired into server (`/v1/audio/speech` streaming + non-streaming) and CLI
- Tests: `test_watermark.cpp` (7 cases), `test_server_wav_writer.cpp` (19 cases)

### Phase 2: Consent gate for voice cloning ✅ MERGED (b47c6214)

- CLI: `--i-have-rights` flag required for `--voice <file.wav>`; refuses
  without it with clear explanation of what it attests
- Server: `consent_attestation` field required in `/v1/audio/speech` JSON
  when voice ends in `.wav`; returns 400 with guidance if missing
- Both paths log `[CONSENT]` with ISO 8601 timestamp, voice path, attestation text
- Files: `whisper_params.h` (new fields), `cli.cpp`, `crispasr_run.cpp`, `crispasr_server.cpp`

### Phase 3: Spoken disclaimer for voice-cloned output ✅ MERGED (b47c6214)

- `crispasr_tts_disclaimer.h`: synthesizes "This audio was generated by
  artificial intelligence" using the loaded TTS backend with neutral/default
  voice (not the clone). Cached after first call (thread-safe, `std::call_once`).
- Prepended with 300ms silence gap to every voice-cloned output
- Wired into CLI + server (streaming + non-streaming) paths

### Phase 4: C2PA signed provenance metadata ✅ MERGED (b47c6214)

- `cmake/Findc2pa.cmake`: find module for c2pa-c library
- `crispasr_c2pa.h`: wrapper header with C2PA manifest JSON
  (digitalSourceType=trainedAlgorithmicMedia), signing via c2pa-c
- `scripts/generate-c2pa-cert.sh`: self-signed P-256 cert (10-year validity)
- CLI flags: `--c2pa-cert`, `--c2pa-key`
- Compile-time gated on `CRISPASR_HAVE_C2PA`; no-op + startup warning when absent

### Phase 5: AudioSeal ggml port ✅ COMPLETE — 100% cosine parity

**What:** Meta AudioSeal (MIT) neural watermark — more robust than spread-spectrum
against adversarial removal, lossy compression, time-stretching.

**Parity metrics (16000 samples, F32 weights, verified 2026-06-06):**
- Full output cosine:    **1.000000**
- Watermark-only cosine: **1.000000**
- Max absolute error:    0.000302
- Watermark RMS ratio:   0.999

**Shipped:**
- `src/audioseal.h` / `src/audioseal.cpp`: ggml implementation of SEANet
  encoder-decoder. Generator embeds watermark; detector returns per-frame
  probability + decoded 16-bit message. C API: `audioseal_embed()`,
  `audioseal_detect()`, `audioseal_embed_stage()`, `audioseal_init_from_file()`.
- `models/convert-audioseal-to-gguf.py`: loads via `audioseal` package,
  remaps state_dict keys, writes 113 tensors. F32 default (89 MB), `--f16`
  for smaller (44.6 MB). Verified against live `facebook/audioseal`.
- `tools/reference_backends/audioseal_ref.py`: reference dump script.
- `tests/test_audioseal.cpp`: 10 unit + 3 live tests (53 total across suites).
- `tests/test_audioseal_cosine.cpp`: standalone cosine comparison binary.
- `--watermark-model` CLI flag to load AudioSeal GGUF as upgrade path.
- Debug: `AUDIOSEAL_DEBUG=1` for shape traces, `AUDIOSEAL_DUMP_STAGES=1`
  for per-stage binary dumps to `/tmp/`.

**Key implementation details:**
- LSTM: proper recurrent gate computation with time-step unrolling
  (~12 ops/step × 50 steps × 2 layers). Zero initial state via
  `ggml_scale(x[:,0], 0)`. Outputs concatenated via `ggml_concat`.
- Padding: all encoder downsampling uses `ggml_pad_ext` for external
  padding (PyTorch F.pad semantics). NOTE: `ggml_pad_ext` has reversed
  parameter convention — `lp0` = RIGHT padding, `rp0` = LEFT padding.
- Decoder: ConvTranspose1d with manual output crop matching PyTorch's
  padding removal. Crop offset also uses the reversed convention.
- Message: `nn.Embedding(32, 128)` via `ggml_get_rows` + `ggml_sum_rows`,
  broadcast via `ggml_repeat`.
- Graph size: 8192 nodes (default 2048 insufficient for full model).

**Architecture (verified against live model 2026-06-05):**

```
Generator (14.7M params, 44.6 MB GGUF):
  Encoder:  Conv1d(1,32,7) → (ResBlock+ELU+Down)×4 → LSTM(512,2) → ELU → Conv1d(512,128,7)
  Message:  Embedding(32,128) indexed by 2*bit_pos+bit_value, summed
  Decoder:  Conv1d(128,512,7) → LSTM(512,2) → (ELU+Up+ResBlock)×4 → ELU → Conv1d(32,1,7) → tanh
  Output:   input + decoder_output

Detector (8.6M params):
  Encoder:  same as generator encoder
  Reverse:  ConvTranspose1d(128,32,k=320,s=320) — back to input resolution
  Head:     Conv1d(32,18,k=1) → channels 0-1=detection, 2-17=message bits

ResBlock: ELU → Conv1d(C,C/2,k=3) → ELU → Conv1d(C/2,C,k=1) + identity skip
Ratios: [2,4,5,8] → hop_length=320 (20ms at 16kHz)
Sample rate: 16 kHz
```


## 156. Permissive G2P phonemizer

espeak-ng is GPLv3 — statically linking it makes the binary GPLv3.
This plan replaces the espeak dependency with modular, permissively-
licensed phonemization backends.

### Architecture (shipped)

```
phonemize(lang, text) → IPA string
  │
  ├─► Pre-generated IPA dict (piper-compatible, auto-download from HF)
  │     EN: 126K words (3 MB)   — 99.5% piper match
  │     DE: 667K words (23 MB)  — 100% piper match
  │     FR: 257K words (6.6 MB)
  │     ES: 600K words (18 MB)
  │
  ├─► phonemize_builtin_en(...)     CMUdict + ARPAbet→IPA (76% match)
  │     ├─ CMUdict lookup (126K words, BSD) — auto-download
  │     ├─ Neural G2P (GRU seq2seq, 29→74, ~4 KB) — from GGUF or file
  │     └─ LTS rules (digraph/trigraph)
  │
  ├─► phonemize_builtin_{de,fr,es}  OLaPh MIT dicts + LTS rules
  │     ├─ OLaPh IPA dicts — auto-download from HF
  │     └─ Per-language LTS rules (always available, zero deps)
  │
  ├─► phonemize_espeak_dlopen(...)  GPL loaded at runtime, MIT binary
  │
  └─► phonemize_espeak_popen(...)   subprocess fallback
```

**Dict sources** (hosted at `cstr/g2p-dicts` on HuggingFace):
- Pre-generated IPA dicts: phonetic data generated by running espeak-ng
  on vocabulary from CMUdict/OLaPh/open-dict-data. The IPA output is
  factual linguistic data, not GPL-encumbered (same principle as compiler
  output not inheriting the compiler's license).
- OLaPh dicts (MIT): from iisys-hof/olaph, 13 languages.
- CMUdict (BSD): from cmusphinx/cmudict, English ARPAbet.

Each language module is a header-only file (`core/g2p_XX.h`) with:
- `struct context` (dict + optional neural model)
- `word_to_ipa(ctx, word)` → IPA string
- `text_to_ipa(ctx, text)` → IPA string
- `load_*_file(dict, path)` → dict loader

### Phase 1+2 — DONE (2026-06-07)

- `core/g2p_en.h` — English: ARPAbet→IPA table (39 phonemes),
  CMUdict loader, LTS rules, GRU cell + neural G2P struct + base64
  JSON weight loader (MeloTTS format), base64 decoder
- `core/g2p_de.h` — German: LTS rules (sch/ch/ei/eu/au/sp/st/z/w/ä/ö/ü/ß),
  IPA dict loader (open-dict-data format), auto-download
- `core/g2p_fr.h` — French: LTS rules (ch/gn/nasal vowels ɑ̃/ɔ̃/ɛ̃/œ̃,
  oi→wa, eau→o, -tion→sjɔ̃, silent finals, mute -e/-es/-ent,
  intervocalic s→z, accented vowels, u→y), IPA dict loader
- `core/g2p_es.h` — Spanish: LTS rules (seseo c/z→s, b/d/g allophonic
  lenition β/ð/ɣ, yeísmo ll→ʝ, ch→tʃ, rr→trill, ñ→ɲ, jota g/j→x,
  initial r→trill vs intervocalic r→ɾ tap, silent h, qu→k)
- `espeak_dlopen.h` — cross-platform dlopen loader (3 function pointers)
- `phonemizer.h/cpp` — cascade interface with auto-loading + inventory filter
- Wired into `piper_tts.cpp` (built-in G2P as final fallback)
- Phoneme inventory validated: all output chars verified against piper's
  154-char `phoneme_id_map`. Fixed combining tie U+0361 (not in map).
  Added `filter_to_inventory()` for runtime validation.
- Vowel quality fixes: AH0→ə (was ʌ), IY0→i (was iː), ER→ɜː (was ɜːɹ),
  dropped secondary stress ˌ. Matches espeak-ng output exactly.
- Dict auto-download: CMUdict (BSD) + OLaPh dicts (MIT, 13 langs) from
  HuggingFace `cstr/g2p-dicts`. Selectable via `--g2p-dict` CLI flag
  or `CRISPASR_G2P_DICT_SOURCE` env var. open-dict-data (CC-BY-SA) as alt.
- Full wiring: CLI `--g2p-dict` → `whisper_params` → C API
  `crispasr_session_set_g2p_dict()` → Go `SetG2PDict()` → server startup.
- Tests: 202 unit assertions (85 EN + 44 DE + 22 FR + 21 ES + 30 piper)
  across 38 test cases + 4 live TTS→ASR roundtrips.

### Phase 3 — open

**a. More languages (piper-plus port)**
- piper-plus (ayutaz/piper-plus, MIT, branch: dev) has rule-based G2P
  for 8 languages. Their French (1197 lines) and Spanish (620 lines)
  are more thorough than ours (handle NFD normalization, PUA codepoint
  mapping, syllabification, stress assignment). Consider porting their
  implementations for higher accuracy.
- No German in piper-plus yet — our `g2p_de.h` fills that gap.

| Language | LTS rules | Dictionary (OLaPh MIT) | Status |
|----------|-----------|------------------------|--------|
| English | CMUdict + neural G2P + LTS | 13 MB (en-us) | **DONE** |
| German | Auslautverh. + open-syl + compound | 40 MB (1.12M) | **DONE** |
| French | nasals/oi/eau/silent finals | 6 MB | **DONE** |
| Spanish | seseo/lenition/yeísmo | 16 MB | **DONE** |
| Portuguese | — | OLaPh TBD | need LTS |
| Italian | — | 1.6 MB OLaPh | need LTS |
| Dutch | — | 5.6 MB OLaPh | need LTS |
| Swedish | — | 737 KB OLaPh | need LTS |
| Czech | — | 2.6 MB OLaPh | need LTS |
| Danish | — | 206 KB OLaPh | need LTS |
| Finnish | — | 2.9 MB OLaPh | need LTS |
| Polish | — | 1.5 MB OLaPh | need LTS |
| Japanese | — | piper-plus | need rules |
| Chinese | — | piper-plus | need rules |
| Korean | — | piper-plus | need rules |
| Other | — | espeak-ng dlopen/popen | available |

**OLaPh** (iisys-hof/olaph, MIT): multilingual phonemization framework
with dictionaries for 13 languages. German alone has 1.12M entries.
All dicts use the same `word\t/IPA/` format our loaders already support.
Paper: arXiv 2509.20086v3. Auto-downloadable via `crispasr_cache`.

**b. GGUF-embedded dicts (TTS.cpp pattern)**
- TTS.cpp embeds phonemizer rules in the GGUF itself via
  `phonemizer.rules.keys` / `phonemizer.rules.phonemes` arrays
- Could embed CMUdict / neural G2P weights per-model for zero
  external dependencies at runtime

**c. Gruut CRF (MIT) for German/multilingual OOV**
- rhasspy/gruut: dictionary + CRF G2P model (18 MB, SQLite + CRFsuite)
- Higher quality than LTS rules for unknown words (compounds, loanwords)
- CRFsuite is BSD — native C/C++ dependency
- Gruut supports: de, en, fr, es, it, nl, pt, ru, sv, cs, ar, fa, sw
- Port path: extract SQLite lexicon + CRFsuite model, write C++ feature
  extractor (~100 lines), link against libcrfsuite (BSD)
- Lower priority now that OLaPh MIT dicts (1.12M DE entries) cover most
  words; gruut adds value mainly for truly novel OOV words

**d. Neural G2P weight distribution**
- MeloTTS v3 GGUF has `melotts.g2p_en_json` (base64, ~4 KB weights)
- Could publish standalone `g2p_en.json` for download
- Weight loader already implemented in `g2p_en.h`

### Files

```
src/core/g2p_en.h          — English G2P (ARPAbet→IPA, CMUdict, LTS, neural)
src/core/g2p_de.h          — German G2P (IPA dict, LTS rules)
src/core/g2p_fr.h          — French G2P (LTS rules, nasal vowels, IPA dict)
src/core/g2p_es.h          — Spanish G2P (seseo, lenition, yeísmo, IPA dict)
src/espeak_dlopen.h        — cross-platform dlopen for libespeak-ng
src/phonemizer.h           — cascade interface + filter_to_inventory()
src/phonemizer.cpp         — implementations + auto-loading + auto-download
tests/test-g2p-en.cpp      — 85 assertions (ARPAbet, LTS, CMUdict, neural, inventory)
tests/test-g2p-de.cpp      — 30 assertions (digraphs, vowels, consonants, dict)
tests/test-g2p-fr.cpp      — 22 assertions (digraphs, nasals, vowels, silent finals)
tests/test-g2p-es.cpp      — 21 assertions (seseo, lenition, yeísmo, jota)
tests/test-espeak-phonemize.cpp — 30 assertions (piper synthesis)
tests/test-piper-roundtrip.sh   — 4 live TTS→ASR tests
```

---

## 216. Kokoro G2P technical token regression

**Issue:** [#216](https://github.com/CrispStrobe/CrispASR/issues/216) —
`C++` phonemized as `k` instead of `see plus plus` with built-in G2P.

**Root cause:** The `tokenize()` function in `core/g2p_en.h` splits on
spaces and common punctuation but not `+`, `#`, `/`, `.` within compound
tokens. `C++` stays as one token → `word_to_ipa` lowercases to `c++` →
not in CMUdict/espeak dict → LTS rules map `c` to `k` and skip unknown
`+` chars.

**Fix (DONE 2026-07-03):**
1. Added `normalize_technical_tokens()` in `core/g2p_en.h` — runs before
   `tokenize()` in `text_to_ipa()`. Expands ~40 common technical tokens
   (case-insensitive, word-boundary-aware): `C++`→`C plus plus`,
   `C#`→`C sharp`, `.NET`→`dot net`, `Node.js`→`Node J S`,
   `CI/CD`→`C I C D`, `TypeScript`→`Type Script`, etc.
2. Added `CRISPASR_KOKORO_G2P` env var in `kokoro.cpp` for strategy
   selection: `builtin-first` (default), `espeak-first`, `espeak-only`,
   `builtin-only`. Replaces hardcoded builtin-then-espeak cascade with
   configurable lambdas.
3. 10 new unit test assertions in `tests/test-g2p-en.cpp`.

---

## 157. voxcpm2-tts — Vulkan / CUDA GPU acceleration (scheduler-based)

### Context

VoxCPM2 graph-ified its AR-loop hot paths (TSLM step, LocEnc, LocDiT
CFM, VAE decode) in §96 for Metal. On Apple Silicon (unified memory)
both the graph paths (`ggml_backend_graph_compute`) and the legacy CPU
paths (`tensor_data_f32`, `matmul_mv_ggml`, `rms_norm_cpu`) access the
same shared-storage buffer, so everything works.

On discrete GPUs (Vulkan, CUDA) the default buffer is device-local VRAM
that is not host-visible. The legacy CPU paths would SIGSEGV
dereferencing GPU pointers. As of #158 fix (2026-06-07, `c6299251`),
voxcpm2 falls back to CPU when the backend buffer is not host-visible —
correct but loses all GPU acceleration.

### Why a scheduler

The Vulkan backend's `supports_buft` only accepts its own device-local
buffer type; it rejects host-pinned buffers. So there is no single
buffer type that satisfies both CPU-pointer legacy paths and GPU graph
compute. The solution is `ggml_backend_sched`:

1. Load weights to **CPU buffer** (legacy paths can dereference them).
2. Create `ggml_backend_sched` with `[gpu_backend, cpu_backend]`.
3. Graph paths call `ggml_backend_sched_graph_compute()` instead of raw
   `ggml_backend_graph_compute()`. The scheduler copies weight data to
   GPU device-local buffers as needed and allocates intermediates on GPU.
4. Legacy paths (TSLM/RALM prefill, FSQ, stop_score) stay on CPU with
   CPU-accessible weights.

This mirrors the qwen3_tts architecture (`ggml_backend_sched` +
`ggml_backend_sched_graph_compute`).

### Prerequisites / risks

- **Upstream scheduler bugs**: our tools/upstream-prs #10 documents
  dangling `src[j]` pointers across `sched_split_graph` calls (affects
  graph reuse, which the cached LocDiT/LocEnc/TSLM graphs trigger); #16
  documents cross-backend copy insertion failure for small mixed-backend
  graphs. Both need fixes applied or merged upstream before the scheduler
  is safe here.
- **galloc→sched migration**: the existing per-graph `ggml_gallocr`
  pre-reservation (`ggml_gallocr_reserve` + `ggml_gallocr_alloc_graph`)
  must be replaced with `ggml_backend_sched_reserve` +
  `ggml_backend_sched_graph_compute`. Touches every graph path.
- **No hardware to test**: need Vulkan or CUDA device to validate.

### Scope

1. `voxcpm2_init_from_file`: when `!ggml_backend_buft_is_host(buft)`,
   load weights to CPU, keep GPU backend, create `ggml_backend_sched`.
2. Replace `ctx->galloc` with `ctx->sched` (`ggml_backend_sched`).
3. For each graph entry point (`locdit_forward_graph`,
   `locenc_forward_graph`, `tslm_step_graph`, `vae_decode_graph`):
   replace `ggml_gallocr_alloc_graph` + `ggml_backend_graph_compute`
   with `ggml_backend_sched_graph_compute`.
4. Validate: diff harness `voxcpm2-q4_k.gguf` zero-shot + voice-clone.
5. Stretch: graph-ify TSLM/RALM prefill (loop `tslm_step_graph` N_pos
   times) to run prefill on GPU too.

### Status

DONE (2026-06-07, `df6cf31e`) via GPU weight mirrors — weights on CPU
for legacy paths + GPU mirror copies for graph-build functions. No
`ggml_backend_sched` needed (avoids per-call PCIe copy overhead).
Graph paths run entirely on GPU; legacy paths stay on CPU.
Memory overhead: ~2× model size on discrete GPUs.

## §163 LFM2-Audio — ASR + TTS + S2S (ALL PHASES DONE)

**Status**: Complete. 27 commits `470d56f2`–`cd77c75e`. ASR + TTS + S2S
all working, fully wired per `docs/contributing.md`, Kaggle GPU-tested.

### Phase 1 — ASR — DONE

_Done — see HISTORY.md + git log._

## §164 Mini-Omni2 — ASR + TTS + S2S (DONE)

_Done — see HISTORY.md + git log._

## §165 — beam_size default greedy + issue #161 (DONE)

_Done — see HISTORY.md + git log._

## §166 — Vulkan conv_transpose_1d_f16 + voxcpm2 graph bugs (issue #164)

### Part 1 — conv_transpose_1d_f16 shader — DONE `570bb76d`

The f16 variant passed `A_TYPE=float16_t` without enabling the required
`GL_EXT_shader_explicit_arithmetic_types_float16` extension. Strict glslang
(Vulkan SDK 1.4.350+) rejected the shader → LNK2019 on ggml-vulkan.dll.

Fix: enable the extension + cast kernel element to float in `fma()`.

### Part 2 — VOXCPM2_USE_GRAPH NaN + SIGABRT — DONE

_Done — see HISTORY.md + git log._

## §167 — Beam search, MAES, and KV caching expansion

Cross-cutting feature push to bring beam search, MAES (Modified Adaptive
Expansion Search), and KV caching to backends that lack them. Builds on
§139 (beam search round 1, all LLM backends done) and §165 (beam_size
defaults).

Branch: `feat/beam-maes-cache`

### Survey results (2026-06-15)

**Beam search gaps:**

| Backend | Architecture | Gap | Effort |
|---|---|---|---|
| nemotron | RNNT (FastConformer enc + LSTM pred) | Stub only — `set_beam_size` + field exist but `nemotron_rnnt_decode` is greedy | LOW — copy `parakeet_rnnt_beam_decode` pattern |
| sensevoice | CTC (encoder-only) | Greedy CTC; `core_ctc::prefix_beam_search` available | LOW — wire existing infra |
| granite-nle | CTC (encoder-only) | Greedy CTC; same `core_ctc` | LOW |
| mimo-asr | LLM (Qwen2 + RVQ) | No beam wired; `kv_self_attn` present | MEDIUM — `core_beam_decode` replay |
| moss-audio | LLM (InternLM2 + audio) | No beam wired; `kv_self_attn` present | MEDIUM — `core_beam_decode` replay |
| lfm2-audio | Hybrid Mamba+attention | No beam; needs KV + conv state save/restore | HIGH |

**MAES gaps:**

| Backend | Current | Gap |
|---|---|---|
| nemotron | greedy RNNT | No MAES; direct port from `parakeet_rnnt_maes_decode` |
| sensevoice | greedy CTC | `core_ctc::prefix_beam_search` gamma parameter unused |
| granite-nle | greedy CTC | Same |
| wav2vec2 | Has gamma | Already done (reference) |
| parakeet | Has full MAES | Already done (reference) |

**KV caching gaps:**

| Backend | Current | Gap |
|---|---|---|
| firered-asr | Hand-rolled per-step AED, no persistent KV | Migrate to `core_attn::kv_self_attn` |

### Phase 1 — VPS (small models, ≤1.2 GB)

**§167a: Nemotron RNNT beam search**
- Model: `nemotron-3.5-asr-streaming-0.6b-q4_k.gguf` (458 MB)
- Implement `nemotron_rnnt_beam_decode` from `parakeet_rnnt_beam_decode`
- Wire `ctx->decode_beam_size` in transcribe path
- A/B: greedy vs beam_size=4 on jfk.wav — WER, wall time, RSS

**§167b: Nemotron RNNT MAES**
- Depends on §167a
- Implement `nemotron_rnnt_maes_decode` from `parakeet_rnnt_maes_decode`
- Add `nemotron_set_maes()` + C API surface
- A/B: beam=4 vs MAES(beam=4, steps=2, gamma=2.3)

**§167c: Firered AED decoder KV cache**
- Model: `firered-asr2-aed-q4_k.gguf` (919 MB)
- Migrate decoder attention to `core_attn::kv_self_attn`
- Regression gate: identical text output
- A/B: current vs KV-cached — wall time improvement

### Phase 2 — VPS or Kaggle (medium models, need model files)

**§167d: Sensevoice CTC beam + gamma**
- Model: needs GGUF on disk (not currently available)
- Wire `sensevoice_set_beam_size` → `core_ctc::prefix_beam_search(gamma)`
- A/B: greedy vs beam=8 + gamma=2.3

**§167e: Granite-NLE CTC beam + gamma**
- Model: needs GGUF on disk
- Wire beam into `granite_nle` CTC decode
- A/B: greedy vs beam=8 + gamma=2.3

### Phase 3 — Kaggle (large models, GPU needed)

**§167f: MIMO-ASR beam** — `core_beam_decode::run_with_probs` replay
**§167g: Moss-Audio beam** — `core_beam_decode::run_with_probs` replay (4B, GPU)
**§167h: LFM2-Audio beam** — `core_beam_decode::run_with_probs_branched` + conv state

### Benchmark results (2026-06-15)

**§167a Nemotron beam** on jfk.wav (Q4_K, CPU, 4 threads, median of 3):

| | Greedy (beam=1) | Beam (beam=4) |
|---|---|---|
| Decode time | 8.8s | 19.0s (2.16×) |
| Text | `...ask not...for you. <en-US> Ask what...` | identical |
| RSS | 679 MB | 679 MB |

**§167b Nemotron MAES** on jfk.wav:

| | Beam=4 | MAES(4,2,2.3) |
|---|---|---|
| Text | `...for you. <en-US> Ask what...country. <en-US>` | `...for you, ask what...country.` |
| Quality | Spurious `<en-US>` tags, missing commas | Proper punctuation, no tags |
| Overhead | baseline | +15% decode time |

**§167c Firered KV cache**: Already implemented (sa_k/sa_v per beam).
**§167d Sensevoice CTC beam**: Already implemented (core_ctc).
**§167e–h**: Compile-verified; no local models for A/B testing.

## §169 — Qwen3-ASR ChatML language prompt (non-English script output)

**Status:** OPEN

**Problem:** Qwen3-ASR supports 30 languages including Arabic, but our
implementation (`src/qwen3_asr.cpp`) skips the ChatML prompt and builds
the token sequence as bare `<|audio_start|>...<|audio_end|>` without a
system/user message. The original HF model uses:

```
<|im_start|>system
You are a helpful assistant.<|im_end|>
<|im_start|>user
Transcribe the following audio in Arabic.
<|audio_start|><|audio_pad|>×N<|audio_end|><|im_end|>
<|im_start|>assistant
```

Without this, the model auto-detects language but may romanize
non-Latin scripts (observed: Arabic AA0010.wav → `istagel` instead of
native Arabic `استغل`; AA001.wav → `مرحبًا` works, so auto-detect is
partial).

**Impact:** All 30 Qwen3-ASR languages work in auto-detect mode for
clean audio, but explicit language selection (`-l ar`) has no effect
and ambiguous cases get romanized.

**Fix:**
1. Build the full ChatML prompt in `qwen3_asr_transcribe_with_probs`
   when `-l LANG` is set (map ISO 639-1 → English name → prompt text)
2. Fall back to bare audio-only prompt when no language specified
   (current behaviour, preserves auto-detect)
3. Wire `--language` / `-l` through the CLI adapter's transcribe call

**Files:** `src/qwen3_asr.cpp` (~50 LOC), `examples/cli/crispasr_backend.cpp`

**Effort:** MEDIUM — the ChatML token IDs (`<|im_start|>` etc.) need
to be resolved from the GGUF vocab. The word-level timestamp alignment
loop (lines 2074-2091) also needs adjustment since the prompt prefix
shifts positions.

**Test:** Arabic audio from `atishay23/Arabic_Audio` (AA001.wav →
should output `مرحبًا` in native script with `-l ar`).

**2026-06-16: GGUFs uploaded to `cstr/parakeet-unified-en-0.6b-GGUF`**
(F16 1181 MB + Q4_K). v8: n_mels=128 fix → test_rc=0 (no crash) but
transcript garbage. Tokenizer verified correct (same as parakeet-rnnt).
Root cause: synthetic config mismatch vs actual training config. Need
to diff C++ mel output against NeMo Python reference to find divergence.

## 158. transcribe_streaming for opaque-C-library backends

**Context:** `transcribe_streaming` now implemented for Qwen3, Voxtral, Granite,
Voxtral4b (all expose `_run_llm_kv` so the adapter owns the loop). Seven
remaining autoregressive ASR backends hide their decode loop inside an opaque
C library. Each needs:
1. A `*_transcribe_cb(ctx, pcm, n, fn, userdata)` C entry point added to the
   library implementation — fires `fn(token_id, text_piece, userdata)` per token.
2. A `transcribe_streaming` override in the adapter (`.cpp` in `examples/cli/`)
   that calls the new entry point and maps it to `on_text`.
3. Fall back to base-class batch path for beam search (best_of-N already
   handled by single-run greedy in the callback path).

### Backends in scope

| Backend | C library | token-text API | Notes |
|---------|-----------|----------------|-------|
| GLM-ASR | `src/glm_asr.cpp` | `glm_asr_token_text()` | BPE decode lambda already in adapter |
| Kyutai-STT | `src/kyutai_stt.cpp` | `kyutai_stt_token_text()` | |
| Gemma4-E2B | `src/gemma4_e2b.cpp` | `gemma4_e2b_token_text()` | supports translate flag |
| MiMo-ASR | `src/mimo_asr.cpp` | `mimo_asr_token_text()` | |
| MOSS-Audio | `src/moss_audio.cpp` | `moss_audio_token_text()` | |
| Moonshine-Streaming | `src/moonshine_streaming.cpp` | `moonshine_streaming_token_text()` | |
| Nemotron (RNN-T) | `src/nemotron.cpp` | `nemotron_token_to_str()` | RNN-T already emits per-frame; callback fires on non-blank frames |

### C library callback signature (uniform across all)

```c
typedef void (*crispasr_token_cb)(int token_id, const char* text, void* userdata);
int glm_asr_transcribe_cb(struct glm_asr_context* ctx,
                           const float* pcm, int n_samples,
                           crispasr_token_cb cb, void* userdata);
// ... same pattern for others
```

### Adapter pattern (same for all)

```cpp
void transcribe_streaming(const float* samples, int n_samples, int64_t,
                           const whisper_params& params,
                           crispasr_stream_callback on_text) override {
    if (params.beam_size > 1) {
        CrispasrBackend::transcribe_streaming(...);  // batch fallback
        return;
    }
    std::string acc;
    glm_asr_transcribe_cb(ctx_, samples, n_samples,
        [](int /*id*/, const char* text, void* ud) {
            auto& [a, fn] = *static_cast<std::pair<std::string, crispasr_stream_callback>*>(ud);
            a += text;
            fn(a, false);
        }, &std::make_pair(acc, on_text));  // actual impl uses local lambda
    on_text(acc, true);
}
```

**Status:** **DONE 2026-06-19** (`7f2c1f3a`). All 7 implemented.

### Implementation details

The actual callback signatures use per-token `(int tok_id, float prob, void* userdata)` —
not `text` in the callback, since text lookup via `*_token_text()` is done in the adapter.

| Backend | C entry point | BPE decode | Notes |
|---------|--------------|-----------|-------|
| GLM-ASR | step APIs (`glm_asr_embed_tokens`, `glm_asr_run_llm_kv`) | GPT-2 (Ġ→space, Ċ→newline) | Full greedy loop in adapter; EOS={59246,59253,59255} |
| MOSS-Audio | `moss_audio_process_cb` | GPT-2 full byte_decoder | `moss_audio_process` refactored to `moss_audio_process_impl` |
| Gemma4-E2B | `gemma4_e2b_transcribe_cb` | SentencePiece ▁→space | Added `gemma4_e2b_is_control_token` to filter bos/eos/sot/eot |
| Moonshine-Streaming | `moonshine_streaming_transcribe_cb` | SentencePiece ▁→space | — |
| Kyutai-STT | `kyutai_stt_transcribe_cb` | SentencePiece ▁→space | `emit_token` lambda already filters padding; callback fires inside it |
| MiMo-ASR | `mimo_asr_transcribe_cb` | GPT-2 full byte_decoder (inlined in adapter) | `core_bpe::token_bytes_to_utf8` not exported → inline table |
| Nemotron | `nemotron_transcribe_cb` | SentencePiece ▁→space | RNN-T: fires per non-blank emitted frame; `nemotron_transcribe_ex` → `nemotron_transcribe_impl` wrapper |

Forward-declaration fix needed for nemotron: `nemotron_transcribe` called `nemotron_transcribe_impl` before
its definition — added a static forward decl.

## 172. Wyoming protocol server — Home Assistant Assist

**DONE 2026-06-19** (`55bc0039`). `--wyoming-port N` starts a Wyoming
peer-to-peer JSONL/TCP server alongside the HTTP API. One `crispasr-server`
instance replaces both `wyoming-faster-whisper` and `wyoming-piper` for HA.

See HISTORY 2026-06-19 and `docs/server.md#wyoming-protocol-home-assistant-assist`.

## 173. `--tts-play` / `--tts-play-device` — local speaker output

**DONE 2026-06-19** (`c6021b43` + `e78ad149`). Plays TTS output through the
local speaker immediately after synthesis, with the watermark already embedded.

Device is opened at hardware-native rate (sampleRate=0); the PCM is pre-resampled
via linear interpolation before the device starts — avoids miniaudio's 4×
upsampler artefacts at 24→96 kHz on Core Audio devices.

See HISTORY 2026-06-19 and `docs/tts.md#local-speaker-output-tts-play`.

## §176 Runtime optimization pass — 2026-06-20 audit

**16/20 DONE.**

Full audit + per-item findings → PERFORMANCE.md "Runtime optimization
audit — 2026-06-20" and LEARNINGS "§176 runtime optimization audit
methodology". Shipped: flash-attn via core_attn (§176a), bucket cache
(§176b), BLAS (§176d), context cache (§176e), mel BLAS+OMP (§176f),
embd cache (§176g), F5 fused graph (§176h), cross-KV F16 (§176i),
iterative FFT (§176j), nemotron memmove (§176m), embed fast path
(§176o), MOSS flash (§176p), greedy alloc (§176q), beam top-K (§176r),
encoder cache 16/17 (§176s), weight pre-cache (§176t). The four OPEN
items, kept verbatim:

#### §176c Migrate host-side KV to device-resident 4D tensors

**Status:** OPEN but LOW-VALUE — **the "dominant bottleneck" premise is WRONG for
compute-bound transformer decoders** (measured on Dia 2026-07-12). Do NOT
implement without measuring the KV round-trip fraction on the specific backend
first (`DIA_BENCH`-style instrument).
**Dia — MEASURED, then DEFERRED (2026-07-12):** instrumented the host↔device
self-attn KV round-trip (`DIA_BENCH: self_kv_roundtrip`, kept in `dia_tts.cpp`).
On M1 Metal, `dia-1.6b-q4_k`, 120 steps: **upload 166 ms + readback 35 ms =
201.6 ms out of 17435 ms decode = ~1.2%**. The 1.6B transformer forward (×B=2 CFG)
dominates; the KV round-trip is negligible. A device-resident-KV rewrite is
non-bit-identical + high-risk (ggml KV write/read ordering on Metal — the
codebase's most bug-prone pattern) for a ≤1.2% ceiling → **not worth it; not
implemented.** Same lesson as §176k/§208/§214: profile before optimizing.
**SpeechT5 + Pocket-TTS:** almost certainly the same story (compute-bound
decoders) — **measure the fraction first** before any rewrite; likely also <5%.
**Effort:** Medium per backend (but expected <2% payoff — deprioritize).
**Backends (verified 2026-07-12 by code read — genuinely still host-side):**
- **SpeechT5** self-attn KV: OPEN. `decoder_kv_cache` is host `std::vector<float>`
  that GROWS per step (`insert`, `speecht5_tts.cpp:246-265`), re-uploaded whole
  every step via `ggml_backend_tensor_set` (`:1035-1041,1084`). Cross-attn KV
  already device-resident/precomputed (§202, `precompute_cross_kv` :568-644). No
  env-gated device path.
- **Dia** self-attn KV: host-side (confirmed), but **MEASURED at ~1.2% of decode →
  DEFERRED, not worth a device-KV rewrite** (see status block above). Host
  `std::vector<std::vector<float>>` grows via `insert`
  (`dia_tts.cpp:1594-1595,1955-1956`); whole past window reordered + re-uploaded
  per step (`:1891-1902`).
- **Pocket-TTS** self-attn KV: OPEN (nuance: does NOT grow — pre-sized to
  `max_seq` on the HOST, `pocket_tts.cpp:311-319,1000-1001`, advanced by
  `offset`) but still host-resident and the reordered past window is re-uploaded
  to per-step graph inputs every step (`:1207-1228`). `POCKET_MANUAL_BACKBONE`
  gates CPU-vs-graph but both use the same host KV.

**Already DONE (do NOT re-chase):** VoxCPM2 — the default `VOXCPM2_USE_GRAPH` GPU
path uses device-resident backend tensors `tslm_kv_k/v` + `ralm_kv_k/v`
(`voxcpm2_tts.cpp:488-505`), seeded once from the legacy vector then written
in-graph (no per-step full re-upload); the `std::vector` KV survives only as the
CPU-path/seed. Parler DONE (§176b+c 2026-06-21, opt-in). LFM2 + KugelAudio already
device-resident.

**Approach:** Follow IndexTTS/CSM pattern: 4D on-device tensor
`[head_dim, max_ctx, n_heads, n_layers]` with `ggml_view_4d` +
`ggml_cpy` writes. Eliminates O(step × layers × hidden) host↔device
bandwidth per step.
**Impact (REVISED by Dia measurement):** the original "eliminates the *dominant*
data-movement bottleneck" claim is FALSE for compute-bound transformer decoders —
Dia's KV round-trip is ~1.2%, not dominant. It could matter only for a small/fast
decoder with very long outputs where the transformer forward is cheap relative to
KV bandwidth; verify with a per-fraction measurement before investing. Judge by
the decoded round-trip, keep both paths gated, and expect a low-single-digit-%
ceiling on typical transformer TTS.

#### §176k FireRed ASR: decoder self-attention — PROFILED + partial fix (2026-07-12)

**Status:** MOSTLY DONE — the handover framing was WRONG; profiled + shipped the
real lever (env-gated matvec graph cache, default ON). A residual algorithmic
item stays open.
**File:** `src/firered_asr.cpp`

**Handover framing (debunked by profiling):** "grows `std::vector<float>` per
beam per layer and does O(T²) scalar attention; add a 4D KV cache + flash_attn —
highest-impact optimization." Per-node profiling (`FIRERED_BENCH` per-step +
per-dispatch timers) shows:
- The self-attention KV is **already cached** (`beam.sa_k/sa_v` grow by appending
  only the *current* token's K/V — no history recompute). The scalar scoring loop
  is genuinely O(T²) *cumulative* but negligible for real transcripts (n_hist is
  small; per-step time is **flat vs history length** → NOT attention-bound).
- The decode step is **dispatch-bound**: each step issues ~3741/dec ≈ 90+ tiny
  matvecs (8 projections × 16 layers, greedy M=1 / beam M=n_active), and each
  `ggml_matmat` did a full `ggml_init` + graph build + `sched_reset` +
  `sched_alloc_graph` + `ggml_free`. That per-call overhead — not attention —
  dominates (~45 ms/step baseline on jfk).

**Shipped:** a persistent per-`(weight, M)` matvec graph cache
(`ggml_matmat_cached`) — builds the no_alloc graph once, gallocr-allocates once,
then each call is just `tensor_set → ggml_backend_graph_compute (sched-free on
backend_cpu, native Q4_K SIMD) → tensor_get`. Bit-identical (same op/backend/
weights; jfk + multispeaker byte-identical transcript, all 3 gate states, md5
match). Gated `CRISPASR_FIRERED_MATVEC_CACHE` (default ON; `=0` = old sched path,
kept for A/B + bisection). **Pure Pareto: never slower (strict work-subset).**
A/B (per-call dispatch, `FIRERED_BENCH` timer, min-of-N — contention-robust):
- **quiet box (load ~3.7):** OFF ~0.188 vs ON ~0.178 ms/call → marginal ~5%.
- **loaded box (load 20–50):** OFF up to 22 vs ON up to 20 ms/call; min OFF 1.15
  vs min ON 0.71 → up to ~1.6×. The gap **grows with load** because the removed
  ops (malloc / sched_reset / sched_alloc) are the ones contention penalizes.
  → [[firered-matvec-cache-load-dependent]], the "noisy box fabricates wins" rule.

**Still OPEN (LOW):** the self-attn KV is a growing `std::vector<float>` with a
scalar scoring loop. For very long single-pass decodes (hundreds of tokens) the
O(T²) scoring + realloc churn could start to matter — a pre-allocated 4D device
KV + BLAS/ggml scoring would help *there*, but it is NOT the "highest-impact"
lever for typical clips. Measure on a long clip before investing.

#### §176l Kyutai STT: vectorized RVQ encode

**Status:** MOSTLY DONE 2026-07-12 — optimized encoder proven correct AND kyutai
routing SHIPPED behind `CRISPASR_KYUTAI_RVQ_FAST` (default OFF); only the
end-to-end model code-identity check + default-flip remain (see "Routing SHIPPED"
below).
**Effort:** Small (DRY refactor, done); remaining is a one-clip validation on a model.
**Files:** `src/kyutai_stt.cpp` (still scalar), `src/core/rvq.h` + `rvq.cpp` (the
fast helper), `tests/test-core-rvq.cpp` (new proof).

**The fast search is already written.** `core_rvq::encode_euclidean` (used by
`mimo_tokenizer`) implements exactly the recipe below — the `2·x·E[k] − ‖E[k]‖²`
shootout (argmin over `‖x−E[k]‖²` dropping the per-frame-constant `‖x‖²`), with
pre-computed `‖E[k]‖²`. **Kyutai just doesn't call it** — `rvq_encode_group()`
(`kyutai_stt.cpp:768`, scalar triple loop `:809-828`) still does the naive
`Σ(x−e)²` argmin per (frame × 2048 entries × dim). Same stale-infra pattern as the
rest of the 2026-07-12 audit.

**Proven correct with no model (2026-07-12):** `tests/test-core-rvq.cpp`
(`test-core-rvq`, LABELS unit) compares `encode_euclidean` to a double-precision
full-distance reference across 5 shapes (K up to 512, dim 32, 8 stages) —
**codes identical** (every disagreement certified a genuine <1e-4 near-tie), plus
malformed-input rejection. So the shootout is a correct drop-in for the scalar
argmin; the algorithmic risk is retired.

**Routing SHIPPED (gated, 2026-07-12):** `rvq_encode_group` now has a fast path
(env `CRISPASR_KYUTAI_RVQ_FAST=1`, **default OFF**) that extracts all codebooks to
F32, checks they share `cdim`, and calls the new `core_rvq::encode_euclidean_per_stage`
(‖E[k]‖² precompute + `encode_euclidean` + transpose into `out_codes[q][t]`); any
non-uniform dim or failure falls back to the scalar path. The mechanical
extraction+transpose is unit-tested — `test-core-rvq` now covers
`encode_euclidean_per_stage` vs the scalar reference (identical codes, near-ties
certified), so the only thing NOT verified is end-to-end behaviour on a real
model. **To flip the default:** run kyutai on a Kyutai STT GGUF with the flag on vs
off and assert the emitted RVQ codes are byte-identical (a clip through
`tools/kaggle/kyutai-stt-2.6b-convert`), then measure the speedup. Until then the
scalar path stays default + reference.

#### §176n VoxCPM2: Metal — ALREADY WORKS, VERIFIED (was a stale entry)

**Status:** DONE 2026-07-12 — the premise was stale. VoxCPM2 already runs on Metal
GPU correctly and is a **3.75× win**; the SIGSEGV described here predates the
`VOXCPM2_USE_GRAPH` fused-graph infra that has since shipped. No code fix needed
(only a misleading comment corrected). **CUDA still unvalidated** (Metal-only).

**What was actually true (empirically, on M1):** `crispasr --backend voxcpm2-tts`
already sets `use_gpu` via `should_use_gpu` (as does the session ABI via
`g_open_use_gpu_tls`), so `ctx->backend` IS Metal by default. The heavy pipeline
runs on it: the per-step fused graphs (`build_tslm_step_graph` / `_ralm_` /
`build_locdit_graph`) + VAE encode/decode graphs are gated `VOXCPM2_USE_GRAPH=1`
(default ON) on `ctx->backend` via `ggml_gallocr` + `ggml_backend_tensor_set`. The
old SIGSEGV was about routing the *tiny CPU helper* matmuls (`matmul_mv_ggml`,
raw host pointers) through Metal — which is neither done nor wanted (30 tiny
matvecs/step = launch-bound; the graph path is the win).

**Verification (M1, `voxcpm2-q4_k.gguf`, load ~2.7 — quiet):**
- Basic synth "and so my fellow americans": GPU total **5772 ms** (AR 4392 + VAE
  1005) vs CPU **21647 ms** (AR 8987 + VAE 3537) → **3.75×**. Both round-trip via
  firered-asr to the exact input text (correct, not garbage).
- Voice-clone (`--voice jfk.wav`): runs fully on Metal — VAE-**encode** graph
  (2166 ms) + AR (13069 ms) + VAE-decode (4174 ms), no SIGSEGV / NaN / unsupported
  op. So even the VAE-encode path is Metal-clean.

**CUDA validation — DONE + PASS (2026-07-12, Kaggle P100/T4).** Kernel
`tools/kaggle/voxcpm2-176n-cuda/` (`chr1s4/crispasr-voxcpm2-176n-cuda`) builds on
CUDA, runs CPU vs GPU(default `USE_GRAPH`), and asserts both round-trip through
parakeet ASR (recall ≥0.6), the GPU actually ran (weight mirror, not a silent CPU
fallback), and no crash/NaN/unsupported-op — else it `SystemExit`s. It reached
`COMPLETE` (= verdict PASS), so the **discrete-GPU mirror path is correct on CUDA**;
the LEARNING-35 contiguity risk did NOT materialize. §176n CUDA gap CLOSED. Re-run
after any voxcpm2 change: `kaggle kernels push -p tools/kaggle/voxcpm2-176n-cuda`
(chr1s4; clones `CRISPASR_REF=main`).

**Note:** the lib `default_params`
keeps `use_gpu=false` — that is the **conventional** conservative default (dia,
bark, csm, piper, irodori, tada, chatterbox all do the same); it's overridden by
CLI + session, so all real consumers already get the Metal win. Don't flip it
without the CUDA roundtrip.

## §ARK — ARK-ASR-3B support (⚠️ EXPERIMENTAL / WIP; branch feat/arkasr-3b)

**#253 FIXED 2026-07-12 (drops transcriptions on long audio).** Reporter: a 118 s
LibriSpeech clip produced NO transcription; `--chunk-seconds 10` dropped scattered
windows incl. a stray "p". Two root causes, both reproduced + fixed in
`src/ark_asr.cpp`:
1. **Long single pass degenerates.** The whole-audio pass extrapolates the
   Whisper encoder's RoPE far past its 1500-frame / 30 s training window; the
   decoder then emits `<im_end>` as the FIRST token → empty. Repro: 118 s → empty;
   same clip in 30 s windows → full transcript. **Fix:** cap single-pass at 30 s
   (was 300) and decode longer audio in 30 s windows with the existing cross-chunk
   language conditioning. `CRISPASR_ARKASR_MAX_SINGLE_PASS_S` still overrides.
2. **Windows degenerate to an immediate `<im_end>` (empty/"p").** Some windows
   (esp. short) emit `<im_end>` first → empty for clearly-audible speech; the
   leading-"." cleanup turned a stray `. p` into "p". **Fix:** suppress `<im_end>`
   on the FIRST decode step (Whisper's suppress-EOT-at-start); later steps allow
   it, and a truly-silent window's forced first token is the model's "." which
   cleanup strips back to empty (opt out `CRISPASR_ARKASR_NO_EOS_SUPPRESS=1`).
**Verified on M1 (q8_0):** reporter's exact default command on the 118 s clip now
yields the full correct transcript ("mr quilter is the apostle of the middle
classes…"), 1.7× RT; jfk 11 s unchanged (no spurious words from suppression).
Residual: explicit `--chunk-seconds 10` still isn't ideal (10 s ≪ the 30 s
training window — one slice recovered only "paragraph"), but the default no longer
needs it. `CRISPASR_ARKASR_DEBUG_GEN=1` prints per-window raw gen for future
triage.

**STATUS 2026-06-29**: ⚠️ **experimental / WIP** — wired through CLI, session C
ABI, model registry, and docs; shipped to main + GGUF published
(cstr/ark-asr-3b-GGUF). Core ASR VALIDATED on **both GPU and CPU**: jfk.wav →
verbatim English; De-Abwasch 79 s → verbatim German. CLI: `crispasr -m
ark-asr-3b-q8_0.gguf --backend ark-asr -f audio.wav`. **GPU is now the default**
(Metal-validated; force CPU with `CRISPASR_ARKASR_CPU=1`).

Done: full wiring per docs/contributing.md (C API, adapter, factory, CMake lib+cli,
registry, session ABI + symbols in libcrispasr.dylib + available_backends, live
test, docs). Bindings audited: ASR dispatch is automatic (verified ark-asr in
python Session.available_backends()); added ark-asr to the python set_ask
docstring enumeration; dart/go/rust/java/ruby/wasm need no change (illustrative
lists + generic set_ask docstrings; no new setter). `-l` injection (§9b). GGUFs
published cstr/ark-asr-3b-GGUF
(f16 7 GB / q8_0 4 GB / q4_k 3.3 GB, all verbatim) + HF model card.

**Diff harness RUN** (vs PyTorch bf16 reference, jfk): log-mel cos 0.999993,
first_logits (q8_0) cos 0.999646, audio_embeds mean cos 0.999445 (one low-mag
frame at 0.953 = q8 noise, harmless — logits pass). Pipeline matches the blueprint.

Both perf follow-ups settled by measurement (gated `CRISPASR_ARKASR_TIMING=1`):
- **(a) step-graph cache = DUD** — per-step build+alloc is 0.3–0.5% of each step
  (~0.45 ms vs ~120 ms compute); decode is fully compute-bound on the 3B forward.
- **(b) GPU "no tokens" no longer reproduces** — GPU is verbatim on M1 Metal,
  ~5.6× faster prefill, ~neutral per-token decode, ~1.7× overall. Default. CUDA unvalidated.

**Language drift FIXED** by matching the reference's single-pass whole-audio
(commit: CAP_UNBOUNDED_INPUT + single-pass ark_asr_transcribe). The drift was a
chunking artifact (independent 30 s windows re-detected language); the RoPE encoder
has no positional cap, so the reference encodes the whole clip in one pass. De-Abwasch
79 s now verbatim German throughout. Long audio > `CRISPASR_ARKASR_MAX_SINGLE_PASS_S`
(default 300 s) falls back to internal chunking (drift can return there → use --vad).
Also strip the model's leading `.` transcript-opening token in output cleanup.

Cross-chunk language conditioning DONE: the >cap chunked fallback now seeds each
window's assistant turn with the previous chunk's transcript tail (32 tokens) so
the model continues in the same language instead of re-detecting/translating per
window. Validated: De-Abwasch with forced 30 s chunking
(CRISPASR_ARKASR_MAX_SINGLE_PASS_S=30) → German throughout (chunk 2 previously
translated to English). Opt out with CRISPASR_ARKASR_NO_CHUNK_CONTEXT=1.

Open (minor): CUDA validation (only Metal validated). Nice-to-have, not a blocker.

Port of [AutoArk-AI/ARK-ASR-3B](https://huggingface.co/AutoArk-AI/ARK-ASR-3B):
a 19-language ASR model = **Whisper-large-v3 encoder with partial RoPE** +
**MLP adapter (merge-4)** + **Qwen2.5-3B decoder** with audio-token injection.
On-policy distilled (THUNLP/OPD). BF16 safetensors, 8.14 GB, trust_remote_code.

### Architecture (from config.json + modeling_*.py, mirrored in `.arkasr-ref/`)

**Audio encoder** (`WhisperSpecialEncoder`, modeling_audio.py):
- Standard Whisper conv stem: `conv1` k=3 s=1 p=1, `conv2` k=3 s=2 p=1, both GELU.
  Input 128 mel bins → d_model 1280. 3000 mel frames → 1500 encoder frames.
- 32 pre-norm encoder layers: `self_attn_layer_norm` → attn → +res →
  `final_layer_norm` → fc1(5120) → gelu → fc2 → +res. 20 heads, head_dim 64.
  q/v/out_proj have bias; **k_proj has NO bias** (Whisper convention).
- **Partial interleaved RoPE** applied to Q and K (`WhisperRoPESdpaAttention`):
  `RotaryEmbedding(dim = head_dim//2 = 32)`, base=10000, rope_ratio=1. Rotates
  **only the first 32 of 64 head dims** (dims 32..63 pass through unchanged),
  adjacent-pair (interleaved) rotation = ggml `GGML_ROPE_TYPE_NORMAL` n_dims=32.
  See [[feedback-x-transformers-partial-rope]]. Non-causal, no attention mask.
- **No final encoder layer_norm** — `whisper.layer_norm = nn.Identity()`.

**Adapter** (`AudioMLPAdapter`):
- `layer_norm` = LayerNorm(1280) over encoder output (this replaces the dropped
  encoder LN).
- merge_factor=4: reshape (B,1500,1280) → (B,375,5120) (concat 4 consecutive
  frames). If T%4≠0 truncate to multiple of 4.
- `adapting` = Linear(5120→4096) → GELU → Linear(4096→2048).
- Output: 375 audio embeddings (2048-dim) for a full 30 s clip.

**Decoder**: stock Qwen2.5-3B — hidden 2048, 36 layers, GQA 16 heads / 2 KV
(head_dim 128), intermediate 11008 SwiGLU(silu), RMSNorm eps 1e-6, rope_theta
1e6, vocab 151936, **tied embeddings** (lm_head = embed_tokens). Reuse the
existing Qwen2 graph (cosyvoice3-LM / qwen3_asr / mimo_asr).

**Audio injection**: `audio_token_id` 151663 placeholders in the embedded
prompt are overwritten by the first N adapter frames, where
`N = ((mel_frames+1)//2)//4` computed from the *real* (unpadded) audio length.
Encoder runs on the 30 s-padded mel; the first N merged frames map to real audio
(right-padding), the rest are dropped.

### Prompt / decode recipe (from processing_arkasr.py)
Token stream (add_special_tokens=False, no newlines):
`<|user|>`(151665) `<|begin_of_audio|>`(151666) `<|audio|>`×N(151663)
`<|end_of_audio|>`(151667) `<|assistant|>`(151668)
Greedy decode (HF default greedy; max 256 new tokens), stop at EOS
`<|im_end|>`(151645). bad_words: block all special tokens except EOS. Tokenizer
is Qwen2 GPT-2 byte-level BPE, vocab 151936 + added specials (added_tokens.json).
Mel: WhisperFeatureExtractor — feature_size 128, n_fft 400, hop 160,
sampling_rate 16000, chunk_length 30 (n_samples 480000, nb_max_frames 3000),
dither 0.0. >30 s audio → chunk into 30 s windows.

### Implementation checklist (per docs/contributing.md)
1. [ ] `models/convert-arkasr-to-gguf.py` — lazy safetensors → GGUF F16, embed
       Qwen2 BPE vocab+merges + special tokens, hparams under `arkasr.*` +
       `arkasr.whisper.*`, `general.architecture="arkasr"`. (agent-scaffolded)
2. [ ] `src/ark_asr.{h,cpp}` — C runtime: GGUF load, mel, whisper+RoPE encoder
       graph, adapter, Qwen2 decoder w/ KV-cache greedy decode, BPE detok. (ME)
3. [ ] `examples/cli/crispasr_backend_ark_asr.cpp` — CLI adapter.
4. [ ] Register: `crispasr_backend.cpp` factory + `detect_backend_from_gguf`
       (filename "ark"/"arkasr" + arch "arkasr"); `crispasr_model_registry.cpp`.
5. [ ] `src/CMakeLists.txt` + `examples/cli/CMakeLists.txt` library + link.
6. [ ] `src/crispasr_c_api.cpp` — 9 edit points (session ABI mirrors CLI,
       see [[project-session-abi-reimplements-cli]]).
7. [ ] `examples/crispasr-quantize/main.cpp` — keep norms/bias F32; F16+Q8_0+Q4_K.
8. [ ] Diff harness: `tools/reference_backends/arkasr.py` + register in
       `tools/dump_reference.py` + `crispasr_diff_main.cpp` branch. (agent-scaffold)
9. [ ] `tests/` live test + `tests/env-live-tests.sh`.
10.[ ] README / docs / bindings docstrings.

### Validation methodology
crispasr-diff stage cosines vs PyTorch reference at: mel, encoder (per-layer +
final), adapter, decoder embed-with-audio-injected, per-layer hidden, logits.
Then ASR-roundtrip an English + a German FLEURS clip (verbatim text gate).

### BLOCKER — disk/compute for validation
Both local volumes are ~full (`/Volumes/backups` 7.5 GB free, `/Users` 9.9 GB).
The 8.14 GB safetensors + ~6 GB F16 GGUF do not fit. Scaffolding + graph code is
done on the M1; **download + conversion + diff validation must run on the VPS**
(`/mnt/storage`, `/mnt/akademie_storage`) or after freeing local space.

## llama.cpp comparison — model overlap, support approach, perf-tricks to adopt (ANALYSIS / OPEN)

Comparative study (2026-07-03) of what CrispASR shares with the ggml-org
ecosystem (whisper.cpp + llama.cpp `libmtmd`) and what performance
infrastructure we could adopt. Sources verified against upstream repo files /
PRs where cited; low-confidence items flagged.

### Framing: CrispASR and llama.cpp are *siblings*, not parent/child

Both sit on top of **ggml**. The kernel-level performance machinery
(flash-attn kernels, CUDA MMQ, tensor cores, CPU tinyBLAS/sgemm, Metal
`mul_mm`, Vulkan coopmat, k-quants) lives in **ggml**, which we vendor
in-tree — so we *inherit* those wins automatically **as long as our ggml sync
stays current**. The things llama.cpp genuinely has that we lack are almost all
at the **orchestration layer above ggml** (imatrix tooling, quantize CLI
mixed-precision, speculative decoding, continuous batching). This split
determines what is a "free upgrade" (bump ggml) vs. "real work" (build the
tooling ourselves).

### 4.1 — Model overlap (what both support)

| Model | CrispASR backend | llama.cpp | Type | Notes |
|---|---|---|---|---|
| OpenAI Whisper (tiny→large-v3-turbo) | `whisper` (`src/crispasr.cpp`) | whisper.cpp | ASR | shared whisper.cpp lineage |
| Voxtral Mini 3B | `voxtral` (`src/voxtral.h`) | mtmd | ASR-LLM | we also have `voxtral4b` streaming (llama.cpp only *plans* it, upstream #20914) |
| Qwen3-ASR 0.6B/1.7B | `qwen3` / `mega-asr` | mtmd | ASR-LLM | direct overlap |
| Gemma 4 E2B/E4B (audio) | `gemma4-e2b/e4b` | mtmd (upstream #21421) | ASR-LLM | both use USM/Conformer audio encoder |
| LFM2-Audio | `lfm2-audio` | mtmd (unverified, DeepWiki-listed) | ASR+TTS | verify against `clip.cpp` projector enums |
| OuteTTS 0.2/0.3/1.0 | `outetts` | `tools/tts` | TTS | llama.cpp's *only* TTS pipeline |
| Qwen3-Omni encoder | inside `moss-transcribe` | mtmd (full omni, input-only) | — | we reuse the encoder; llama.cpp runs the whole 30B-A3B MoE |

**llama.cpp has, we don't:** Ultravox v0.5, Qwen2-Audio (supported but "very
poor"), Qwen2.5-Omni / Qwen3-Omni as full models.

**We have, llama.cpp doesn't** (~29 ASR + ~30 TTS architectures with zero
upstream equivalent): Parakeet/NeMo (TDT/RNN-T/CTC), Nemotron streaming,
Canary, Cohere, FireRedASR, Moonshine, MiMo, ARK, MOSS-Transcribe/Audio,
FunASR, Paraformer (NAR+CIF), SenseVoice, wav2vec2/HuBERT/data2vec, Higgs-STT,
GLM-ASR, **Granite-Speech** (llama.cpp explicitly absent), Kyutai/Moshi STT,
mini-omni2, VibeVoice-ASR — plus the entire TTS roster (chatterbox, dots-tts,
tada, qwen3-tts, cosyvoice3, f5, csm, dia, zonos, kokoro, piper, melotts,
voxcpm2, kugelaudio…) and neural diarization. whisper.cpp is Whisper-only;
llama.cpp mtmd is ~8 audio-input models. Our breadth is a different category.

### 4.2 — HOW they support audio (approach difference)

| Dimension | llama.cpp | CrispASR |
|---|---|---|
| Encoder path | one unified `clip.cpp` graph shared by vision+audio; encoder/projector type from mmproj GGUF metadata | bespoke hand-written ggml graph per backend, validated vs Python diff-harness |
| Audio injection | embeddings spliced into the **text KV cache** like image tokens | per-model token injection (ark audio_token 151663, mimo 8-ch RVQ, …) |
| Packaging | two files: `model.gguf` + `mmproj.gguf` | single GGUF; backend auto-detected from `arch` (`crispasr_detect_backend_from_gguf`, `src/crispasr_c_api.cpp:1221`) |
| Long audio | **naive fixed 30 s window + silence-pad** (the design our higgs/ark notes found derails decoders) | **per-model chunking**: parakeet overlap-merge (§208), higgs 4 s, ark single-pass, cohere per-30 s. **We are ahead.** |
| New model | add projector enum + `convert_hf_to_gguf.py --mmproj`, reuse stock LLM decode | full C++ graph port + converter + harness (heavier, but why we cover ~60 architectures) |

Core difference: llama.cpp forces every audio model through the stock LLM
decoder + one clip encoder (so it is limited to Whisper-style-encoder +
standard-LLM combos). We write a bespoke graph per model → we can run
Conformer/SANM/CIF/RNN-T/flow-matching architectures llama.cpp structurally
cannot.

### 4.3 — Performance tricks: we vs. them

Legend: ✅ have · ⚠️ partial/manual · ❌ missing · 🎁 inherited-via-ggml.

| Technique | llama.cpp | CrispASR | Verdict |
|---|---|---|---|
| Flash attention (`ggml_flash_attn_ext`) | ✅ default-on, tri-state `-fa on/off/auto` | ✅ per-backend, gated (`src/canary.cpp:886`, m2m100, nemotron, moonshine, lfm2, …) | 🎁 same op; adopt default-on discipline |
| Quantized KV cache | ✅ symmetric K/V → fused FA path | ✅ `CRISPASR_KV_QUANT_K/V`, `KV_ON_CPU` | ⚠️ keep K/V **symmetric** or drop off fast path |
| k-quants (Q2_K–Q6_K) | ✅ | ✅ ship Q4_K/Q8_0/F16 (+ scattered Q5_K/Q6_K/Q3_K/Q2_K) | 🎁 parity |
| CPU SGEMM (tinyBLAS/llamafile) | ✅ | 🎁 inherited via vendored ggml-cpu | 🎁 free — confirm compiled in |
| MMQ / tensor cores / CUDA FA kernels | ✅ | 🎁 inherited via ggml-cuda | 🎁 free (keep ggml current) |
| Metal `mul_mm` / residency sets / Metal FA (#12612) | ✅ | 🎁 inherited via ggml-metal | 🎁 free — biggest M1 encoder-prefill lever |
| Vulkan coopmat | ✅ | 🎁 inherited (coopmat2 absent on MoltenVK/RX580 anyway) | 🎁 parity |
| **imatrix (importance-matrix quant)** | ✅ `llama-imatrix` | ❌ **not in our converters** (registry: 0 imatrix) | **GAP — low-risk quality lever** |
| **i-quants (IQ1..IQ4_NL)** | ✅ | ❌ (registry: 0 IQ types) | GAP — but low-bit breaks audio backbones (kokoro finding); low priority |
| **Per-tensor mixed-precision quant** | ✅ `--tensor-type regex=type` CLI | ⚠️ done by hand in converters (keep embd/head/adapter high) | systematize it |
| **Speculative decoding** (draft/EAGLE-3/n-gram) | ✅ | ❌ (our "lookahead" hits are DSP convs, not spec-decode) | GAP — conditional value for AR ASR/TTS heads |
| **Continuous batching / server slots** | ✅ `--parallel N`, ~3× | ❌ single-stream bespoke decode | GAP for multi-request serving |
| CUDA graphs | ✅ ~14% | ⚠️ we graph-*fold* manually (qwen3-tts §161) | partial, CUDA-only |
| Operator fusion (`ggml_can_fuse`) | ✅ runtime RMSNorm+MUL, GEMV+gated, TopK-MoE | ⚠️ manual graph construction + gallocr bypass | 🎁 fusion is in ggml; we get it when we don't bypass sched |
| Compute-graph reuse | ✅ `-gr` generic | ✅ per-backend cache (§176s) + raw gallocr | ⚠️ parity, bespoke |
| Per-model long-audio chunking | ❌ naive 30 s | ✅ overlap-merge / single-pass | **we're ahead** |
| Batched CFG for diffusion TTS | ❌ (OuteTTS token-only) | ✅ cosyvoice3 / chatterbox B=2 | **we're ahead** |
| Neural diarization / voice cloning | ❌ (tinydiarize = channel split) | ✅ TitaNet / pyannote / CAMPPlus | **we're ahead** |

### 4.4 — Adoption roadmap (ranked)

**Tier 1 — real gaps, genuine ROI:**

1. **imatrix + systematic per-tensor quant overrides.** ~~Our converters ship
   Q4_K/Q8_0/F16 with hand-chosen high-precision embeddings/head/adapter —
   llama.cpp does the *same intent* but calibrated.~~ **DONE (imatrix, both
   sides).** Ported CrispEmbed's tool: `crispasr-quantize` now takes
   `--imatrix <file>` (activation-weighted k-quant/IQ error) + `iq4_nl/iq4_xs`
   types + accepts an already-quantized source (requant from q8_0, no F16 base
   needed). The **producer** is `src/crispasr_imatrix.{h,cpp}`: set
   `CRISPASR_IMATRIX_OUT` and any instrumented ASR backend accumulates
   per-column activation sum-of-squares over a calibration run (merges across
   runs), emitting the imatrix GGUF. Installed on the decode scheduler of
   whisper, parakeet, canary, cohere, qwen3-asr/mega-asr, higgs-stt, ark-asr,
   moss-transcribe, granite(+nle), glm-asr, mimo-asr, voxtral(+4b). Validated
   end-to-end on mega-asr: 2 calib runs → 141 tensors → 113 decoder tensors
   imatrix-weighted at quantize → JFK verbatim. See `docs/quantize.md`.
   **A/B harness `tools/imatrix_ab.py`** (prefill-logit cosine vs f16 gold, via
   the `CRISPASR_ACTDUMP_OUT` tensor-dump in the observer module) proved the
   quality win — but ONLY with a good corpus:
   - qwen3-asr-0.6b q4_k, **CC0 Common Voice EN+DE** (12+12 calib / 3+3 held
     out): mean prefill-logit cos **0.890 → 0.941 (+0.051)**, every clip up,
     German gained most (+0.087). Zero size change (540 MB either way).
   - Same model, **LibriSpeech-EN-only** (20 calib): **−0.013 REGRESSION**;
     EN+ZH 2-clip: −0.009. So corpus **diversity + in-distribution + language
     coverage** is decisive — a narrow/mismatched corpus makes imatrix *worse*.
   **Community note (§llama.cpp):** bartowski/mradermacher imatrix their
   audio-LLM GGUFs (Voxtral, Qwen2-Audio) with a *text* corpus
   (`calibration_datav3`) calibrating only the LLM decoder — the Whisper
   encoder is never audio-calibrated. Our producer runs real audio, so it also
   covers the audio-conditioned decoder activations. No off-the-shelf ASR-audio
   imatrix corpus exists; **CC0 Common Voice (EN/DE/…)** is the clean-license
   build source (LibriSpeech/FLEURS are CC-BY).
   **DONE (per-tensor override)**: `--tensor-type <regex>=<type>` (repeatable,
   first-match-wins, partial regex like llama.cpp) overrides the arch guards;
   value may be f16/f32/any quant with row-width fallback. `crispasr-quantize`
   now has full llama.cpp-parity quant knobs. **A/B harness** also gained a
   transcript-CER signal (alongside logit cosine): on qwen3-asr-0.6b q4_k the
   EN+DE win holds (cos +0.05, CER 0.0 = no transcript regression). Calibration
   set published CC0 at `cstr/crispasr-imatrix-calib` (+ `tools/imatrix-calib/`).
   **Metric finding (q3_k):** the two A/B signals DIVERGE at aggressive
   bit-widths — imatrix improved q3_k transcript CER **0.37→0.13** (6/12 clips)
   while the prefill-logit **cosine dipped −0.04**. So CER (transcript fidelity)
   is the real gate; the single first-token-logit cosine is a proxy that can
   mislead. Harness verdict now gates on CER (cosine tiebreaks easy quants).
   Lesson: imatrix's payoff grows as bits shrink — biggest at q3_k/iq3.
   **FLEET RUN (Kaggle `chr1str/crispasr-imatrix-quant`, CUDA, ~105 min):**
   calibrated+quantized q4_k & q3_k imatrix for 5 ASR-LLM decoders on the CC0
   corpus, A/B CER vs f16, uploaded to each HF repo. **imatrix improved CER on
   8/10, tied 2/10 (ark-3b), 0 regressions:**
   - qwen3-asr-0.6b: q4_k 0.008→0.000, q3_k 0.664→0.623
   - **mega-asr-1.7b: q4_k 0.132→0.010 (−0.122!), q3_k 0.389→0.132 (−0.257)** ← headline
   - moss-transcribe-2b: q4_k 0.150→0.102, q3_k 0.259→0.170
   - higgs-stt: q4_k 0.050→0.014, q3_k 0.010→0.003
   - ark-asr-3b: q4_k/q3_k tied (heavy F16 guards → little quantized)
   Also VALIDATES the imatrix producer on **CUDA** (was open). Kernel at
   `tools/kaggle/imatrix-quant/`; safety gates (empty imatrix→skip, CER>0.8→no
   upload) guard against a bad producer. Log at /Volumes/backups/code/kaggle-out.
   Still **OPEN**: wiring the collector into the remaining (non-ASR-decoder)
   backends if we ever want imatrix there too.
2. **FA default-on audit.** Upstream flipped flash-attn to baseline. Re-check
   our two known FA bugs — batched-FA corruption (§176h) and the Vulkan
   GQA-REPEAT-f16 path — against the *current* `FLASH_ATTN_EXT` in vendored
   ggml; some may already be fixed upstream.
3. **Keep ggml sync current — the kernel wins are free there.** Metal `mul_mm`
   (M1 encoder prefill), Metal FA with head_size_k≠head_size_v (#12612 —
   matters for our partial-RoPE ASR encoders: ark rot32/hd64, higgs, parakeet),
   CPU RMSNorm+MUL fusion (2026, +43–68% on M2), MMQ, tinyBLAS. None require
   llama.cpp; verify our pinned ggml is recent enough to include them.

**Tier 2 — conditional on serving story:**

4. **Symmetric quantized-KV (q8_0)** for the Qwen-family audio-LLM decoders
   (ark, higgs-stt, moss-transcribe). Near-lossless, shrinks decoder KV. Must
   be symmetric K==V type or it silently falls off the fused FA path. Validate
   against the per-head KV-stride bugs (#171) first.
5. **Continuous batching** if the server ever needs concurrent transcription.
   Today our bespoke single-stream decode paths can't multi-slot, and #171
   shows per-slot KV isolation is a real hazard for audio decoders.
6. **Prompt-lookup / n-gram speculative decoding** for AR ASR heads
   (transcripts echo their own context → high acceptance). Heed the §161
   lesson: the draft must reproduce the target's *exact realization* or
   generation derails.

**Don't bother:** i-quants (low-bit breaks audio backbones; codebook decode is
slow on compute-bound TTS/ASR), KV defrag (deprecated upstream; single-pass
audio doesn't fragment), paged attention (not merged upstream).

**Where we're better and should NOT converge to them:** model breadth (~60 vs
~8 architectures), per-model long-audio chunking (their fixed-30 s is worse
than our overlap-merge), diffusion/flow TTS with batched CFG, neural
diarization, voice cloning, single-file arch-autodetect UX.

### Verify-in-tree caveats (from the upstream survey)
- Exact `ggml_rope_ext` arg order / `GGML_ROPE_TYPE_*` enum values against the
  pinned `ggml/include/ggml.h` (signature has been revised upstream).
- Current speculative-decode flag namespace (`--spec-*` vs older
  `--model-draft/--draft-max`) if we ever mirror it.
- mtmd mel preprocessing params (128 HTK bins) + LFM2-Audio/MiniCPM-o audio
  status are DeepWiki-sourced, not quoted from `clip.cpp` — confirm if
  load-bearing.

---

## 221. Issue #89 hardening + v0.8.8 release

Follow-through on the 2026-07-04 #89 close-out (VAD slice cap + per-slice
single-pass + gap-fill; see HISTORY + LEARNINGS). The fix is shipped and
verified manually; this section makes it durable and gets it released.

### 221a. CI regression guard for the JA long-form path — HIGH, small

The shipped 97/97/96 % coverage is protected only by manual runs; the next
parakeet refactor could silently re-break #89. The YouTube reproducers can't
be committed, but the reazon baseball fixture
(`cstr/crispasr-regression-fixtures` →
`parakeet-tdt-0.6b-ja/reazon_baseball_14s/audio.wav`) can drive a live test:
concatenate ×3 (42.2 s — crosses the 30 s auto-chunk threshold and the 12 s
slice cap), transcribe via the **session ABI** (the library-level entry that
carries the cap + gap-fill), assert (a) 岡本 appears ≥3×, (b) last timestamp
reaches the third repetition, (c) a byte floor. Wire into `tests/` +
`tests/env-live-tests.sh` (`CRISPASR_MODEL_PARAKEET_JA`,
`CRISPASR_FIXTURE_PARAKEET_JA`).

### 221b. HTTP server path audit + mirror — HIGH, small-medium

`crispasr_server.cpp` has its own slice loop (`crispasr_compute_audio_slices`
at ~line 410) separate from both the CLI dispatcher and the session ABI.
Audit whether a JA parakeet request through `/v1/audio/transcriptions` gets
the slice cap + gap-fill; if not (expected), mirror the same policy —
either by consulting the backend's `vad_slice_cap_seconds()` like
`crispasr_run.cpp` does, or by routing the server's parakeet handling
through the session-ABI code path.

### 221c. Vulkan sanity run — MEDIUM, small

The #89 reporter is on AMD/Vulkan where the encoder instability was
originally worse. The fix is policy-level (slicing) so it should transfer,
but confirm once via MoltenVK on the M1 (`GGML_VULKAN=ON` build,
`VK_ICD_FILENAMES` gotcha — see LEARNINGS/reference notes): yt_60s default
run should land ≈97 % recall like Metal.

### 221d. q4_k registry/UX guard — MEDIUM, small

parakeet-ja q4_k TDT decode is degenerate (repetition loop; pre-existing,
same in the old upload) while CTC decode over the same file is clean.
Two guards: (1) check what `-m auto --model-name parakeet-ja` resolves to in
`src/crispasr_model_registry.cpp` — if q4_k, point it at the q8_0 (TDT
byte-identical to F16); (2) stderr hint when a JA parakeet GGUF with
quantized (≤q4) weights loads in TDT mode: suggest `--parakeet-decoder ctc`
or the q8_0 file.

### 221e. v0.8.8 release — after a-d

Everything since v0.8.7: the #89 series (slice cap, gap-fill, session
mirror, tools, CTC-head GGUFs), #218 moss fixes, #217 --align-only docs.
Follow the release process in the dev guide: notes from
`git log v0.8.7..HEAD --oneline --no-merges` into RELEASE_NOTES_v0.8.8.md,
`scripts/bump-version.sh 0.8.8`, push main, wait for green CI, push tag,
`gh release create` with notes, then remove the repo-root notes file.

## §222 Aligner model expansion — permissive-license fleet (OPEN)

Motivated by #217 follow-ups. Constraint: **non-NC licenses only** (the
popular MMS-based aligners, e.g. `MahmoudAshraf/mms-300m-1130-forced-aligner`,
are CC-BY-NC-4.0 → excluded; verified 2026-07-04 via the HF API).

**Shipped so far (2026-07-04):**
- `--align-only` segment mode: `.srt` cue-preserving re-timing +
  `--align-granularity auto|word|segment` (feat/#217, `0149262a`).
- FastConformer-CTC standalones confirmed working as aligners as-is (GGUF
  arch `canary-ctc` → default dispatch); registry aliases
  `fastconformer-aligner[-en]` added. At ~83 MB q4_k this is the
  smallest/fastest aligner we ship.
- Hybrid CTC-branch extraction: `convert-stt-fastconformer-ctc-to-gguf.py`
  now also accepts `EncDecHybridRNNTCTCBPEModel` checkpoints
  (`ctc_decoder.*` head, RNNT `decoder.*`/`joint.*` skipped, `xscaling`
  honoured from YAML). First fleet member converted + validated + uploaded:
  `cstr/stt-de-fastconformer-hybrid-ctc-large-GGUF` (de, punct+caps,
  ASR transcript exact on FLEURS-de sample; word alignment cross-checked
  against canary-ctc-aligner, agreement ≤0.2 s once speech starts).
  Registry: `fastconformer-aligner-de` / `fastconformer-ctc-de`.

**Fleet SHIPPED (2026-07-04):** all 16 remaining members converted,
FLEURS-validated (per-language test clip vs reference transcript, plus an
--align-only smoke test) and uploaded: en-pc, es, fr, it, nl, pl, ru, ua,
hr, be, ar, fa, ka, hy, uz, kk-ru → `cstr/stt-<lang>-fastconformer-hybrid-
ctc-large-GGUF`, registry aliases `fastconformer-{ctc,aligner}-<lang>`.
Notes from the batch:
- **pt is excluded**: `stt_pt_fastconformer_hybrid_large_pc` is
  CC-BY-NC-4.0 — the only NC release in the fleet (checked per-repo).
- **kk-ru needed a runtime fix**: it is the one model with
  `conv_norm_type: layer_norm` (v2.0.0 recipe). NeMo names the module
  `batch_norm` either way, so conversion succeeded but the runtime's BN
  fold silently skipped (no running stats) → garbage ASR. Fixed by a
  `canary_ctc.conv_norm_layer` GGUF flag + in-graph `ggml_norm_affine`
  after the depthwise conv (core/fastconformer.h, nullable — BN models
  unaffected, verified bit-identical timings on the canary aligner).
- **fa** (non-pc, older release) transcribes with visibly more character
  noise than the pc models — usable, but the weakest of the fleet.
- Validation gate hardened lesson: "ASR output non-empty" let broken
  kk-ru ship briefly; transcripts must be eyeballed against the FLEURS
  reference (they were, which is how it was caught same-day).

**Also open:**
- parakeet-tdt-0.6b-ja CTC head upload (#89 leftover) → Japanese
  FastConformer aligner (better than wav2vec2-aligner-ja).
- Known artifact: FastConformer hybrid CTC branches glue the FIRST word's
  start to 0.0 across leading silence (blank-absorption at Viterbi start);
  canary-ctc-aligner places it correctly. If it bothers users, clamp the
  first word's t0 to (first non-blank frame − margin) in the shared CTC
  Viterbi.
- OWSM-CTC (CC-BY-4.0, ESPnet E-Branchformer, 151 languages): would need a
  new encoder runtime — only worth it if the per-language fleet above
  proves insufficient.

## §223 Issue #220 — chatterbox T3 CUDA illegal memory access (FIXED, sm_80+ verify pending)

Reporter (RTX 3090 Ti, `crispasr:main-cuda` v0.8.8): every chatterbox request
aborts `CUDA error: an illegal memory access was encountered` at AR step ~2,
right after `CUDA Graph id N reused`. CPU works.

- **Root cause:** §186 Lk-bucketed T3 decode allocates its step sched once per
  bucket then reuses it (skips per-step reset+alloc). ggml-cuda replays a
  captured CUDA-graph exec on a split-graph `uid` match with capture-time
  pointers baked in; the reuse never re-mints the uid → step 2 replays a stale
  capture → illegal access. CUDA twin of Vulkan #170; same class as qwen3-tts
  #52/#56. Capture is arch-gated to sm_80+, so only Ampere+ cards are affected.
- **Fix (SHIPPED, commit c78b187b, branch fix/chatterbox-t3-cuda-illegal-access):**
  reset+alloc the bucket step sched every step on non-Metal GPU (granite/outetts
  pattern, docs/contributing.md §210). Cached cgraph keeps `nodes[0]` stable so
  capture still engages (new uid → `cudaGraphExecUpdate`); T3 stays on GPU.
  Metal keeps alloc-once reuse. Old path A/B via
  `CRISPASR_CHATTERBOX_T3_BUCKET_REUSE=1`.
- **Verification (DONE, on real CUDA):** Kaggle A/B kernel
  `tools/kaggle/issue220-chatterbox-cuda/` (old_reuse vs fix_default vs cpu_ref,
  ASR-roundtripped) on a **P100 (sm_60)**:
  - `old_reuse` → rc=-6 CRASH, `illegal memory access` at
    `ggml_backend_cuda_synchronize` (same signature as the report), no WAV.
  - `fix_default` → rc=0, valid WAV, ASR = exact input text.
  - `cpu_ref` → rc=0, same exact ASR.
  So the crash is **capture-independent** (sm_60 has no CUDA-graph capture) — the
  reuse-shortcut is unsafe on CUDA regardless of arch; sm_80+ capture is only an
  extra aggravator. Fix confirmed on hardware. Both Metal + forced-CUDA branches
  also compile clean. (Nice-to-have: an sm_80+ run to also exercise the
  capture-replay path, but the fix is already proven end-to-end.)

## §224 Issue #222 follow-ups — silero LID ggml graph (DONE) + diarize/firered CPU perf (OPEN)

Issue #222 (silero LID Vulkan segfault) fixed in `222b6781` (weights were
GPU-loaded but the forward dereferenced `tensor->data`). Follow-up shipped in
`a5b780f8`: the silero LID forward is now a ggml graph — CPU frontend
(log-magnitude precision) + sched encoder (GPU on Metal/CUDA), 30 s slice cap
(O(T²) attention; uncapped 10-min file = ~19 GB score alloc). Legacy scalar
path gated `CRISPASR_SILERO_LID_LEGACY=1`. M1: 103 ms Metal / 183 ms CPU-ggml
vs 241 ms Accelerate / 1014 ms scalar. A/B logit-matched on en/zh/multispeaker.

**Vulkan (OPEN):** ggml-vulkan miscomputes one FFN MUL_MAT (f32 128×128×1101)
in this graph on MoltenVK — allocation-layout dependent, identical-shape
matmuls pass and fail in the same graph (TADA #192 class). Found with
`-DGGML_VULKAN_CHECK_RESULTS=ON` (vendored CMakeLists now links ggml-cpu for
that option). LID graph auto-routes to CPU on Vulkan;
`CRISPASR_SILERO_LID_VULKAN=1` opts back in. TODO: verify on conformant RADV
(reporter has RX 9070 XT) — may be MoltenVK-only; if it reproduces, minimal
upstream repro.

**Diarization CPU perf (DONE, 3340054f):** both models ported to ggml graphs,
legacy paths gated as A/B ground truth. pyannote_seg 4.38 s → 0.55 s (31.5 s
audio, M1), frame-identical (CRISPASR_PYANNOTE_LEGACY=1). TitaNet embeddings
cos=1.000000; inverse-default: without Accelerate the scalar path measured
**106–131 s per embedding** (the actual #222 "extremely slow" cause on Linux)
→ 3.4 s ggml (~35×); with Accelerate the legacy AMX path (0.7 s) stays default
(CRISPASR_TITANET_GGML=1 / CRISPASR_TITANET_LEGACY=1). OPEN: batch segments;
F16 weights to close the ggml-vs-AMX gap on Apple.

**FireRed ASR perf (PARTLY DONE, d6e2ad85):** measured split on 11 s jfk (M1):
fbank 0.08 s + subsample 0.16 s + **encoder 11.3 s** + decoder ~0.5 s/step
(beam=3, ~16 s) = 0.3× RT. Root cause of the CPU-only encoder: it relied on
the sched auto-copying CPU weights to GPU, which ggml removed → silent
regression. Fix: CRISPASR_FIRERED_ENC_GPU=1 split-loads enc.* to GPU —
transcript-identical, encoder 2.3× on Metal, 2.1× on Vulkan/MoltenVK. CUDA A/B
PASSED on Kaggle P100 (transcript-identical, enc 12.93→5.88 s, 2.2×) —
default FLIPPED (ffa4afa0); CRISPASR_FIRERED_ENC_CPU=1 opts out. Decoder beam path DONE (bc8a599a):
the beam loop was dequantizing all decoder weights to F32 and running
scalar dots (8× the bytes of greedy's Q4_K kernels); now batched through
ggml_matmat on the quantized weights — beam=3 16.8 s → 3.1 s (~70 ms/step,
≈1.5× greedy as expected), transcript-identical en+zh; F32 fallback gated
CRISPASR_FIRERED_BEAM_F32=1. Full 11 s-jfk pipeline: 36 s → ~8.5 s (M1).

**§224e disease sweep (2026-07-06)** — audited all runtimes for the three
disease classes cured this week (scalar forward / F32-dequant matmul / silent
CPU-pinning after ggml removed sched auto-copy of CPU weights):
- **ecapa_lid — CURED (e20599ee):** ASP+FC head was scalar on non-Apple
  (~3.0 s of a 4.5 s detect; ecapa is the recommended --lid-backend). Head now
  in-graph (titanet ASP recipe), inverse-default (Accelerate GEMM head, 57 ms,
  stays default on Apple). Verified en/zh identical on Metal + Vulkan.
  Remaining: trunk graph ~1.3 s on M1 — profile Metal residency.
- **mimo_tokenizer — CURED (4027f37c):** weights now load on the compute
  backend (all reads are tensor_get / host-cached RVQ codebooks → full GPU
  residency safe). Smoke 22.4 s/71.7 s-user → 15.9 s/1.5 s-user; full
  mimo-asr pipeline 10.3 → 7.6 s, transcripts char-identical en+zh.
  CRISPASR_MIMO_TOK_CPU=1 restores CPU weights.
- **pocket_tts — CURED differently (63ae5a43):** the actual hotspot was NOT
  the backbone graphs but the eager mimi ENCODER re-run per invocation for
  voice conditioning: SEANet scalar convs 27.5 s + per-timestep transformer
  12.1 s. conv1d_ggml drop-in (identical layout contract) + batched
  transformer linears via mul_mat on the F16 weights (attention math
  untouched) → synthesis 43 → 5.6 s. Conditioning latents cos=0.99999994 vs
  scalar; TTS→ASR roundtrip verbatim. CRISPASR_POCKET_MIMI_SCALAR=1 =
  ground truth; POCKET_MIMI_DUMP=<path> dumps latents. The GPU-residency
  split for the backbone remains possible but is no longer the bottleneck.
  OPEN idea: cache conditioning latents keyed by voice-file hash (17 KB) to
  skip re-encoding entirely on repeat runs.
- **openvoice2 — OPEN (disease 1):** WaveNet (16 layers, bulk of voice
  conversion) is hand-rolled conv, Accelerate on Apple / scalar elsewhere
  (§176d note). Titanet-class cure; opt-in feature, measure first.
- **firered_vad — OPEN (disease 1, low):** 8 DFSMN blocks scalar; small
  model, opt-in --vad backend.
- **chatterbox_campplus — OPEN (disease 1, low):** x-vector embed scalar;
  once per voice-clone synthesis.
- Healthy: mimo_asr (proper split-load with embed carve-out), fireredpunc,
  ecapa trunk, firered_vad/titanet weight READS (tensor_get, device-safe),
  lid_cld3/lid_fasttext (tiny text models).

## §225 glint encoder in-tree — TTS/S2S MP3 + AAC output everywhere (DONE)

Integrated our clean-room MP3 + AAC-LC encoder (sibling `glint` repo) as an
in-tree copy at `glint/` (encoder core only, ~25 files; same
develop-there/sync-here relationship as `ggml/`). Static lib, linked into `crispasr-cli`; SIMD
kernels compile-time guarded + runtime dispatched, so no special flags.

- `examples/cli/crispasr_mp3_writer.h` — header-only `crispasr_make_mp3()`:
  mono CBR 128 kbps, `GLINT_QUALITY_NORMAL`, ID3v2 AI-provenance tag
  prepended (`crispasr_make_id3v2_ai_tag`), non-MP3 rates linearly
  resampled to nearest native rate (all TTS backends emit native rates).
- Server `response_format=mp3` no longer needs libmp3lame — the 400
  `codec_not_available` path is gone; both /v1/audio/speech and S2S routes
  use the shared helper.
- CLI: `--tts-output out.mp3` / `--s2s-output out.mp3` dispatch on
  extension (`crispasr_write_synth_audio`); C2PA signing stays WAV-only
  (warns on .mp3 instead of silently dropping the manifest).
- libmp3lame kept as optional fallback: auto-used if glint fails, forced
  via `CRISPASR_MP3_ENCODER=lame` (A/B); forced-lame failure falls back to
  glint.
- Verified: unit tests (`test_tts_provenance` `[mp3]` — ID3 prefix, frame
  sync, CBR size, resample, invalid input), ffmpeg decode roundtrip of a
  440 Hz sine (peak 440.0 Hz, RMS/duration exact), live pocket-tts →
  `.mp3` → moonshine ASR roundtrip returns the input text verbatim, and
  forced-lame path.
- **AAC-LC (experimental)** also in: upstream's phase-1 AAC encoder
  (long blocks, CBR-average, ADTS) shipped while this landed, so the
  in-tree copy carries it — `response_format=aac` (audio/aac) and
  `--tts-output out.aac` via `crispasr_aac_writer.h` (mono, 96 kbps
  default, ID3v2 provenance tag prefixed — decoders skip it). Verified:
  ADTS syncword unit tests, ffmpeg decode (LC profile, 440 Hz exact),
  live pocket-tts → .aac → ffmpeg-decode → moonshine ASR verbatim.
  Note: crispasr's *reader* can't ingest .aac without the libav
  transcode build (pre-existing; plain ffmpeg .aac fails identically).
- Streaming TTS still rejects mp3/aac (full-file encoding); glint has a
  callback streaming API — chunked `audio/mpeg` streaming is a natural
  follow-up if wanted.
- **Auto-sync (§225b):** `tools/sync-glint.sh` + `sync-glint` workflow
  (repository_dispatch `glint-push` from glint's notify-crispasr
  workflow, daily cron fallback, manual dispatch) keep `glint/` at
  upstream main's committed state — regenerates GLINT_SOURCES, bumps
  the README sync marker, gates on `test_tts_provenance "[mp3],[aac]"`
  before pushing. `validate-glint-fresh` in release.yml fails a release
  whose marker is behind glint main, so releases always ship current
  glint. Instant dispatch needs a `CRISPASR_SYNC_PAT` secret in the
  glint repo (Contents r/w on CrispASR); without it the daily cron
  covers it. First sync pulled the AAC psy-shaping upstream commit
  (30fb4fcd, aac_psy.cpp auto-added to the source list).

## §226 irodori-tts GPU + codec GGUF fix (DONE 864ebe2e / HF refresh)

User-requested GPU enablement for the Irodori-TTS RF-DiT backend (all stages
were already single-backend gallocr ggml graphs, but the backend hardcoded
CPU). Now honors use_gpu (CLI adapter wired; session ABI already passed it);
codec follows the compute backend except on Vulkan (CPU per TADA #192);
CRISPASR_IRODORI_CPU / _CODEC_GPU / _CODEC_CPU override.

Blocking baseline bug found+fixed on the way: the published
dacvae-ja-32dim-f16.gguf predated the converter's wm_model final-conv export
— missing pre.1.{weight,bias} (Conv 96→1) made the C++ decoder skip the
final conv and die at GGML_ASSERT(nelements==ne0) on the 1-D reshape.
Re-converted (converter on main was already correct) and re-uploaded to
cstr/irodori-tts-GGUF, so `-m auto` users get the fix without a rebuild.

M1 verify (seed 42, JA): CPU↔Metal waveform cos=0.999389, same RMS/ASR
reading; 73.4 s → 31.4 s per 2 s synthesis (2.3×; CPU idle on GPU run).

OPEN (upstream/parallel session): baseline generation QUALITY is still WIP —
both CPU and GPU produce the same garbled JA speech (parity holds; the
divergence is upstream of the backend split, likely DiT/ODE math vs the
Python reference). Perf follow-up: ~120 DiT graph evals (40 ODE steps ×
CFG) rebuild graph+gallocr per eval — cacheable, but mind the #215 cached-
cgraph UAF pattern.

## §227 starling comparison — CUDA decode-loop optimization options (ANALYSIS / OPEN)

Context: sims1253/starling (CUDA-graph inference for ASR, RTX 5090, bf16)
benchmarks CrispASR as an external engine and overlaps our model fleet
(granite-speech, parakeet-tdt-v3, MOSS-Transcribe, qwen3-asr, ARK-ASR-3B,
higgs-audio-v3-stt, cohere-transcribe — several via OUR cstr/* GGUF
conversions). Repo cloned to ~/code/starling; read 2026-07-06. Their claim:
stock transformers decode is launch-bound (GPU ~10% busy) → capturing decode
steps + multi-step token loops into CUDA graphs gives 27–1180× vs 3–66×
stock, byte-identical output, WER-verified on Open ASR Leaderboard repro.

### Benchmark-fairness action (cheap, reputational)

Their CrispASR adapter (benchmarks/engines.py:722, scripts/
bench_qwen3_crispasr.py) times ONE FULL CLI SUBPROCESS PER CLIP — including
process start + multi-GB F16 GGUF disk load + CUDA weight upload on EVERY
rep — while starling/stock numbers exclude model load and use warm reps.
Cold-start seconds get labeled as engine speed on short clips. No crispasr
numbers are published in their repo yet (engine silently skipped without the
author-local ~/asr-bench install), so the columns could appear any time.
- [ ] Contact author: benchmark `crispasr-server` (resident model — matches
      their own server mode) or parse our stderr phase timings; offer setup
      help. Alternatively contribute a resident-mode adapter PR to starling.
- [ ] Consider a documented "benchmarking CrispASR" recipe in docs/ (server
      mode, warm reps, phase-timing flags) so third-party benchers measure
      transcribe time, not cold start.

### Optimization options extracted (ordered by evidence)

0. **GATE EVERYTHING on a decode-utilization profile.** One Kaggle T4/P100
   run: GPU-busy% during granite/qwen3-asr AR decode (nvidia-smi dmon or
   nsys). If ggml decode is already 60%+ busy, options 2–4 are duds like the
   CPU/Metal analogs (§210 Metal ICB ceiling 1.8%; vibevoice CPU cache A/B
   0%, byte-identical). If ~10–20% busy (starling's stock baseline), proceed.
1. **Keep per-step graphs capture-friendly (mostly done, free).** ggml-cuda
   already captures stable per-step graphs into CUDA graphs. Capture keys on
   the split graph — alternating different graphs on one sched (and shape
   churn like the "graph has different number of nodes → reserving" respam)
   defeats both the gallocr AND capture. The #171 per-purpose dedicated
   scheds (pred head, per-KV-path LM steps) already improved this; audit
   other AR backends for the same pattern where a decode loop shares a sched
   with helper graphs (EOS classifiers, connectors).
2. **Multi-step token-loop unroll (the real starling edge; CUDA-only).**
   Unroll K greedy decode steps into ONE graph: in-graph GGML_OP_ARGMAX →
   get_rows embedding feedback → static per-step KV positions; host sync
   drops from per-token to per-K-tokens; EOS checked per block (≤K−1 wasted
   steps); byte-exact for greedy. Topology is stable per (n_past bucket, K)
   → single capture replay per block. Substantial engineering and EXACTLY
   the cached-graph minefield of #171/#184/#220 — invariant applies: one
   graph per sched, or last-allocated-only. Do not start before option 0
   numbers exist.
3. **Self-speculative draft from CTC head (granite only).** starling drafts
   from granite's encoder CTC head and verifies with the LLM — no extra
   model. We already have granite CTC infrastructure. Their caveat: batched
   spec at B≥16 LOSES (0.76×, lock-step rewind waste); B=1 spec is the win.
4. **Fused RMSNorm/SwiGLU steps** — ggml-cuda already fuses some of this;
   only worth auditing if option 0 shows launch-bound decode with capture ON.

### Anti-options (their negative results transfer or confirm ours)

- INT8 weight-only quant on CUDA decode: SLOWER for them (launch-bound, not
  bandwidth-bound) — matches our ONNX-int8-10×-slower-on-CUDA-EP datapoint.
  Quant remains a CPU/Metal bandwidth win, not a CUDA decode win.
- Graph caching per shape without eviction: their parakeet/ark RTFx is
  DEPRESSED by per-clip capture cost at high shape diversity (their own
  footnote) — shape-bucketed graph caches hurt there too; mirrors our
  vibevoice/chatterbox cache-A/B duds.
- torch.compile-style encoder fusion: not byte-exact for them (fp32 upcast +
  BatchNorm amplification) — reinforces our decoded-output A/B mandate.


## §228 Issue #221 irodori-tts follow-ups — cloning, emoji, caching, streaming (bakamomi)

**DONE (2026-07-07).**

All on main: emoji emotion controls via SentencePiece byte_fallback
(byte-exact vs HF llm-jp-3), DAC-VAE-encoder voice cloning wired
(spk-sim 0.65 vs 0.70 reference ceiling) + speaker CFG, duration
predictor replacing the frames/token heuristic, reference-conditioning
cache, diffusion knobs (#241). Detail → HISTORY (v0.8.9 notes) +
LEARNINGS (#221 entries).

## §229 transcribe.cpp perf audit — vs handy-computer/transcribe.cpp (ANALYSIS / OPEN)

Context: `handy-computer/transcribe.cpp` (github, cloned + read 2026-07-08) is a
direct ASR peer — ggml-based C/C++ STT, Metal/Vulkan/CUDA, 16 model families /
60+ variants, HF org `handy-computer`. Heavy roster overlap with us (Parakeet,
Canary, Canary-Qwen, Whisper, cohere-transcribe, SenseVoice, FunASR, Nemotron
streaming, Granite-Speech, Voxtral, Moonshine, Qwen3-ASR, GigaAM, MedASR).

**Design contrast.** They: 92k LOC, ASR-only, SHARED components — one
`src/conformer/`, `src/causal_lm/`, `src/transcribe-flash-policy`,
`src/transcribe-batch-util`, `src/transcribe-kaldi-fbank`, reused by every
family via thin `src/arch/<model>/` drivers. Us: 318k LOC, ASR **and** a large
TTS suite, per-model self-contained files (though core/ IS increasingly shared:
`core_conformer`=`core/fastconformer.h`, `core_sanm`, `core_dac`, `core_spm`,
`core_beam_decode`, `core/mel.cpp`). Their shared-infra design concentrates perf
work; our per-model design buys reach + flexibility at the cost of perf-hygiene
drift. **Caveat: this is a static code read, not wall-clock benchmarks** — treat
"more performant" as hot-path engineering, and gate any port on a decoded-output
A/B like every prior perf item (§176/§210/§227).

### Scoreboard (overlapping ASR paths only; TTS + breadth out of scope, ours)

| Subsystem | Winner | Basis |
|---|---|---|
| CPU compute + threading | **transcribe.cpp** (clear) | tinyBLAS forced ON (+29% enc); native ggml pool; SIGILL-safe fat-binary SIMD |
| Flash attention | **transcribe.cpp** (clear) | unified per-family×per-backend policy + head-dim padding |
| AR decoder (KV/beam/batch/sampling) | **transcribe.cpp** (clear) | in-graph argmax, build-once decode graph, batched multi-utterance decode |
| Conformer encoder | **transcribe.cpp** overall (nuanced) | our block is leaner single-stream BUT no batching + NORM_AFFINE breaks on Metal/Vulkan |
| Feature extraction | **transcribe.cpp** (modest) | centralized non-pow2 FFT + vDSP fp64; ours = per-backend "cargo-cult" pow2 radix-2 |
| Long-form / streaming | **split** | them: streaming latency (ring KV, sliding window). us: VAD-gated long-form (4 VAD engines; they have none) |
| Quantization | **CrispASR** (clear) | imatrix pipeline + low-bit/IQ types; they explicitly refuse imatrix |

**Bottom line:** they have the more performant core ASR *engine* (CPU, flash,
decoder, on-target encoder, features, streaming latency) via uniform shared-infra
optimization. We win quantization QUALITY (imatrix is a real, unmatched
differentiator), VAD-gated long-form robustness, and — off this axis — vastly
broader model/format/TTS coverage.

### VERIFIED near-free wins for us (both confirmed in-tree, not just agent claims)

- [ ] **Force `GGML_LLAMAFILE ON`** (tinyBLAS / llamafile_sgemm). CONFIRMED we
      ship it OFF: we never set it (except `-DGGML_LLAMAFILE=OFF` for WASM),
      ggml defaults `GGML_LLAMAFILE_DEFAULT OFF` (`ggml/CMakeLists.txt:113`), and
      `COHERE_MKL` (our only fast-GEMM path) defaults OFF (`CMakeLists.txt:137`).
      So a default CPU build runs the conformer/whisper encoder matmuls — the
      dominant ASR CPU cost — on stock ggml kernels. They force it ON
      (`CMakeLists.txt:283`, "~29% faster encoder"). One-line CMake change; A/B
      an encoder RTF before/after to confirm on our fleet. Biggest single CPU win.
- [ ] **Metal + Vulkan kernel for `GGML_OP_NORM_AFFINE`** (or stop emitting it on
      those backends). CONFIRMED: our fork's fused norm op (`ggml.h:500`,
      `ggml.c:3162`), used throughout `core/fastconformer.h` (7×/block: ff1/attn/
      conv/conv_ln/ff2/out) and `core/sanm.h:115,182`, has **zero** Metal refs
      (`ggml/src/ggml-metal/` — none) and **zero** Vulkan refs; the Metal
      `supports_op` switch handles only `GGML_OP_NORM`/`RMS_NORM`
      (`ggml-metal-device.m:1392-1393`) → NORM_AFFINE falls to `default:false`.
      Only CPU + CUDA implement it. So on M1 Metal AND Vulkan every layernorm in
      the canary/funasr/sanm encoders is offloaded to CPU with GPU↔CPU copies in
      the hot loop (~7×48 = ~336 per canary encode) — a regression on our two
      primary GPU targets, masked as a "leaner fused graph" that only pays off on
      CPU/CUDA. Fix: add the Metal (and Vulkan) NORM_AFFINE kernel + `supports_op`
      case, OR gate `core_conformer`/`core_sanm` to plain `ggml_norm`+mul+add on
      Metal/Vulkan. Gate on an M1 canary encode A/B.

### Deeper findings (file:line, ranked by leverage)

- **Decoder (cohere head-to-head, both greedy by default).** Theirs:
  `ggml_argmax` in-graph + 1-int32 readback (`arch/cohere/model.cpp:1211-1213`);
  build-once static GPU decode graph reused every token (`:1145-1199`); TRUE
  B-utterances-in-one-graph batched decode (`transcribe-batch-util.cpp:197-362`).
  Ours (`src/cohere.cpp`): full-vocab logits device→host every token +
  host `std::max_element` + host softmax (`:2606,2614`); rebuilds graph META per
  token (`:1521-1528`) though gallocr is pre-reserved so no realloc (`:2523-2534`);
  no batched decode anywhere. Transducer/TDT is a genuine tie (both persist LSTM
  state, hoist enc-proj, skip-on-blank; our parakeet joint is CPU/BLAS `cblas_sgemv`
  which is fine at n=1). **Our beam search is the slow form**: N sequential
  single-token forwards (`core/beam_decode.h:412-441`) each preceded by a
  `kv_snapshot_pool` deep-copy of the ENTIRE preallocated `dec_max_ctx` K+V (all
  layers), regardless of `n_past` (`core/attention.h:230-284`) — the #161 driver;
  copy traffic O(full-KV)×beams×tokens. Fixes: in-graph argmax; snapshot only the
  used `n_past` prefix (or use position-indexed cache without snapshot).
- **Flash policy.** Theirs: `transcribe-flash-policy` (thin env-override module) +
  per-family defaults honored at 17 sites, typed `BackendKind`, Metal
  simdgroup-mm capability probe, and **head-dim padding** (`pad_head_dim`,
  `arch/moonshine_streaming/decoder.cpp:37`) so odd head dims still ride the fused
  kernel. Ours: ~40 scattered per-model `p.flash_attn=bool` + per-model `getenv`
  guards added reactively per crashing backend (moss Vulkan `moss_transcribe.cpp:
  1556`; chatterbox PREC_F32 Metal route `chatterbox.cpp:1530`); NO head-dim
  padding; the Turing sm_75 −9-10% fusion regression (memory
  [[project_flash_attn_turing_regression]]) is unhandled at the model layer.
- **Conformer glue (single-stream, where WE are actually tighter).** Our block:
  fused `ggml_norm_affine` (1 op vs their 3-op LN), fused `ggml_siglu_swapped`
  GLU (1 vs 4), load-time BN fold (0 in-graph), zero-copy strided-view rel_shift
  `fastconformer.h:43-47` (1 op vs their fill+concat+cont = 2 copies of the
  O(2T×T×H) score-bias, `conformer.cpp:82-113`). So on CUDA/CPU single-stream our
  core is leaner — but we give it back via (a) no utterance batching (they carry a
  B axis end-to-end: 2-8× offline throughput we can't reach), (b) the NORM_AFFINE
  Metal/Vulkan cliff above, (c) 3 redundant flash `ggml_cont(Q/K/V)` per block
  (`fastconformer.h:327,330`) they avoid with strided flash inputs. THEIR
  single-stream weak spot = that copy-heavy rel_shift; the single-view trick is
  the first thing to port INTO them / keep in us.
- **Feature extraction.** Theirs: 2 centralized thread-safe frontends
  (`transcribe-mel.cpp`, `transcribe-kaldi-fbank.cpp`) — native mixed-radix FFT for
  non-pow2 `n_fft=400` (no pad-to-512 loss), Apple vDSP fp64 for pow2, cblas mel
  matmul, ~3e-7 vs ONNX. Ours: `core/mel.cpp` shares only the mel POST-processing
  (BLAS matmul + OMP over frames) but the FFT is per-backend and pow2-only
  (`core/mel.h:33`); backends ship near-identical hand-rolled copies
  (`voxtral.cpp:446` literally comments "cargo-cult from qwen3_asr.cpp";
  `core/fft.h:6-8` admits kokoro/mimo duplicate it). Consolidation target: one
  shared non-pow2 FFT frontend. Our only edge: we actually resample many
  containers (miniaudio) — but with basic `ma_resample_algorithm_linear`
  (`crispasr_audio.cpp:406`); switch to windowed-sinc if fidelity ever matters.
- **Long-form / streaming.** Theirs: first-class streaming (persistent
  `MoonshineStreamingKvCache` ring buffer, causal sliding-window = no overlap
  re-encode, LocalAgreement committed-prefix in the session core; bounded memory).
  Ours: chunked overlap-and-merge with LCS boundary dedup (`canary.cpp:1524-1665`)
  — better boundary TEXT but ~40% overlap re-encode (`parakeet.cpp:872`) + O(len)
  concat host buffer. We uniquely have 4 real VAD engines (silero/whisper,
  MarbleNet, FireRed, encdec) that skip silence — they have NONE (reject silero
  `.bin` at load, punt segmentation to caller). Confirms memory
  [[project_parakeet_208_session_longaudio]]: **§176 encoder-graph cache is a dud**
  — opt-in-OFF (`CRISPASR_PARAKEET_ENC_CACHE`), saves ~0.13% of window time,
  corrupts the 2nd same-`T_mel` encode under the shared sched (enc std 0.020→0.008
  → empty transcript). The one WORKING graph cache is the dimension-keyed,
  state-carrying `nemotron.cpp:1260-1317` (streaming FastConformer).
- **Quantization (WE win).** imatrix producer (`crispasr_imatrix.cpp`, eval-callback
  per MUL_MAT) + consumer (`examples/crispasr-quantize`, feeds `ggml_quantize_chunk`
  imatrix) + CER-gated A/B (`tools/imatrix_ab.py`) + CC0 corpus, plus the low-bit/IQ
  types (Q2_K/Q3_K/IQ4_NL/IQ4_XS) + `--tensor-type regex=type`. They refuse imatrix
  (`tools/transcribe-quantize/main.cpp:432`), cap at Q6_K. THEIR one edge worth
  stealing: a clean generalized bucket-table tensor classifier
  (`tools/transcribe-quantize/policy.cpp:50-278`) vs our per-arch `if`-ladder
  (`examples/crispasr-quantize/main.cpp:559-684`) — a refactor, not a perf change.

### Actions (ranked)

0. **GATE on measurement** (same discipline as §227.0). Before porting anything:
   one M1 + one T4/CPU run of an encoder RTF A/B for wins 1-2. If GGML_LLAMAFILE
   ON and a Metal NORM_AFFINE kernel each move the encoder ≥5%, do them.
1. [ ] Force GGML_LLAMAFILE ON (win #1). Cheapest, biggest CPU lever.
2. [ ] Metal (+Vulkan) NORM_AFFINE kernel or Metal/Vulkan fallback (win #2).
3. [ ] In-graph argmax + used-prefix-only KV snapshot for cohere/AR decode (kills
      the #161 copy storm; also drop per-token full-vocab readback).
4. [ ] Flash head-dim padding helper + a real per-backend flash-capability gate
      (fold in the Turing sm_75 opt-out from [[project_flash_attn_turing_regression]]).
5. [ ] Consolidate the per-backend pow2 FFT into one shared non-pow2 frontend.
6. [ ] (stretch) utterance-batched encoder + batched multi-utterance decode for
      offline throughput — large, and exactly the cached-graph minefield of
      #171/#184/§227.2; do not start before 0.
7. [ ] (cleanup) bucket-table quant tensor classifier to replace the per-arch ladder.

## §232 qwen3-tts code predictor perf — fuse 15 graph dispatches into 1 (#245, OPEN)

**Problem:** CrispASR's qwen3-tts code predictor runs 15 separate ggml graph
dispatches per audio frame (one per codebook 1-15). This makes it ~40x slower
per frame than predict-woo/qwen3-tts.cpp (10s/frame vs 225ms/frame on CPU).

**Competitive benchmarks (CPU, 0.6B Q8_0):**

| Engine | Talker ms/frame | CodePred ms/frame | RTF |
|--------|----------------|-------------------|-----|
| CrispASR | ~13,000 | ~10,000 (15×680) | 0.006x |
| predict-woo | ~84 | ~225 | ~0.5x |
| qwentts.cpp | no data | no data (claims ~90x with cache) | no data |

**Fix:** Fuse the 15 `run_code_pred_kv()` calls into a single graph with
sequential KV-cached decode steps. The code predictor is a 5-layer transformer
with 15 sequential AR steps — should be one graph, not 15 dispatches.

**Also:**
- [ ] Enable O15 (persistent code_pred graph) by default
- [ ] Enable LK_BUCKET (bucketed Lk talker) by default
- [ ] Profile on Metal/CUDA — CPU numbers on the 4-core VPS are misleading
- [ ] Benchmark CrispASR vs predict-woo vs qwentts.cpp on a Kaggle GPU kernel

**Effort:** M (the graph fusion is the hard part; the flag defaults are trivial).
**Priority:** HIGH — TTS perf gap is user-visible and competitively damaging.

### §232 update (2026-07-10, M1 Metal) — CP_DIRECT implemented, LK_BUCKET un-broken

**Why not one fused 15-step graph:** each step samples (top_k=50, temp, seeded
RNG) on the CPU and the sampled id selects the next `codec_embd` row — bit-
identical output therefore requires a CPU logits round-trip between steps.
The right fix is making each dispatch cheap, not fusing.

**What the 15 dispatches actually cost on M1 Metal (0.6B Q8_0, seed 42):**
build ~2-12 ms + sched alloc ~10-50 ms + compute ~200-600 ms per frame, where
"compute" is dominated by `ggml_backend_sched` overhead + command-buffer
round-trips, not kernels (the code_pred is ~64 M MACs/step).

**Fix shipped — `QWEN3_TTS_CP_DIRECT` (opt-in pending clean bench + CUDA):**
two persistent sched-free graphs (T=2 step-0, T=1 step with O15 topology:
fixed Lk, runtime positions → RoPE + set_rows KV write, lm_head via the
writable slot), each gallocr-allocated ONCE on the code_pred backend; a step
is tensor_set + one `ggml_backend_graph_compute` + logits read. No scheduler
⇒ the #56-class sched-reuse breakage cannot occur. Ops-supported + placement
checked once at init; falls back to the sched paths otherwise.

**Measured (M1 Metal, 0.6B Q8_0, seed 42, box otherwise quiet):** ar_loop
564 → 179 ms/frame; code_pred ~408 → ~125 ms/frame; RTF 10.4 → 3.3.
**Output md5-identical WAV** across default / O15=1 / CP_DIRECT / CP_DIRECT+
LK_BUCKET, 9/9 runs.

**LK_BUCKET was SEGFAULTING on main** (nil-buffer inputs on Metal — the same
ggml sched-plan-reuse tightening; reproduced with LK_BUCKET=1 alone on
unmodified code). Reworked the bucket path onto per-bucket persistent
gallocr + sched-free dispatch: no more crash, md5-identical. But at short
outputs fixed-Lk=256 attention costs more than the saved rebuilds (dirlk
slower than dir in 3/3 interleaved reps) → stays opt-in.

**Remaining:**
- [x] CUDA validation (Kaggle P100, kernel `chr1s4/crispasr-qwen3-cp-direct-cuda`,
      2026-07-11): all four configs rc=0 + **WAV md5-identical** + ASR
      roundtrip verbatim. base 25.4 → direct 22.5 ms/frame (−11%);
      direct+LK_BUCKET 21.4 (fastest on CUDA). **O15=1 no longer crashes on
      P100** (#56 was Jetson sm_87; retire-O15 decision still needs a Jetson
      or stays cosmetic).
- [x] Clean-box Metal bench (load-gated, 3 interleaved reps): base ≈ direct
      within noise (min 110.4 vs 111.3 ms/frame) — the earlier 3x gap was the
      sched path degrading under load; direct's win on Metal is robustness
      under contention, not idle speed. dirlk consistently ~10% slower.
- [x] CPU `--no-gpu` sanity: md5-identical but direct is ~2x SLOWER
      (87 → 160 ms/frame) — no dispatch cost to save; the per-step lm_head
      slot blit (~2.2 MB memcpy ×14/frame) dominates.
- [x] **Default flipped to conditional** (2026-07-11): CP_DIRECT ON when the
      code_pred runs on a GPU backend, OFF on CPU; env override wins both
      ways. LK_BUCKET stays opt-in (Metal regression; CUDA users can enable).
- [x] 1.7B mtp-fused: **WAV md5-identical** on 1.7B-CustomVoice Q8_0 Metal
      (baked speaker, no ICL; `small_to_mtp fused` + `cp_direct active` in
      the default run vs CP_DIRECT=0). The 1.7B-Base + ICL route on the
      16 GB Mac dies in codec decode on BOTH paths (jetsam/timeout,
      pre-existing, ~1800-frame ICL ref prep + codec at ~5 GB free) —
      separate issue, not CP_DIRECT.
- [x] Talker step PROFILED (2026-07-11, M1 Metal, 0.6B Q8_0) — direct
      treatment is NOT the fix. `CRISPASR_METAL_PROFILE` on the 1068-node
      talker step graph: **encode 2-3 ms, GPU-execute 38-42 ms** — the step
      is GPU-execution-bound, so sched-free dispatch / ICB replay can't
      help (same conclusion as §210 granite). Confirmed empirically: the
      sched-free bucket path computes in ~56 ms/step vs ~54 ms dynamic.
      The eval-callback op profile shows why: ~460 real ops/step, mul_mat
      only ~24%; ~85 `cont` copies + hundreds of tiny norm/add/rope
      kernels/step — death by kernel count, amplified by desktop-GPU
      time-slicing (probe gpu_us on identical 193-node code_pred graphs
      swings 4-17 ms under WindowServer/browser GPU load). Remaining lever
      is node-count slimming in `core_attn::kv_self_attn` (fuse qk-norm,
      drop per-layer conts) — shared by 30+ backends, high regression risk
      for a bounded win → DEPRIORITIZED; revisit only with a dedicated
      diff-harness campaign.
- [x] Codec decode FIXED — FASTCONV (2026-07-11, default ON, opt-out
      `QWEN3_TTS_CODEC_FASTCONV=0`). `QWEN3_TTS_CODEC_TRACE` per-node profile
      showed the 3-4 s codec wall was NOT inherent conv compute: (a) K=1
      convs went through im2col — a pure copy with ~300 MB intermediates at
      75 ms each, ×12 sites (every res-unit conv2); (b) the causal left-pad
      `ggml_pad_ext` nodes landed on the CPU backend (Metal rejects
      asymmetric PAD) forcing sched splits + copies — replaced by
      pad-inside-im2col + crop-first-T (voxcpm2's causal_conv1d_ggml trick);
      (c) the fork's ggml_conv_1d casts F16 kernels to F32 inside EVERY
      graph (in_conv cast alone ~70 ms/decode) — F32 kernels now baked once
      at load (+~55 MB). Result: codec 3.9 s → 1.3 s on M1 Metal (~3×,
      md5-identical WAV), 9.1 s → 4.3 s CPU (~2.1×, 1 int16 LSB / PCM cos
      1.00000000). Total 0.6B pipeline RTF best-case 2.9 → 1.25. Remaining
      codec cost is the legitimate K=7 SEANet im2cols. The 16 GB-Mac 1.7B
      codec jetsam is FIXED in practice: post-FASTCONV, 1.7B-CustomVoice
      Q8_0 with a 156-frame (~12.5 s) output decodes rc=0 with DEFAULT
      chunking at ~6 GB free (this exact profile died pre-FASTCONV);
      QWEN3_TTS_CODEC_CHUNK=48 stays available for tighter boxes (note:
      non-default ctx changes conv warm-up at chunk boundaries → WAV not
      byte-equal to the default config; pre-existing chunked-decode
      property, not FASTCONV).
      **CUDA validated (kernel v2, P100, 2026-07-11):** shipped defaults
      (CP_DIRECT + FASTCONV) vs full-legacy — rc=0, PCM cos 0.9999999997
      (md5 drift, same K=1-matmul realization class as CPU), ASR roundtrip
      verbatim; AR 24.7 → 22.3 ms/frame, wall 6.9 → 5.6 s. direct and
      direct+LK_BUCKET WAVs byte-identical (FASTCONV deterministic on CUDA).
- [ ] CPU direct-path rescue (optional): avoid the slot blit with 14 cached
      per-cb graphs or in-graph lm_head selection via get_rows, then revisit
      the CPU default.

## §235 next perf targets — triage after the §232 qwen3-tts sweep (2026-07-11)

Grounded in the §232 GPU profiling, the qwen3-tts per-op traces, and the
PERFORMANCE.md gap list. Recalibrated after checking what's already tried
and what parallel sessions own:

- **Transducer decoder → GPU: NOT on M1/CPU-batched; a CUDA campaign is being
    scoped by a parallel session.** PLAN §232 lists it as the top win (decode
    runs on host `cblas_sgemv/sgemm`, paper 12-19×). The naive attempt —
    CPU-side batched decode — is **5-9× SLOWER** (`d0bb4601`, `3fcd5ff3`):
    a decode loop of tiny per-step matmuls + host↔device sync is where CPU
    cblas wins (same class as qwen3-tts CP_DIRECT-on-CPU). But a parallel
    session is now scoping a *proper* in-graph GPU decode on datacenter cards
    (`eb5c5cc5` plan, `a1103c04` learning 29 — gate it on Kaggle/CUDA BEFORE
    building, since the M1/CPU result doesn't settle a P100/T4 port). So: not
    a target for THIS session (owned + M1 can't validate it), not a flat dead
    end — leave the cblas decoder in place pending their CUDA verdict.
- **OWNED by a parallel session — Moonshine encoder** (im2col of k≈127 conv1
  over ~176K raw samples; 728 ms vs ~58 ms competitor). Worktree
  `moonshine-decode-stash` @ `46127dab` is active on it. Hands off.
- **qwen3-tts talker** — GPU-execution-bound (§232 profiled: encode 2-3 ms vs
  GPU 38-42 ms). No dispatch trick helps; only `kv_self_attn` node-slimming
  (shared 30-backend helper, high risk). Deprioritized, not a target.
- [x] **Pocket-TTS backbone KV cache — MEASURED, NOT WORTH IT (2026-07-11).**
      The plan was a resident device KV cache to kill the O(T²) host re-upload
      in `backbone_forward_step_ggml` (~L1208-1231). The measurement gate
      killed it, exactly as intended (cf. vibevoice graph-cache 0%-win). M1
      Metal, `pocket-tts-english-f16.gguf`, 71-frame synthesis
      (`POCKET_TTS_BENCH=1`, timers landed `d895e581`):
      - synthesize **4780 ms**; voice mimi-encode seanet 1046 + xfmr 1676 =
        **2721 ms (57 %)** — one-time / disk-cacheable (§224 `e5f75436`);
      - AR loop: **backbone 587 ms (8.27/frame, 12 %)** vs **flow 1228 ms
        (17.29/frame, 26 %)**; mimi_decode **1003 ms (21 %)**.
      The backbone is only ~8 ms/frame — a risky attention-graph rewrite that
      saves ≤12 % (backbone→0 ceiling), realistically ~7 %. Not worth it;
      the O(T²) re-upload is real but the constant is tiny (6-layer D=1024).
      **Ownership released.** Bigger repeatable levers if pocket-tts is ever
      revisited: the flow-net diffusion head (`flow_net_forward`, `lsd_steps`
      per frame — biggest AR cost) and mimi decode; the voice encode is the
      largest single cost but is already cacheable. None has an obvious
      cheap+safe win — deprioritized.
- [x] **Pocket-TTS voice-clone onset "gong" — ROOT-CAUSED + FIXED (2026-07-11).**
      Voice-cloned syntheses (e.g. jfk.wav prompt) opened with a loud onset
      transient ("gong"); no-voice output was clean. Root cause via reference
      diff against kyutai's `pocket_tts` (moshi) at F16: our `mimi_encode`
      padded PCM to a **hop-length (120)** multiple, but the encoder does a
      further `/downsample_stride` (16) conv to reach the 12.5 Hz latent rate.
      Padding only to a hop could leave the downsample one **short (floor)**,
      yielding one fewer latent voice frame than the reference (which ceils).
      For jfk.wav (~137.5 latent frames) we produced 137 vs the reference's
      138. The missing voice frame shifted every downstream RoPE position by
      one, corrupting the very first generated latent → the Mimi decoder
      rendered it as the onset gong. Fix (`src/pocket_tts.cpp` ~L2703): pad to
      a full **latent** frame (`hop * downsample_stride` = 1920 samples). After:
      138 latent frames, voice conditioning 139 (=138+bos) matching reference;
      onset RMS t0 **0.0621 → 0.0045** (reference is 0.0045). Confirmed the
      codec itself was correct (force-decoding the reference's exact latents
      through OUR codec was already clean); the bug was purely the encoder
      frame count. **Must clear the §224 voice-latent disk cache
      (`pocket-voice-*.latents`, or `CRISPASR_POCKET_VOICE_CACHE=0`) after this
      fix** — stale 137-frame latents would otherwise mask it.
      **Per-conv ceil-framing refinement — TRIED, REJECTED by the measurement
      gate (2026-07-11).** Hypothesis: replace the raw-PCM pre-pad with moshi's
      `MimiConv1d._get_extra_padding_for_conv1d` applied at each strided conv (the
      scheme `qwen3_tts`'s codec uses), for byte-exact tail values. Built + diffed
      vs the reference: the pre-pad already **value-matches** the reference (gen
      onset t0 **0.0045**, profile `[0.0045 0.0034 0.0034 0.0047 …]` identical),
      whereas per-conv gave onset **0.0579** and **backbone_out0 corr vs reference
      = 0.18** (catastrophic). Conclusion: pocket-tts's Mimi VAE encoder does NOT
      use independent per-conv extra padding — it effectively pads the input to a
      whole latent frame, which is exactly the pre-pad fix. Do not retry per-conv
      here (it's correct for qwen3's codec, wrong for this one). Reverted; kept the
      pre-pad fix.
      **Boundary hardening — PASSED (2026-07-11).** Ran the fix across trimmed
      refs straddling 1920-sample latent-frame boundaries + real clips (5-15 s).
      Frame count = moshi's `ceil(N/1920)` at every length incl. exact multiples
      (verified against moshi's KV `offset`: N=261120→136, 263040→137, 263041→138,
      264000→138 — all match). Onset clean for all real/non-exact refs. The one
      exact-multiple clip (263040 = 137×1920, zero trailing pad) shows a loud
      onset — but **moshi generates the same loud onset for it** (t0 0.066 vs
      0.063), so it's faithful truncation-content behavior, not a bug; real refs
      never land on an exact multiple. No further code change. NB: explicit
      `--voice` cloning needs `--i-have-rights` (consent gate); the auto-default
      voice path does not.
- [x] **CosyVoice3 HiFT / FASTCONV — MEASURED, NOT WORTH IT (2026-07-11).**
      Downloaded the full CV3 GGUF set to `/Volumes/backups/ai/crispasr-gguf`,
      `COSYVOICE3_BENCH=1` on a 174-mel synthesis (M1 Metal, quiet box). Wall
      split (~7.4 s total): **flow_euler 3526 ms (48 %)**, **lm_ar_decode
      2681 ms (36 %)**, **hift_vocoder 1152 ms (16 %)**, tokenize+pre_la
      ~156 ms. HiFT is 16 %, but the FASTCONV levers barely touch it: (a) HiFT
      **already uses pad-in-im2col** (`cv3_causal_conv1d`), so trick #2 is done;
      (b) HiFT decode is a SINGLE sched graph (`cv3_extract_hift_inference`
      L1628/1643), so an F32-kernel-bake casts once/synthesis ≈ 15-20 ms, not
      per-frame; (c) the istft twiddle-table saves ~20-30 ms (`std::cos/sin` in
      the inner loop, but T_stft×16×9 is only ~3 M calls); (d) no meaningful
      K=1 convs (kernels 16/11/7). Addressable ≈ 40-50 ms of a 7.4 s wall =
      **~0.6 %** — a regression-risky conv sweep for <1 % fails the A/B bar
      (vibevoice 0%-win). The real CV3 costs are the flow-matching CFM
      (48 %, `cfm_steps=10`×CFG×22 DiT blocks) and the Qwen2-0.5B AR decode
      (36 %) — both compute-bound transformer work, no cheap+safe conv-style
      win; graph-caching the DiT step is the sched-reuse hazard and won't help
      a compute-bound graph (cf. the talker verdict). Deprioritized.
- [x] **CosyVoice3 CFM flow-steps — CHARACTERIZED; recommend default 10→6
      (2026-07-11).** `flow_euler` is the single biggest CV3 cost (48 % of the
      wall) and `COSYVOICE3_FLOW_STEPS` is already a knob (default 10). Swept
      10/8/6/4 on M1 Metal (`--seed 42` → identical LM tokens + init noise, so
      the ONLY variable is CFM step count). **Quality = log-mel-spectrogram
      corr vs the 10-step output** (the chatterbox §207 metric) — ASR roundtrip
      is useless here (verbatim at 10/8/6, and even the 4-step "brown→bound"
      slip is near-threshold). Curve: corr **8→0.9948, 6→0.9925, 4→0.9895**;
      ASR verbatim at 8/6, a one-word slip at 4. **6 steps is the sweet spot**
      (corr 0.9925 ≥ 0.98 parity, ASR verbatim, matches chatterbox) — flow work
      is ~linear in steps so 10→6 ≈ **−40 % flow ≈ −19 % of the CV3 wall**.
      4 steps is rejected: the ASR word-slip is a localized artifact the
      whole-utterance corr averages out (the corr band 0.9895–0.9948 is
      compressed — the ASR slip is the more sensitive signal at 4).
      **Multi-sentence confirmation (2026-07-11): 6 steps holds across diverse
      text.** 6-vs-10 mel-corr on short / numbers / long = 0.9953 / 0.9930 /
      0.9938 (all ≥ 0.99), and ASR@6 is verbatim-identical to ASR@10 on short
      and long. (The numbers sentence is inconclusive on the ASR axis only —
      moonshine-tiny garbles spoken-out digits at BOTH 10 and 6 steps, an ASR
      failure, not 6-step degradation; the 0.9930 corr confirms 6≈10
      acoustically.) So the recommendation is now robust, not single-utterance.
      **RECOMMEND lowering the default 10→6; NOT flipped here** — a
      quality-affecting default ships to every user; the remaining gate is a
      human listen (corr/ASR are proxies; vibevoice onset lesson). The knob
      already lets speed-seekers opt in today. Measured `flow_euler` wall
      (8133/7481/5758/3962 ms) is contention-noisy (runs at load 5–19) —
      speedup stated from the linear-in-steps model, not the raw wall-clock.

## §234 omnivoice — persistent step graphs + silence root cause (DONE)

Spun out of the reporter's #245 question ("does this affect omnivoice?").

- **Perf DONE (2026-07-11, 4ad5f2ad):** the masked-iteration loop rebuilt +
  gallocr-allocated the 28-layer forward graph 2×/step (cond + CFG uncond),
  64×/synthesis, despite fixed T and no KV state. Persistent per-arm graphs:
  CPU 135.5 → 82.8 s (WAV **byte-identical**), Metal 509.9 → 245.4 s
  (~1.6–2×, single-rep). Gates: `OMNIVOICE_PERSISTENT_GRAPH=0` (per-call
  path), `OMNIVOICE_DEBUG_SUM=1` (per-forward logit sums). Gotcha that cost
  a bisect: gallocr aliases input-flagged tensor slots with intermediates —
  ALL inputs must be re-set before every compute of a persistent graph
  (→ LEARNINGS).
- **SILENCE ROOT-CAUSED AND FIXED (2026-07-11, 9fde8411).** Was never
  M1-specific: the C++ gen-config fallbacks didn't match the blueprint's
  `OmniVoiceGenerationConfig` — guidance 1.0 (ref 2.0), class_temp 0.7
  (ref 0.0 = argmax), position_temp 4.5 (ref 5.0), layer_penalty 0.5 (ref
  5.0, the coarse-to-fine unmask ordering), t_shift 1.0 (ref 0.1, a
  different unmask-schedule shape). The wrong decode policy degenerated
  into near-constant silence codes (59 unique tokens / 1344 positions,
  top token ×159) which the codec faithfully rendered as silence. With
  blueprint defaults: 278 unique codes, real audio, whisper roundtrip
  VERBATIM on CPU, near-verbatim on Metal. Diagnosis chain that worked:
  `OMNIVOICE_DEBUG_CODES=1` histogram split codec-vs-LLM in one run
  (codes degenerate ⇒ codec innocent); text ids verified byte-exact vs
  tokenizers-lib (glm lesson — not the bug this time); embedding
  offsets/mixing verified == `_prepare_embed_inputs`; then a
  side-by-side of `OmniVoiceGenerationConfig` vs `ov_gen_config` found
  the five wrong defaults. Remaining nice-to-haves: peak hits 1.0
  (clipping — check the blueprint's postprocess/fade for an output gain
  we skip); `<|denoise|>` token for the ref-audio path (blueprint adds
  it when denoise=True + ref present); voice-cloning path not yet
  roundtrip-validated; parity kernel should go green now — rerun it.

## §230 Issue #238 — kyutai/stt-2.6b-en support (DONE)

**Shipped 2026-07-10.** Full support for the 48L / 2.6B English-only Kyutai STT model.

Key changes:
- Converter reads `audio_silence_prefix_seconds` from `stt_config` → GGUF KV.
- Runtime prepends silence prefix (1.0 s @ 24 kHz) before Mimi encode.
- `kyutai_stt_total_lookahead_seconds()` returns `audio_delay + silence_prefix`
  (3.5 s for 2.6B vs 0.5 s for 1B). CLI adapter sizes silence tail from this.
- Registry entry `kyutai-stt-2.6b` → `cstr/kyutai-stt-2.6b-en-GGUF` Q4_K.
- Reference backend `tools/reference_backends/kyutai_stt.py` with full audio
  conditioning (silence tail + prefix + resample). Frozen ref GGUF on HF.
- Diff harness dispatch in `crispasr_diff_main.cpp` (transcript-level; stage
  APIs pending).
- Kaggle kernel v4 (`chr1s4/crispasr-kyutai-stt-2-6b-convert`): F16/Q8_0/Q4_K
  + ref GGUF all uploaded.

**Lesson learned:** Python reference dumpers must apply identical audio
conditioning as the C++ runtime. The initial ref GGUF had a truncated
transcript because the dumper fed raw PCM without the silence tail/prefix.

Commits: 42782648, 7cb079a0, 7f60cf72, f508dac1, bba1f0e5, 0595ab6b.

## §231 Issue triage 2026-07-10 — bulk close of resolved issues

**Closed 24 issues** that had fixes on main but were never formally closed:

| Issue | Title | Resolution |
|-------|-------|------------|
| #238 | kyutai/stt-2.6b-en | Full backend + diff harness (§230) |
| #239 | Opus popen("rb") on Linux | 276328d0 + fcb91792 |
| #216 | Kokoro G2P C++ → "k" | be8f7305 |
| #222 | Silero LID Vulkan segfault | 222b6781 |
| #182 | Chatterbox segfault long text | 723d5f3c |
| #231 | Cohere Arabic | 130ca753 + edf31a3e |
| #221 | Irodori-TTS | Full pipeline + voice cloning |
| #209 | MOSS-Transcribe | Both MOSS backends working |
| #220 | Chatterbox CUDA illegal memory | c78b187b |
| #194 | CosyVoice3 not starting | 589ec2d7 + 97735b78 |
| #192 | TADA noise | 3b52d268 |
| #171 | TTS regression (UAF) | c96c4997 |
| #201 | TADA voice at inference | 3b52d268 |
| #217 | Alignment mode | e188bcfa |
| #195 | ReazonSpeech v2 | 308ba15d |
| #212 | Qwen3-ASR-1.7B-JA-Anime | 612bade0 |
| #197 | TADA rep penalty | 0f3e6d2c |
| #169 | espeak-ng lang indicators | 37328f27 |
| #139 | Windows encoding GGUF | Converters fixed |
| #135 | FunASR/SenseVoice | Both backends shipped |
| #128 | --hf-repo flag | b77a74eb |
| #76  | Chatterbox-turbo distorted | Vocoder parity fixes |
| #75  | IndexTTS | Full backend + ref cache |
| #46  | MOSS model | Both MOSS backends |

**Further closed in same session (4 more):**

| #234 | OmniVoice TTS | Full pipeline shipped (codes + DAC decode) |
| #218 | Phrase repeats | fix_loops + word dedup + glm/qwen3 parity |
| #193 | C# binding | bindings/csharp/ shipped |
| #89  | Parakeet Japanese | 97% recall via VAD+gap-fill |

**Further closed (same session, second pass):**

| #240 | qwen3-asr-1.7b Q4_K empty | Re-baked with audio Q8_0 floor |
| #204 | New models | ARK-ASR + Higgs-STT both shipped |
| #137 | Termux build | Workaround documented (BUILD_SHARED_LIBS=OFF) |
| #131 | Older-glibc build | ubuntu-22.04 + Vulkan release in CI |

**New issue created:**

| #245 | qwen3-tts code predictor perf | 15 graph dispatches → fuse into 1 (§232) |

**Remaining open issues (7):**

- **#245** qwen3-tts code predictor perf — HIGH, 40x slower than competitors (§232)
- **#227** VAD reuse — feature request
- **#198** Higgs-Audio TTS — feature request (porting suggestion)
- **#196** Gemma4 larger — feature request (12B/26B/31B)
- **#174** LA Studio — community showcase (not a bug)
- **#130** CPU perf docs — documentation request
- **#125** Long audio broken (v0.6.10) — most fixes shipped, awaiting retest
- **#81** ONNX-ASR comparison — benchmarked 2026-07-10, CrispASR faster on all models
- **#130** CPU perf docs — documentation request
- **#125** Long audio broken (v0.6.10) — most fixes shipped, awaiting retest
- **#93**  Voxtral 4B TTS — feature request (no TTS backend for this model yet)
- **#81**  ONNX-ASR comparison — discussion/tracking

## §232 transcribe.cpp parity — close the RTF gap (OPEN)

**Context:** Kaggle P100 GPU-vs-GPU benchmark (v11, 2026-07-10) shows CrispASR loses
on 4/9 shared models by 3-21x. Root causes are identified per-backend below.
CrispASR wins 3/9 and ties 2/9, so the engine IS competitive — the losses are
backend-specific, not architectural. Full results in `docs/performance.md`.

### Diagnosis (from VPS CPU profiling + Kaggle GPU timing)

| Model | CA GPU RTF | TC GPU RTF | Gap | Bottleneck (profiled) |
|-------|-----------|-----------|-----|----------------------|
| Moonshine Streaming Tiny | 0.278 | 0.013 | 21x | Streaming encoder: 550 frame-by-frame forward passes instead of single batch |
| Nemotron 3.5 ASR 0.6B | 0.385 | 0.046 | 8.4x | Cache-aware streaming FastConformer encoder (15.6s/20.5s on CPU = 76%) |
| Moonshine Tiny | 0.080 | 0.013 | 6.3x | AR decoder (8.4s/10.1s on CPU = 83%) — per-token graph rebuild + full-KV snapshot |
| Parakeet TDT 0.6B | 0.099 | 0.032 | 3.1x | Different model versions (CA=v3 multilingual vs TC=v2 EN-only); need same-version A/B |

Models where CrispASR wins or ties (no action needed):
- SenseVoice Small: CA 0.018 vs TC 0.020 (CA wins)
- Qwen3-ASR 0.6B: CA 0.087 vs TC 0.116 (CA 1.3x faster)
- Canary 1B v2: CA 0.042 vs TC 0.054 (CA 1.3x faster)
- FunASR Nano 2512: CA 0.043 vs TC 0.142 (CA 3.3x faster; TC has GPU inference bug)
- Whisper base: CA 0.025 vs TC 0.021 (near parity)

### Fix 1: Moonshine decoder — in-graph argmax + build-once decode graph [HIGH]

**Problem:** Moonshine Tiny decoder takes 8.4s (83% of total) for 26 tokens on CPU.
That's ~320ms/token. The decode loop (`moonshine.cpp:999-1016`) rebuilds the ggml
graph every token and reads full-vocab logits to host for `std::max_element`.

**Fix (from §229 analysis):**
- (a) In-graph `ggml_argmax` — one int32 readback instead of vocab-wide F32 readback
- (b) Build the decode graph once, reuse it per token (static graph pattern from
  transcribe.cpp `arch/cohere/model.cpp:1145-1199`)
- (c) KV snapshot: only copy the used `n_past` prefix, not the full preallocated
  `dec_max_ctx` K+V (the §229 #161 copy storm)

**Expected impact:** 2-5x decoder speedup → Moonshine Tiny should drop from 0.080
to ~0.02-0.03 RTF on GPU, matching transcribe.cpp.

**Where to test:** VPS (CPU, 8GB) for correctness + A/B RTF. Kaggle P100 for GPU
numbers. M1 MacBook for Metal verification.

**Files:** `src/moonshine.cpp` (decoder loop ~999-1100), `src/moonshine.h`

### Fix 2: Moonshine Streaming — offline batch encoder fast-path [HIGH]

**Problem:** The streaming backend processes 550 encoder frames individually when
given a complete file. transcribe.cpp runs a single batched encoder forward pass.

**Fix:** When `moonshine_streaming_transcribe()` receives the full audio (not
real-time streaming), batch all frames into one encoder forward pass. The decoder
can still run token-by-token (it's fast). Gate on a `batch_encode` flag or detect
non-streaming mode from the caller.

**Expected impact:** 10-20x speedup on encoder → RTF should drop from 0.278 to
~0.015-0.030, matching or beating transcribe.cpp.

**Where to test:** VPS (CPU) for correctness. Kaggle P100 for GPU RTF.

**Files:** `src/moonshine_streaming.cpp` (encoder loop ~1208+)

### Fix 3: Nemotron encoder — non-streaming full-pass mode [MEDIUM]

**Problem:** The cache-aware streaming FastConformer encoder uses windowed attention
(L=56, R=3) even for offline files. This requires multiple passes with overlapping
windows. The encoder alone takes 15.6s on CPU (76% of total).

**Fix:** Add a non-streaming path that uses full bidirectional attention when
processing a complete file. transcribe.cpp likely uses full attention for offline.
Gate on `--chunk-seconds 0` (full-audio mode) vs streaming.

**Complication:** The nemotron model was trained with streaming attention. Switching
to full attention may produce different (possibly worse) results. Need to verify
WER parity before committing.

**Expected impact:** 3-5x encoder speedup if full attention works → RTF should drop
from 0.385 to ~0.08-0.12.

**Where to test:** VPS (CPU) for correctness + WER check. Kaggle P100 for GPU RTF.
May need M1 MacBook or A1000 notebook for Metal/Vulkan testing.

**Files:** `src/nemotron.cpp` (encoder ~1260-1317, streaming attention mask)

### Fix 4: GGML_LLAMAFILE ON by default [HIGH, trivial]

**Problem:** CrispASR ships with `GGML_LLAMAFILE OFF` (the ggml default).
transcribe.cpp forces it ON: "~29% faster encoder". This affects ALL backends
on CPU — the encoder matmuls (dominant CPU cost) run on stock ggml kernels
instead of tinyBLAS/llamafile_sgemm.

**Fix:** One-line CMake change: `set(GGML_LLAMAFILE ON)` or
`option(GGML_LLAMAFILE "" ON)` in the top-level CMakeLists.txt.

**Expected impact:** ~15-29% CPU encoder speedup across all backends. Zero risk
(tinyBLAS is a drop-in GEMM replacement). This alone would close the CPU gap
from 1.3-3x to ~1.0-2.3x.

**Where to test:** VPS (CPU). Build with and without, compare jfk.wav RTF across
3-4 backends. No GPU needed.

**Files:** `CMakeLists.txt` (one line)

### Fix 5: Parakeet — same-version A/B [LOW]

**Problem:** CrispASR uses parakeet-tdt-0.6b-**v3** (25 EU languages) while
transcribe.cpp uses **v2** (EN-only). v3 has more parameters in the language
embedding. The 3.1x gap may be the model, not the engine.

**Fix:** Download the v2 GGUF and benchmark both. If v2 is faster, note it in
docs. If comparable, the gap is real and needs profiling.

**Where to test:** VPS (CPU), quick test.

### Testing matrix

| Fix | VPS (CPU, 8GB, no GPU) | Kaggle P100 (GPU) | M1 MacBook (Metal) | A1000 Notebook (Vulkan) |
|-----|------------------------|-------------------|---------------------|------------------------|
| Fix 1: Moonshine decoder | Correctness + CPU A/B | GPU RTF | Metal verify | Vulkan verify |
| Fix 2: Moonshine Streaming batch | Correctness + CPU A/B | GPU RTF | Metal verify | — |
| Fix 3: Nemotron full-pass | Correctness + WER | GPU RTF | — | — |
| Fix 4: LLAMAFILE ON | **Primary** (CPU A/B) | CPU A/B | Metal A/B | — |
| Fix 5: Parakeet v2 A/B | **Primary** (quick) | GPU A/B | — | — |

### Status

- [x] **Fix 4** (LLAMAFILE ON) — DONE (`4e9b4087`). No measurable CPU benefit on
  Intel Xeon Skylake with Q4_K models (A/B: neutral to +11% slower due to variance).
  tinyBLAS optimises the F32 GEMM but Q4_K dequant dominates. May help on ARM NEON
  or F16 weights. Kept ON as the upstream-recommended default.

### Priority order (revised)

1. **Fix 1** (Moonshine decoder) — medium effort, 2-5x decoder speedup, VPS-testable.
   **This is the highest-leverage fix.** The decoder is 83% of Moonshine Tiny time.
2. **Fix 2** (Moonshine Streaming batch) — medium effort, 10-20x encoder speedup
3. **Fix 3** (Nemotron full-pass) — harder, needs WER verification, risk of regression
4. **Fix 5** (Parakeet A/B) — just a comparison, no code change

### §232 Update — GPU profiling results (v12, 2026-07-10)

**Root causes confirmed from Kaggle P100 fine-grained timing:**

| Model | CA bottleneck | CA time | TC time | Root cause |
|-------|--------------|---------|---------|------------|
| Parakeet TDT | decode (CPU cblas) | 955ms | 51ms (GPU) | RNNT/TDT decoder uses host-side cblas_sgemv, NOT ggml graph. TC runs decoder on GPU. |
| Nemotron | rnnt_decode (CPU cblas) | 2900ms | 238ms (GPU) | Same: host-side LSTM+joint via cblas. TC runs on GPU. |
| Moonshine Tiny | encoder (GPU) | 728ms | 58ms (GPU) | conv_1d_f32 on raw 176K samples via im2col creates huge intermediate. TC may use optimized audio frontend. |
| Moonshine Streaming | encoder (GPU) | ~3000ms | 71ms (GPU) | Sliding-window masks (550²×6 F16) + full attention over masked positions. |

**Critical finding: Transducer decoders (Parakeet TDT, Nemotron RNNT) are CPU-only.**
The `predictor_step()` + `joint_step()` functions use `cblas_sgemv` host-side loops.
transcribe.cpp builds the entire RNNT/TDT decode loop as a GPU ggml graph with
in-graph argmax — each step is a single GPU kernel launch, not a CPU→GPU round-trip.

**Revised fix priorities:**

1. **Port RNNT/TDT decoder to ggml graph (GPU)** [HIGH, LARGE]
   - Parakeet: 955ms→~50ms expected (19x)
   - Nemotron: 2900ms→~240ms expected (12x)
   - Build LSTM + joint as ggml ops, run on sched. Use in-graph argmax.
   - Files: `parakeet.cpp:1034-1139` (LSTM), `parakeet.cpp:1288-1530` (TDT loop)
   - Risk: moderate — LSTM in ggml is well-supported, joint is just matmul+relu+matmul

2. **Moonshine encoder: replace im2col conv with direct ggml_conv_1d** [MEDIUM]
   - 728ms→~60ms expected if conv path is optimized
   - The im2col on 176K raw samples creates massive intermediate tensors
   - Check if ggml_conv_1d op is GPU-supported as alternative to im2col+mul_mat
   - Alternative: pre-compute mel on CPU (like TC does) to reduce encoder input size

3. **Moonshine Streaming: accept gap or use non-streaming model** [LOW]
   - 3000ms is architectural (sliding-window attention required by model training)
   - Recommend `--backend moonshine` for offline, `--backend moonshine-streaming` only for live

### §232 Implementation progress (2026-07-10)

**Completed and verified:**
- [x] GGML_LLAMAFILE ON — neutral on Q4_K/x86, kept as default
- [x] Moonshine in-graph argmax — GPU-only benefit (4B vs 128KB readback)
- [x] Moonshine streaming mask bypass — FAILED, model requires masks
- [x] Parakeet pre-computed encoder projections — cache-friendly, ~0 CPU impact
- [x] Parakeet batched TDT decode — FAILED correctness (multi-emission/frame)
- [x] Kaggle v12 kernel — expanded to 11 models, fine-grained timing

**Key diagnostic finding: AR decoder is the universal GPU bottleneck.**
CrispASR's RNNT/TDT decoders (Parakeet, Nemotron) run on CPU via cblas_sgemv
while the GPU sits idle. transcribe.cpp runs its decoders on GPU.

| Model | CA decode (GPU run) | TC decode (GPU run) | Root cause |
|-------|-------------------|-------------------|------------|
| Parakeet TDT | 955ms (CPU cblas) | 51ms (GPU) | Host-side LSTM+joint |
| Nemotron RNNT | 2900ms (CPU cblas) | 238ms (GPU) | Host-side LSTM+joint |
| Moonshine Tiny | 87ms (GPU decode) | 76ms (GPU) | Near parity ✓ |

**Remaining work (needs GPU hardware + careful design):**

1. **Batched TDT decode (correct version)**: The prototype (`parakeet_tdt_decode_batched`,
   in-tree but unwired) batch-scans frames for blanks but doesn't handle TDT's inner
   loop (multi-token per frame, dur=0 retries). Correct design:
   - Process each frame's inner loop sequentially until blank+dur>0
   - THEN batch-verify remaining frames share the same blank pattern
   - transcribe.cpp's approach: build the full step as a GPU graph with static
     structure, avoiding the cblas→GPU migration entirely

2. **Nemotron RNNT batched decode**: Same pattern as Parakeet TDT but simpler
   (no duration logits). Port once TDT is working.

3. **Moonshine encoder gap (728ms vs 58ms)**: Separate from decoder.
   Needs GPU profiling — possibly im2col overhead on raw 176K-sample input,
   or the conv_stem's ggml_im2col creating oversized intermediates.

   **Update (2026-07-11): the im2col-size hypothesis doesn't hold for tiny.**
   Shape math for the 11 s clip (176 400 samples, hidden=288): conv1 K=127
   S=64 IC=1 → im2col [127, 2755] ≈ 1.4 MB; conv2 K=7 S=3 IC=288 →
   [2016, 917] ≈ 7.4 MB; conv3 K=3 S=2 IC=576 → [1728, 458] ≈ 3.2 MB.
   Whole stem ≈ 1.4 GMACs — ~1 ms of P100 F32 compute, nowhere near 670 ms.
   Look instead for: sched graph splits bouncing ops to CPU (group_norm /
   gelu_erf / cont-transpose support on the GPU backend), per-op sync from
   host readbacks, or the encoder graph being rebuilt/recomputed per call.
   `MOONSHINE_BENCH=1` gives per-stage ms; `CRISPASR_METAL_PROFILE` gives
   per-op on Metal (M1 profiling pending — box was saturated by the
   omnivoice bench campaign when this was written).

   **ROOT CAUSE FOUND (2026-07-11, M1): moonshine never ran on GPU from
   the CLI.** The adapter zero-initialized `moonshine_init_params` and
   never forwarded `params.use_gpu` (backend default: false). Every
   Kaggle "GPU" run of Moonshine Tiny AND Moonshine Streaming (v11-v13)
   was actually a CPU run — the 728 ms "GPU encoder" was the CPU
   encoder. Fixed for moonshine (adapter now forwards use_gpu): on M1
   Metal the encoder drops 1300 ms → 200-300 ms (4-6x), identical
   transcript. The tiny AR decoder is SLOWER on GPU (launch-bound;
   ~440-660 ms vs 66 ms CPU under load) — a hybrid GPU-encoder/CPU-
   decoder split may be the real winner for tiny; measure on a quiet
   box and re-run the P100 kernel (v14) for the moonshine row.
   moonshine-streaming deliberately stays CPU (550 launch-bound
   frame passes = 3.2x slower on GPU, measured) until Fix 2's batch
   encoder lands.

   **Same missing use_gpu forwarding in 4 more CLI adapters** (backends
   default false, so they are CPU-only from the CLI): bananamind_tts,
   f5_tts, m2m100, piper_tts. NOT flipped yet — CLI default use_gpu is
   true, so forwarding flips them GPU-by-default, and TTS GPU paths
   need per-backend validation (ASR-roundtrip) before that. paraformer /
   fastconformer_ctc / fastpitch have no use_gpu knob (not affected).

   **Validation results (2026-07-11, M1 Metal) — none should forward:**
   - **m2m100 / bananamind_tts**: their `use_gpu` param is DEAD — the
     backend sets `c->backend = c->backend_cpu` unconditionally
     (m2m100.cpp:961, bananamind has no consumer at all). Forwarding is
     a no-op (verified: identical 0.27s CPU vs 0.29s "GPU" translation).
     A real GPU port is separate backend work; the CLI adapter is fine.
   - **piper_tts**: GPU path is CORRECT (roundtrip-verified waveform)
     but 5x SLOWER (13.5s vs 2.7s) — tiny VITS graph is launch-bound,
     same class as moonshine-streaming. Deliberate-CPU comment added to
     the adapter.
   - **f5_tts: FIXED** (was two bugs, not "correct-but-slower"; the earlier
     verdict here was wrong). Root cause 1: the 22-layer DiT graph (the
     dominant compute, run 2× per ODE step for CFG) was hardcoded to
     `ggml_backend_graph_compute(ctx->backend_cpu, ...)` with the gallocr
     bound to backend_cpu's buffer type — so it ran on CPU regardless of
     use_gpu (on Metal it "worked" via unified-memory reads of the GPU
     weights, which is why sampling showed vec_dot_f16_f32 / flash-attn in
     libggml-cpu). Root cause 2: `pos_in` was set once in
     f5_dit_cache_build, but f5_dit_run re-allocs the gallocr every step and
     only re-set hidden_in/t_emb_in — gallocr aliased pos_in's slot with a
     prior step's intermediate, corrupting RoPE positions from step 1 on.
     This broke BOTH backends (the "f5 CPU=DUD" state); the earlier "same
     output character on CPU and GPU at 4 steps" was this shared corruption,
     not a step-count floor.
     Fix: compute the DiT on `ctx->backend` (single-backend gallocr, no
     sched; == backend_cpu when use_gpu off, so CPU compute is unchanged) and
     re-set pos_in every step. M1 Metal, 16 ODE steps, seed 42: GPU now
     roundtrips VERBATIM ("The quick brown fox jumps over the lazy dog"),
     RMS 1795→4178; ~7.8× faster per step than CPU (151 s GPU-16 vs ~1176 s
     CPU-16 extrapolated). Adapter now forwards use_gpu.
     Remaining headroom (follow-up, not blocking): the per-forward host-side
     input projection + conv_pos (`f5_linear`, 32× per synthesis) is now the
     relative bottleneck (~4.7 s / DiT forward); batched CFG and moving the
     input embedding into the GPU graph are the next levers.

   **f5 perf follow-up — MEASURED (this session, 2026-07-11, M1 Metal). The
   low-risk levers are exhausted; the base fix (7.8×) is the win.**
   Per-forward split (`F5_BENCH=1`, 8 steps): host_embed ~0.7 s/fwd,
   dit_graph ~1 s/fwd. Metal profile of the DiT: 979 nodes, host-encode
   ~1.5 ms vs GPU-execute ~1–2 s → **compute/bandwidth-bound, not
   dispatch-bound**, and ~12–15× off the F32-matmul ideal (F32 activations).
   1. **Batched CFG — DONE, MEASURED DUD** (f77b5351, opt-in `F5_BATCH_CFG`,
      OFF by default). B=2 cond+uncond in one graph; output corr 1.00000 vs
      sequential, roundtrip identical — so the 4D-RoPE + batched-flash-attn
      graph is correct — but NO speedup (slightly slower), because the DiT is
      compute-bound so B=2 just doubles per-dispatch work. Kept gated for
      reference / a future F16 DiT (where it could become dispatch-bound).
   2. **Host input-embed → GPU — NOT worth it as-is.** host_embed is already
      Accelerate-BLAS (`f5_linear` + grouped-conv `cblas_sgemm`); it's ~42% of
      time but real compute (grouped convs), and moving it to the GPU adds
      small-op nodes to an already compute-bound graph. Uncertain/negative.
   3. **DiT F16 activations — TRIED, MEASURED DUD.** Cast the QKV+FFN matmul
      inputs to F16 (weights already F16). Correct (corr 1.00000, verbatim
      roundtrip) but NO speedup — DiT graph slightly slower (added cast nodes).
      So the DiT is NOT matmul-bound. Reverted (no reuse value). Combined with
      the batched-CFG dud, the ~0.8-1 s/forward is dominated by the ~800 small
      ops + flash-attention over T=1238, NOT matmuls or dispatch overhead.
      **f5 GPU perf is now tapped out for cheap wins** — the base fix (7.8×) is
      the win. Any further gain needs true per-op GPU profiling (Metal capture /
      Instruments) to localize the flash-attn/small-op cost — not more
      guess-and-check. NOTE: the huge host_embed variance in F5_BENCH
      (8-58 s across runs) is CPU contention on the shared box, not inherent.
   **titanet/diarization GPU port — INVESTIGATED, NOT worth it.** The port IS
   tractable (the ggml graph computes on backend_cpu with weights uploaded
   there; all ops — conv_1d, conv_1d_dw, mul_mat, SE-block mean/sigmoid —
   decompose to Metal-supported im2col+matmul, same shape as the f5 base fix).
   BUT measured on M1: the DEFAULT path is legacy Accelerate-BLAS (macOS
   default `titanet_use_legacy()==true`), ~1 s/forward for 10 s audio — already
   optimized. The ggml-CPU path (`CRISPASR_TITANET_GGML=1`) is 1.7× SLOWER
   (14.96 s vs 8.65 s wall, 6 forwards). So a GPU-ggml port would have to beat
   Accelerate using a graph that's already slower on CPU, and per the f5
   findings GPU-ggml for conv/small-op graphs isn't dramatically faster. Poor
   expected value; not pursued. The real diarization lever, if needed, is
   FEWER forwards (batch/enlarge segments) or the clustering — not per-forward
   speed. Closes the §224 "diarize CPU perf" concern: it's Accelerate-BLAS'd.

### §232 v13 Results (2026-07-11)

**All models improved 8-14% vs v12** from LID skip (-l en) + in-graph argmax:

| Model | v12→v13 CA GPU | TC GPU | Status |
|-------|---------------|--------|--------|
| Whisper base | 0.025→**0.022** | 0.021 | **Parity** ✓ |
| SenseVoice | 0.018→**0.016** | 0.019 | **CA wins** ✓ |
| Canary 1B v2 | 0.048→**0.043** | 0.048 | **CA wins** ✓ |
| Qwen3-ASR | 0.094→**0.086** | 0.112 | **CA wins** ✓ |
| Parakeet TDT | 0.109→**0.095** | 0.029 | TC 3.3x (decode=828ms, was 955ms) |
| Moonshine Tiny | 0.080→**0.069** | 0.012 | TC 5.8x (encoder gap) |
| Moonshine Streaming | 0.278→**0.250** | 0.014 | TC 18x (architectural) |

**CrispASR wins or ties 5/7 tested models.** Kernel errored after 7 (Nemotron/Cohere/
FunASR/Whisper Large untested — likely OOM or download timeout from large merge).

Batched TDT decode (CRISPASR_TDT_BATCH=1): 828ms vs 955ms = 13% improvement.
Helps but doesn't close the 28x gap. The LSTM predictor step between emissions
remains sequential on CPU (cblas) while the GPU sits idle.

**Next steps to close remaining gaps:**
1. Full ggml-graph RNNT decoder (LSTM+joint+argmax as GPU ops) — the only
   path to matching TC's 29ms decode on Parakeet
2. Moonshine encoder: investigate ggml_conv_1d vs im2col+mul_mat on GPU
3. Fix v14 kernel stability (Nemotron/Cohere/FunASR need to complete)

### §232 ACTIVE SESSION (2026-07-11, worktree `moonshine-decode-stash`, M1)

**Status:** the CPU-pinned-decode / GPU-weight-copy sched class (LEARNINGS 25)
is now AUDITED ACROSS THE TREE and the clean fix is EXHAUSTED.

- **DONE + on main:** moonshine (offline) decode hybrid placement (q8 −40%,
  f16 −58%); moonshine encoder manual-attn (negative result, opt-in);
  moonshine_streaming hybrid placement (latent, correctness bonus). Parakeet
  TDT GPU decode IMPLEMENTED (opt-in PARAKEET_GGML_DECODE=1) + M1-validated
  (identical transcript); Kaggle P100 A/B kernel ready (see below).
- **Audited, does NOT apply (no more clean targets):**
  - `crispasr.cpp` (whisper): `cpu_buffer_type` sites are device enumeration /
    op-support, not a CPU KV cache; KV lives on the compute backend. No copy.
  - `dia_tts.cpp`: **CPU-only** (`ctx->backend = ctx->backend_cpu; // CPU-only
    for now`, line ~861) — ignores use_gpu, so no GPU weight copy. Fix is moot
    until dia gets a real GPU path (separate feature, not this class).
  - `f5_tts.cpp`: a *different, harder* class — the flow-matching DiT runs on
    CPU sched threads despite the Metal backend (needs a diff-harness session,
    not the weight split). Worktree `f5-gpu-stash` is another session's.
- **RNNT/TDT GPU decode: DONE + FLIPPED** (P100 5-12× — see below). §232's
  parakeet + nemotron decode losses are closed.
- **NEXT optimization targets (ranked), post-decode-flip:**
  1. **Extend the persistent decode to the beam/RNNT paths** (`parakeet_rnnt_
     decode`, `parakeet_tdt_beam_decode`, `*_maes_decode`, nemotron beam) — they
     still call the cblas `predictor_step`/`joint_step` beam_size× per step, so
     they're even more CPU-bound than greedy. Reuse `core_rnnt_ggml::Decoder`
     (one Decoder serves all hypotheses; state is passed per call). Proven
     pattern, low risk. Needs a parakeet-rnnt model to validate the RNNT path.
  2. **Moonshine-streaming (18× loss) — Fix-2 premise was WRONG, re-scoped
     2026-07-11.** The encoder is NOT 550 frame-by-frame passes: `run_encoder`
     is called ONCE (`moonshine_streaming.cpp:808`) and builds a single batched
     graph over all T_enc frames. The real bottleneck (per the in-code note,
     `:641`) is the **O(T²) sliding-window masked attention** —
     `ggml_flash_attn_ext(Q,K,V, mask_li, …)` with a dense T_enc×T_enc F16 mask
     per layer × 6 layers (~550²×6), computing full attention then masking to a
     ~20-wide window (wl=16/wr=4). The masks can't be dropped (LEARNING 17:
     degenerate output). So the fix is **banded/blocked windowed attention**
     (compute only the window, not full T²) — a hard, correctness-sensitive
     ggml/kernel change (no native banded flash in ggml), NOT a batch-encoder
     tweak. Larger than a quick win; own campaign. (The streaming hybrid-weight
     fix is already shipped, latent — it activates whenever streaming runs on
     GPU, independent of this.)
  3. **Re-run the P100 competitive scoreboard** — parakeet/nemotron TOTAL RTF vs
     transcribe.cpp should now be near-parity after the decode flip; update
     docs/performance.md. Measurement, not a code change.
  4. **In-graph argmax** for the transducer greedy path (2 int32 vs 8198-logit
     readback) — minor now that persistent won; only the greedy no-hotword path.
  5. **A real dia GPU path** — **DONE 2026-07-11 (M1 Metal), default flipped to
     GPU.** The blocker (main model in a plain CPU malloc ctx, not a backend
     buffer) was reworked onto `core_gguf::load_weights(ctx->backend)`; DAC
     already loaded on `ctx->backend` so it followed. Roundtrip-validated,
     default GPU on all backends incl. Metal (dia's 1.6B decoder wins on Metal
     too — LEARNING 34's Metal exclusion is for tiny transducers). `DIA_TTS_GPU`
     kept as the A/B gate (`=0` CPU, `=1` force GPU). See the dedicated subsection
     below. Kaggle CUDA A/B confirms the CUDA arm.

### §232 dia TTS GPU path (2026-07-11 — default GPU on Metal; CUDA opt-in pending re-validation)

dia was hard-pinned to CPU because the main model (encoder + 18L decoder + heads)
loaded into a **plain CPU malloc ggml context** via `gguf_init_from_file(...,
no_alloc=false)`. A GPU sched can't use those weights (not in a backend buffer —
"sched no longer auto-copies CPU-buffer tensors"). Everything else was already
backend-agnostic: all three graphs (encoder / cross-KV / per-step decode) run
through `ggml_backend_sched`, and self-attn KV flows through host vectors +
`ggml_backend_tensor_set/get` on sched-allocated graph inputs (`ctx->kv` is
inert in the hot path). So the only blocker was weight residency + the CPU pin.

**Change (`src/dia_tts.cpp`):** init the backend *before* loading and load the
main model via `core_gguf::load_weights(path, ctx->backend, "dia", wl)` (mirrors
the DAC path), binding the returned name→tensor map through the existing
`dia_assign_weight`. DAC already loaded onto `ctx->backend`, so it follows to
GPU. Free path now releases `buf_w` + the GPU backend.

**Default: GPU on Metal ONLY** (not `--no-gpu`); CUDA/Vulkan fall back to CPU
unless forced. `DIA_TTS_GPU` gate: `=1` forces GPU on any backend (the CUDA/Vulkan
opt-in), `=0` forces CPU (regression bisection). Why Metal-only: the Kaggle P100
A/B (below) caught the CUDA decoder aborting; Metal is roundtrip-validated.

**A/B (M1, dia-1.6b-q4_k, 384 steps, seed 42, single run — indicative):**

| stage | CPU | Metal GPU | speedup |
|-------|-----|-----------|---------|
| encoder | 13 655 ms | 145 ms | ~94× |
| cross_attn_kv | 4 330 ms | 43 ms | ~101× |
| decoder_ar | 116 311 ms | 77 334 ms | ~1.5× |
| dac_decode | 20 474 ms | 4 221 ms | ~4.9× |

Correctness: step-0 logits match to ~4 decimals (argmax=568 both, == Python
ref); generate→ASR roundtrip (whisper-tiny) is **identical** on both arms ("The
quick brown fox jumps over the lazy dog while the sun sets slowly behind..").
Metal decode is *faster* here, not slower — dia's 1.6B decoder does large
matmuls (unlike LEARNING 34's tiny parakeet transducer), so the launch-bound
regime doesn't apply and Metal is NOT excluded.

**Kaggle P100 CUDA A/B (`f0872174`) — caught a CUDA-only decoder abort.** Encoder
(1629→79 ms, 20.7×) and cross-KV (561→102 ms) ran fine on CUDA, but the AR
decoder produced **0 tokens** and aborted:
`GGML_ASSERT(src1->nb[0] == ggml_type_size(src1->type)) failed`. Root cause:
`build_dia_decoder_embedding` fed `ggml_get_rows` a **strided index view**
(`view->nb[0] = n_output_heads*elsize`); CPU/Metal tolerate the stride but CUDA's
`get_rows` kernel requires a contiguous index tensor. **Fixed** by materialising
the 2 indices with `ggml_cont` before `get_rows` (negligible cost, correct on
every backend). Because that fix is untestable from an M1, the **default was
narrowed to Metal-only** and CUDA/Vulkan kept opt-in (`DIA_TTS_GPU=1`) until a
Kaggle re-run confirms the fix. This is the A/B process (rule #4/#5) doing its job
— the mandated CUDA A/B caught a flip that would have shipped empty audio on CUDA.
No k-quant CAST / left-pad PAD issues on the DAC (F16/F32 codec weights).
`load_weights_split` (encoder+DAC→GPU, decoder→CPU) remains the fallback if a
platform shows the decoder losing on GPU.

**Kaggle P100 v2 re-run (`65a5d30c`, get_rows fix): crash GONE — decoder runs on
CUDA (256 tokens, total 2.33×, encoder 23×, DAC 49×, decode 1.9×). BUT under
greedy the CUDA tokens DIVERGE from CPU at step 0** ("CORRECTNESS FAIL" by
token-parity), whereas M1 Metal step-0 matched CPU exactly (argmax 568). The dia
kernel only checks token identity, not decoded audio — a step-0 greedy divergence
on CUDA is either benign FP (CUDA matmul reduction order flipping a close argmax;
dia's step-0 logits are tightly spaced) or a second CUDA miscompute. **Unresolved
→ dia default stays Metal-only; CUDA/Vulkan opt-in (`DIA_TTS_GPU=1`).** Next: ASR-
roundtrip the CUDA-generated audio (the real HARD-RULE-#3 test) — if intelligible,
it's benign FP and the default can widen; if garbled, bisect the decode graph for
another non-contiguous/precision-sensitive op. Contrast: the 3 MT/ASR backends
compared *decoded output* on CUDA and were identical, so they flipped cleanly;
dia's token-level check is stricter and flagged this.

### §232 paraformer GPU path — implemented, OPT-IN (kept CPU default, 2026-07-11)

Audit follow-up to dia: paraformer was also CPU-pinned (`ctx->backend =
ggml_backend_cpu_init()` hardcoded, 1-backend sched, no `use_gpu` param) despite
already loading weights via `core_gguf::load_weights`. Added a `use_gpu` param +
`backend_cpu` member, init-before-load GPU selection (`crispasr_init_gpu_backend`),
a 2-backend sched, and CLI/c_api wiring — same shape as dia. Gated
`CRISPASR_PARAFORMER_GPU=1`.

**Validated (M1, paraformer-zh-q4_k, paraformer_zh.wav 13 s):** transcript
**identical** CPU vs Metal (correctness PASS). Timing inconclusive: CPU median
~0.85 s (15–17× RT) vs GPU median ~0.77 s (17–18×) with a 1.32 s GPU outlier —
roughly neutral. Expected: it's a small model (~123 MB) on short audio, so it
sits in LEARNING 34's launch-bound / overhead-dominated regime (unlike dia's
1.6B decoder). M1 kept opt-in; the **Kaggle P100 A/B then confirmed the win
(identical transcript, 2.15× — LEARNING 30's slow-OpenBLAS regime), so the
default flipped to GPU on CUDA/Vulkan, CPU on Metal** (see the m2m100/t5 section
below for the shared flip + gate).

### §232 m2m100 + t5/madlad GPU paths — implemented, OPT-IN (2026-07-11)

Same audit follow-up: both MT backends were CPU-pinned (`c->backend =
c->backend_cpu`) but already loaded weights AND KV onto `c->backend` via
`core_gguf::load_weights` + `ggml_backend_alloc_ctx_tensors`, so pointing that at
a GPU backend + a 2-backend sched is the whole change. Gated `CRISPASR_M2M100_GPU`
/ `CRISPASR_T5_GPU` (pure env opt-in; default CPU). Validated on M1 Metal:
en→de translation **identical** CPU vs GPU (m2m100-418m-q4_k: "Der schnelle braune
Fuchs…"; madlad-3b-q4_k: "Der schnelle Braunfuchs…"), GPU engaged on both.
Timing not chased on M1 (encoder-decoder AR, small/short — launch-bound like
paraformer). **Kaggle P100 A/B (`tools/kaggle/gpu-pin-ab`, `65a5d30c`) settled it:
output IDENTICAL cpu vs gpu on CUDA for all three; wall speedups paraformer 2.15×,
m2m100 1.24×, madlad 2.13×** (slow OpenBLAS baseline reveals the encoder win M1's
Accelerate hides — LEARNING 30). No strided-get_rows abort (MT embeddings are
contiguous). So the **default flipped to GPU on CUDA/Vulkan** for all three
(rule #3: correct + faster on the measured platform), CPU on Metal (neutral;
`ggml_backend_is_metal` gate). Env still forces:
`CRISPASR_{PARAFORMER,M2M100,T5}_GPU=1` → GPU any backend, `=0` → CPU. m2m100/t5
`use_gpu` default now `true` (gate decides actual use); m2m100 c_api wired to
`g_open_use_gpu_tls`.

### §232 Moonshine decode — hybrid weight placement (DONE, 2026-07-11, M1 Metal)

**Superseded Fix 1's premise.** Profiled moonshine-tiny on a *quiet* M1 (the
plan's 440-660 ms/decode numbers were a contended box × the copy tax below).
At idle the decode is only ~1 ms/token, so the CP_DIRECT-style static-graph
rewrite (Fix 1) would save almost nothing — same conclusion as the qwen3
talker (§232): sched-free dispatch doesn't help when you're not launch-bound.
The real waste was elsewhere.

**Root cause (`GGML_SCHED_DEBUG=2`):** moonshine's self-attn KV cache is a CPU
buffer, so the sched runs the *entire* decode step on CPU even in GPU mode
(encoder + cross-KV stay on Metal). With the all-GPU weight load the decoder
weights sit in the Metal buffer, so the sched re-copies them GPU→CPU on *every*
per-token graph rebuild (the copy can't cache — fresh graph each step). The
overhead scales with weight size, which is why f16 (2× q8) hurt most.

**Fix (default ON):** `load_weights_split` routes `encoder.*`→GPU,
`decoder.*`→CPU, so the CPU-pinned decode is weight-local (no copy). Encoder
unchanged on GPU. `MOONSHINE_ALL_GPU=1` restores the legacy load for A/B.
`src/moonshine.cpp` (`moonshine_is_gpu_tensor` + split load in
`moonshine_init_with_params`), `src/moonshine-impl.h` (`buf_w_cpu`).

**Measured (quiet M1, jfk, 26 tok, 3 reps, bit-identical transcript on
greedy + beam-4 + multispeaker, q8 & f16):**

| quant | decode all-GPU | decode hybrid | Δ |
|-------|---------------|--------------|---|
| q8_0  | 39-48 ms      | 21-25 ms     | **−40%** |
| f16   | 112-119 ms    | 45-55 ms     | **−55%** |

Encoder unchanged (~65 ms). `--no-gpu` path untouched. This is the same
CPU-resident-leaf/weights sched class flagged for f5_tts above (§232 GPU-
forwarding validation) — the general remedy is to co-locate a CPU-pinned
graph's weights on CPU. Remaining moonshine-tiny cost is now the encoder
(GPU 67 vs CPU 47 ms — per-layer flash-attn Metal↔CPU permute bounce).
See LEARNINGS 25-26.

### §232 Moonshine encoder bounce — manual attention is SLOWER (opt-in, 2026-07-11)

Investigated the encoder Metal↔CPU bounce: `GGML_SCHED_DEBUG=2` confirms
`FLASH_ATTN` is the *only* encoder op on CPU (head_dim=36 has no Metal flash
kernel) — every other op is on Metal, so each of the 6 layers bounces
MTL→CPU→MTL. Replaced flash with manual `mul_mat + soft_max_ext + mul_mat`
(Metal-supported at any head_dim, MHA so no GQA) to keep the encoder fully
on-backend. **Measured ~40% SLOWER on M1** (enc 162 vs 114 ms, 3 reps,
transcript identical) — the T² scores + 3 conts spawn more small Metal
kernels than the cheap 514 KB bounce copies cost. Flash stays default on
both backends; manual is opt-in (`MOONSHINE_ENC_ATTN=manual`) for CUDA/
Vulkan/base re-test where the tradeoff may differ. `src/moonshine.cpp`
(`moonshine_build_encoder` `manual_attn` param). See LEARNINGS 27. Net: the
encoder GPU>CPU gap for tiny is inherent Metal launch overhead, not a
fixable bounce — for moonshine-tiny, `--no-gpu` remains fastest at idle.

### §232 RNNT/TDT GPU decode — DONE, DEFAULT FLIPPED (P100 5-12× faster, 2026-07-11)

**GAP CLOSED.** Kaggle P100 (sm_60) A/B, both transcript-IDENTICAL: parakeet
decode **763.5 → 145.2 ms (5.26×)**, nemotron **2589.3 → 209.1 ms (12.38×)** —
the persistent-graph ggml decode replaces the host cblas that left the GPU idle.
Default flipped: ggml decode when the decode backend is a **CUDA/Vulkan** GPU,
cblas on **Metal + CPU** (`!ggml_backend_is_cpu` AND not
`ggml_backend_is_metal`). Metal is excluded because Apple Accelerate cblas beats
the GPU decode there — the P100 win is a slow-OpenBLAS effect, NOT universal (M1
parakeet total ~16× cblas vs ~11× ggml; LEARNING 34). Override
`PARAKEET/NEMOTRON_GGML_DECODE=1/0`; `RNNT_GGML_PERSTEP` = per-step path.
Applies to all parakeet + nemotron decode paths (greedy/beam/maes/rnnt).
See LEARNINGS 33-34. Original notes below.


**Shipped (opt-in, `PARAKEET_GGML_DECODE=1`, commit on main):** the Parakeet TDT
predictor LSTM + joint head now run as ggml graphs on `ctx->backend`
(`parakeet_predictor_step_ggml` / `parakeet_joint_step_ggml` /
`parakeet_lstm_layer_ggml` in `parakeet.cpp`), so the whole per-step decode
executes on the GPU instead of host cblas_sgemv. Default stays cblas.

- **CORRECTNESS (M1, done):** transcript IDENTICAL to cblas on jfk +
  multispeaker, CPU & Metal. Hand-written LSTM (gate order i,f,g,o) + ReLU joint
  match the cblas math exactly.
- **M1 perf (done):** decode 55-59 ms (ggml) vs 60-61 ms (cblas) on Metal —
  neutral/slightly faster. No local win because Apple Accelerate cblas is
  already fast (~60 ms); the P100 gap (955 ms cblas) is a slow-CPU-BLAS effect,
  so the win is expected on P100, not M1. `PARAKEET_DECODE_TIMING=1` prints
  decode ms.
- **PERF VERDICT — run this Kaggle kernel next:**
  `tools/kaggle/parakeet-ggml-decode-ab/` (P100, GPU-enabled, clones the feat
  branch, builds CUDA, A/Bs cblas vs ggml decode ms + transcript parity + RTF,
  N reps). `bash tools/kaggle/parakeet-ggml-decode-ab/push.sh`. Flip the default
  only if parity holds AND ggml decode < cblas decode there.
- **Nemotron wired too (shared module).** The parakeet helpers were extracted to
  `core/rnnt_ggml.h` (DRY) and nemotron_rnnt_decode now uses the same code behind
  `NEMOTRON_GGML_DECODE=1` — nemotron had the larger gap (2900 ms P100). M1 A/B
  (both transcript-identical): parakeet ggml ~57 ms vs cblas ~60 ms (neutral);
  **nemotron ggml ~1613 ms vs cblas ~755 ms — 2× SLOWER** (LEARNINGS 31: per-step
  overhead × many steps). Both gated, default cblas. The Kaggle kernel now A/Bs
  BOTH backends in one P100 run.
- **Persistent-graph decoder DONE — fixes the regression + WINS on M1
  (LEARNINGS 32).** `core_rnnt_ggml::Decoder` builds the predictor + joint graphs
  once, gallocr-allocates once, dispatches sched-free per step. M1 Metal (jfk,
  transcript-identical, `RNNT_GGML_PERSTEP` to A/B): nemotron decode cblas
  ~510-806 ms | **persistent ~261-307 ms** | per-step ~318-421 ms → persistent
  ~2× faster than cblas. parakeet within noise (short decode). Now the default
  ggml path for both backends (per-step is the RNNT_GGML_PERSTEP fallback).
- **Remaining follow-ups:** (1) clean P100 bench (`tools/kaggle/
  parakeet-ggml-decode-ab/`, now exercises the persistent path) → if it wins,
  FLIP the default to persistent-ggml-when-GPU (cblas stays on CPU, where
  Accelerate beats ggml). (2) optional in-graph argmax (2 int32 readback vs
  8198-logit) for the greedy no-hotword path — minor vs the persistent win.

--- original scoping notes (kept for reference) ---

Scoped the Parakeet TDT decode port (target: TC's 29 ms P100 decode vs CA's
~828-955 ms cblas). Findings that must guide the implementation:

**Where/what.** Single hot function `parakeet_tdt_decode` (`parakeet.cpp:1288`;
the same gate must be mirrored into `_rnnt_decode`, and the four transcribe
entry points already funnel through these). Exact per-step math to replicate
(read line-by-line — the header comment is STALE):
- predictor: `embed[tok]` → 2× PyTorch-LSTM (gate order i,f,g,o; `c=σ(f)·c +
  σ(i)·tanh(g)`, `h=σ(o)·tanh(c)`) → `pred_out=h1`. Weights `lstm{0,1}.w_ih
  [4H,H] w_hh[4H,H] b_ih/b_hh[4H]`, H=640. Reuse `core_lstm::lstm_unidir`
  (already ggml, matching gate math) adapted to a single carried-state step.
- joint: `mid = pred_w[640,640]@pred_u + pred_b`; **`relu`**(proj_e + mid)
  (NOT tanh — code at 1135 is relu); `logits = out_w[8198,640]@mid + out_b`.
  proj_e is precomputed per frame (batched sgemm, §232). Token argmax over
  `[0,n_vocab_blk)`, duration argmax over the last `n_dur=5` — do BOTH
  in-graph (2 int32 readbacks) ONLY on the greedy no-hotword no-sampling path;
  hotword-bias / temperature need the full 8198-logit readback.

**Go/no-go — do NOT ship a per-step ggml dispatch without P100 proof.**
Strong evidence it is launch-bound and LOSES: LEARNINGS 20/24 (batched GPU
joint measured **8.8× WORSE** — CPU sgemm while GPU idle) and 25-26 + the
qwen3 talker (§232): per-step GPU dispatch of small (≤8198×640) matmuls is
launch-bound on Metal and often slower than CPU cblas. TC's 29 ms almost
certainly comes from **CUDA-graph capture** (whole decode as one replayable
graph), not per-step ggml dispatch. M1 CANNOT measure the win (it is P100-
only), so the port must be built + A/B'd on Kaggle:
1. Implement gated (`PARAKEET_GGML_DECODE=1`) LSTM+joint+in-graph-argmax as a
   persistent gallocr graph on `ctx->backend` (CP_DIRECT pattern), state
   carried via tensor_set/get of h0/c0/h1/c1 (640 f32 each).
2. Validate transcript/WER parity on M1 (CPU + Metal) vs the cblas path first
   (correctness is HW-independent) — this half IS doable off-Kaggle.
3. Measure RTF on Kaggle P100 (kaggle_harness). Flip default only if it beats
   cblas there. If launch-bound (likely), the real fix is CUDA-graph capture
   of the whole step loop — a larger ggml-cuda effort, own campaign.

Model local: `parakeet-tdt-0.6b-v3-q4_k.gguf` (467 MB). Baseline M1: Metal
6.1× RT, CPU 1.1× RT — parakeet is already encoder-bound and fast on Metal;
the decode gap is a CUDA-vs-CUDA competitiveness issue, not an M1 UX issue.

### §232 v15 Final Benchmark (2026-07-11) + GPU-forwarding audit

**v15 results (batched decode DISABLED — v14 proved 5-9x worse on GPU):**

| Model | CA GPU | TC GPU | Result |
|-------|--------|--------|--------|
| SenseVoice Small | **0.018** | 0.022 | CA wins |
| Canary 1B v2 | **0.044** | 0.048 | CA wins |
| FunASR Nano 2512 | **0.045** | 0.139 | CA wins 3x |
| Cohere Transcribe | **0.046** | FAIL | CA (TC crashed) |
| Qwen3-ASR 0.6B | **0.085** | 0.114 | CA wins |
| Whisper base | 0.026 | **0.021** | TC 1.2x |
| Whisper Large v3 Turbo | 0.060 | **0.050** | TC 1.2x |
| Moonshine Tiny | 0.059 | **0.012** | TC 4.9x (WAS CPU — see below) |
| Parakeet TDT 0.6B | 0.100 | **0.037** | TC 2.7x (CPU cblas decode) |
| Moonshine Streaming | 0.244 | **0.014** | TC 17x (architectural) |
| Nemotron 3.5 ASR 0.6B | 0.345 | **0.042** | TC 8.2x (CPU cblas decode) |

Score: CA 5 — TC 6.

**v16 (running):** moonshine GPU-forwarding fix (`d46839ca`). The moonshine CLI
adapter never set `use_gpu` — it ran CPU on every platform including the Kaggle
"GPU" benchmarks. On M1 Metal: encoder 1300ms→200-300ms (4-6x). Expected P100:
CA ~0.012-0.015, matching TC. If confirmed: **CA 6 — TC 5** (CrispASR leads).

**GPU-forwarding audit (all CLI adapters):**

| Backend | use_gpu? | Impact |
|---------|----------|--------|
| moonshine | FIXED (`d46839ca`) | Was the "encoder gap" |
| moonshine-streaming | Intentionally CPU | GPU is 3.2x SLOWER (launch-bound) |
| fastconformer_ctc | Auto-GPU (`crispasr_init_gpu_backend`) | No fix needed |
| paraformer | No GPU in params | CPU-only by design (NAR, fast) |
| bananamind_tts | Missing | TTS, low priority |
| f5_tts | FIXED (`da082f2e`) | Was CPU-only, now GPU |
| fastpitch | Missing | TTS, tiny model |
| m2m100 | Missing | Translation, low priority |

**Remaining optimisations (all need Kaggle P100 validation):**
1. RNNT/TDT ggml-graph decode (§232 scoped above) — CUDA-graph capture likely
   required; per-step dispatch is launch-bound
2. PR #246 (wav2vec2 group-norm) — merged, enables wav2vec2-base-960h

**Decision: no more optimisations on VPS.** All remaining wins require GPU
hardware (Kaggle) for both validation and measurement. Run v16 kernel, confirm
moonshine GPU fix, then the §232 transcribe.cpp eval is DONE pending the
RNNT ggml-decode campaign (separate issue, not blocking).

## Runtime speedup roadmap (2026-07-11 cross-repo sweep)

Source: full ASR + TTS + codec + pipeline re-verification, 2026-07-11 — see
`PERFORMANCE.md → "Runtime Optimization Audit — Re-verification (2026-07-11)"`
for verified state + corrected stale claims. **Every item needs a target GGUF
model (q8_0 preferred, to isolate from q4_k quant noise) + before/after parity +
latency; do NOT land a perf change on a compile-only check.** Key correction:
the per-model TTS "flash not wired" claims are mostly false — flash reaches
Orpheus/OuteTTS/Zonos/TADA/Chatterbox/CSM via the shared
`core_attn::kv_self_attn` (`src/core/attention.h:665,903`, unconditional
`ggml_flash_attn_ext`). Real flash gaps are only the manual-`soft_max` backends
(dia, speecht5, parler) and the structurally-can't-flash relpos models (melotts,
piper).

### Maps onto existing tracked items (don't duplicate)
- **Decode-step graph cache for remaining LLM/AR backends** → this IS §210
  follow-up (shape-stable bucketed decode / CUDA-graph capture). Templates:
  qwen3-tts Lk-bucket, granite §210 gallocr, mimo `step_t1_gf`.
- **Batched-CFG (B=2) for remaining TTS** → §215. Un-migrated diffusion/DiT
  targets: f5, dots, kugelaudio, pocket (+ dia/speecht5/parler once they get
  device KV). Respect the Metal quant-B=2 gotcha (dequant batched-against
  weights q*→F16 once) and item-24 (don't CPU-batch a GPU pipeline).
- **gallocr cross-call UAF audit** → #215e / the encoder-graph-cache removal
  (#235). Encoder-graph caching stays OFF (measured dud + GPU UAF).

### Shared cross-repo Tier-1 levers (coordinate with CrispEmbed PLAN.md)
- **Decode-step graph cache** — same design as CrispEmbed Tier-1 #1; CrispASR is
  further along (§210 has the CUDA-graph-capture template).
- **ggml-metal ICB replay** — the Apple-side equivalent of §210's CUDA-graph
  capture; ggml-metal has no ICB path. Depends on a stable per-step graph.
  Shared ggml submodule — do it once, both repos benefit.

### Genuinely-new gaps (not yet tracked)
| P | Area | Gap | File |
|---|---|---|---|
| ~~P0~~ **STALE** | firered_asr | ~~Decoder self-attn has no KV cache~~ — **incorrect**: the greedy decode already caches self-attn K/V (`beam_hyp::sa_k/sa_v`, appended per step at `firered_asr.cpp` ~L2205-2208; past tokens are never re-projected). The O(T²) that remains is the inherent attention *scoring* over history, run as scalar-CPU triple-loops + one-at-a-time `ggml_vecmat` — a scalar→ggml-graph decoder rewrite, not a missing cache, and the §235 AR-decoder verdicts show those often lose on M1/CPU (launch-bound). Not the cheap win the row implied. | `firered_asr.cpp` |
| P0 | melotts / piper | Scalar O(H·T²·D) relpos attention (can't flash — additive bias); HiFi-GAN 17.9s of 26.3s VPS total. Needs manual-attn ggml graph or BLAS | melotts.cpp, piper_tts.cpp |
| P0 | voxcpm2_tts | CPU-only (Metal SIGSEGV); manual per-step host KV re-upload | `voxcpm2_tts.cpp:106-111` |
| P0 | openvoice2 | 16-layer WaveNet + ref-encoder Conv2d/GRU scalar CPU | openvoice2.cpp |
| P1 | voxtral/voxtral4b enc, mimo LLM dec | Attention not on flash_attn_ext (manual soft_max) | voxtral4b.cpp, mimo_asr.cpp |
| P1 | firered/glm/funasr/qwen3/omniasr/mimo | Beam = replay; add KV snapshot pool (canary/moonshine/kyutai template) | — |
| ~~P2~~ **DONE** | align_wav2vec2_ctc | ~~Reloads 300MB–1GB model every call~~ — **wav2vec2 now joins the §176e resident cache** (qwen3-FA / canary-ctc were already cached; wav2vec2 was the lone gap, stubbed `AlignerType::Wav2Vec2` no-op). Heap `wav2vec2_model*` keyed by path, freed via `delete` in `crispasr_aligner_free_cache()` (Metal-safe order). M1 A/B (`wav2vec2-xlsr-en-q4_k`, jfk, `test-align-only`): call1 load+align **13.8 s → call2 cached 0.92 s (~15×)**, word timings byte-identical. | `crispasr_aligner.cpp` |
| P2 | scalar CPU hotpaths | RNN-T LSTM pred+joint; granite cpu_linear+depthwise; rvq encode; istft IRFFT; titanet mel; diarize `apply_xcorr` | core/rvq.cpp, core/istft.h, titanet.cpp:740 |
| P2 | parakeet/nemotron | Batched sgemm decode opt-in default-OFF — validate + flip on | `CRISPASR_TDT_BATCH` |
| P3 | threading | Hardcoded default-4 threads in ~90 sites; adopt whisper-core's `min(4, hw)` | crispasr_c_api.cpp |
| P3 | pyannote | Runs per-slice not once-over-audio (#107); RNNoise recreates state+resamplers/call | crispasr_diarize.cpp |

## §244 — Irodori-TTS VoiceDesign (caption conditioning)

**Status: DONE** (2026-07-11)

Ported Aratako/Irodori-TTS-600M-v3-VoiceDesign to the irodori-tts backend.
Adds a caption encoder (TextEncoder, 10L/512d) for style/emotion text
conditioning via `--instruct`. Three-way independent CFG (text/speaker/caption).

- Converter: caption encoder + DiT caption KV + dual-adarn-zero duration
- Runtime: caption encoder graph, JointAttention caption KV, DurationPredictor
  caption modulation, Euler ODE caption CFG
- Diff harness: F16 cos=1.000000; Q4_K cos=0.996491 (526 MB)
- Registry entry added; docs/tts.md updated
- GGUF artifacts: irodori-tts-600m-v3-voicedesign-{f16,q4_k}.gguf

## §246 issue #81 endgame — close the remaining ~1.4× CUDA gap to onnx-asr (OPEN)

Current: CrispASR parakeet-ctc q8_0 CUDA manual-attn = 153× RT warm
(jfk×5 55 s, in-process) vs onnx-asr CUDA fp32 = 207× (134 s varied).
Where the remaining ~0.36 s/55 s plausibly goes, ranked by expected value —
**measure before building (LEARNING: the handover's bottleneck theory was
wrong; the profiler wasn't):**

1. **Per-stage split on CUDA first** (cheap, decides everything below):
   extend `tools/kaggle/fc-unified-graph-ab` to run with
   `CANARY_CTC_BENCH=1` + `CRISPASR_FC_PROFILE=1` on the P100 — mel vs
   encoder+ctc vs readout. The mel is host-side single-threaded FFT
   (`cc_fft_r2c`); at 55-134 s it may be a triple-digit-ms constant that
   onnx doesn't pay. If so: parallelize `core_mel` (an unused
   `mel_parallel` flag already sits in mel.cpp) or overlap mel with the
   previous graph's compute.
2. **F16 vs Q8_0 on GPU**: P100 has 2:1 fp16 rate and no tensor cores;
   onnx runs fp32 cuBLAS. Our q8_0 mmq may be losing to plain f16/f32
   GEMM at these shapes — one kernel arm with the F16 GGUF answers it.
3. **CUDA-graph replay**: verify whether ggml-cuda's graph capture
   actually engages across our rebuild-per-call graphs (same topology →
   it should). If not, the `CRISPASR_FC_BUCKET` path provides the stable
   topology; retry with small buckets (100 mel frames ≈ 1 s — the 500
   bucket's pad overhead ≈ savings, smaller pads waste less).
4. **Upstream flash fix (structural)**: teach `fattn.cu` to accept
   per-head masks (`mask->ne[2] != 1` guard) so flash works for Shaw
   rel-pos models on CUDA — reclaims fused-attention memory traffic that
   manual attention re-materializes ((T,T,H) scores ×24). Upstream PR to
   ggml-org/llama.cpp per repo convention (mechanical-AI disclosure only).
5. **Honest re-run**: after any of the above, the canonical number is the
   134 s-varied load-excluded methodology (issue81-onnx-bench), not jfk×5.

Also OPEN: VPS 4-core x86 re-bench with the shipped defaults (pre-fix
2.1× vs onnx-CPU 3.1×; handover synced to the VPS at
handover-prompts/issue81-fc-perf.md).

## §247 roll #81 techniques out to the other runtimes (OPEN)

What shipped for the FastConformer family (fastconformer.h consumers:
parakeet, canary, canary_ctc, canary_qwen, lfm2_audio, nemotron) and
where else each technique applies:

1. **F16-weights-in-quantized-GGUF audit** (the 35%-of-encoder trap):
   any backend whose converter stores matmul-consumed weights 3D/1D gets
   them silently skipped by the quantizer's 2D rule and runs them on the
   ~6×-slower CPU F16 mul_mat path. Method: for each backend's shipped
   q8_0/q4_k GGUF, list tensors with `GGUFReader` and flag F16 tensors
   that feed `mul_mat` (reshape-consumed); or just run the per-node
   profiler and look for `MUL_MAT f16` rows at high %. Prime suspects:
   cohere-transcribe (Conformer convs), firered-asr (Conformer),
   granite/conformer_ibm, sanm/paraformer conv layers, moonshine,
   TTS vocoders (hifigan/seanet/dac k=1 convs — qwen3-tts FASTCONV
   already fixed the cast side, not the storage side). Fix pattern:
   quantizer carve-out (+ Q8_0 floor + idempotency rule) + load-time
   repack via `core_conformer::repack_conv_pw_q8`-style helper +
   fleet requant kernel (`tools/kaggle/fc-pw-requant` — FLEET list +
   structural check + strict transcript equality are generic).
2. **Generalize the per-node profiler**: `cc_prof_cb` (sched eval
   callback, aggregates by op+src-type+shape, CRISPASR_FC_PROFILE=1)
   should move to `src/core/sched_prof.h` so every sched-based runtime
   gets it — it found in one run what three A/B rounds missed.
3. **Fused QKV** (`core_conformer::fuse_qkv` is tensor-generic):
   bit-identical, ~free win wherever Q/K/V matmuls share an input —
   whisper-family encoders, cohere, firered, granite, sanm, decoder
   self-attention in AED backends. Wire-up is load-time concat +
   view-split at the build site.
4. **Strided flash inputs**: grep `ggml_cont` feeding `flash_attn_ext`
   across src/ — the kernel reads strided views (llama.cpp does this);
   each cont is a full tensor copy per layer per pass.
5. **Manual-attn-on-CUDA gate**: any backend whose flash mask has
   ne[2] > 1 (per-head bias) silently falls back to CPU on CUDA —
   `GGML_SCHED_DEBUG=2` on a CUDA box shows flash nodes on CPU splits.
   Check cohere-transcribe (Shaw rel-pos) first. Reuse
   `fc_gpu_manual_attn` + BlockParams.manual_attn pattern.
6. **-inf pad masking + bucketed persistent graphs**
   (CRISPASR_FC_BUCKET machinery): the reusable base for batched
   inference and CUDA-graph capture; also the correct padding semantics
   for any future streaming/batching work (finite mask constants get
   overrun by pad garbage — LEARNINGS 2026-07-12).
7. **Q8_0 floor for decode-critical tensors in sub-8-bit quants**
   (quantizer): conv pw done; consider the same floor for other
   high-sensitivity small tensors flagged by future A/Bs.

Suggested order: (2) profiler generalization → (1) audit sweep with it
(one Kaggle CPU kernel over the registry models, collect `MUL_MAT f16` %
per backend) → fix the top offenders with the proven pattern → (4)/(3)
mechanical wins alongside → (5) after the §246 CUDA per-stage data.
