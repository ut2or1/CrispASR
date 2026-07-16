# CrispASR — cross-cutting improvements program

Five high-leverage improvements surfaced by the #257 marathon. Every runtime
change ships **behind an env gate** (A/B without recompile, never delete the
working path — per the dev-guide), with an **A/B method** and **unit tests**.

## NOW — active work

- [x] **Phase 0** — cross-surface parity harness + test (safety net) — DONE
      (`src/core/asr_parity.h` + `test-asr-parity` 13 assertions; live
      `test-surface-parity.sh` PASS: CLI==session on parakeet-tdt-1.1b/jfk)
- [x] **Phase 1** — collapse the dual dispatch — DONE for **parakeet** (pilot).
      Orchestration hoisted to `src/parakeet_orchestrate.{h,cpp}`
      (`parakeet_transcribe_segments`); the CLI adapter is now a thin wrapper
      (**−310 LOC**, 544→234) and the session C-ABI calls the same code under
      `CRISPASR_SESSION_UNIFIED_DISPATCH=1`. Pure `parakeet_pick_strategy`
      unit-tested (`test-parakeet-strategy`, 11 assertions). A/B: CLI
      byte-identical pre/post refactor; parity harness PASS gate-ON on jfk (short)
      AND the 225 s single-pass clip (where the old inline session diverged).
      **Remaining backends** (canary/cohere/granite/…) follow the same recipe —
      tracked below.
- [x] **Phase 1b** — audited, no further hoist warranted (evidence-driven). Rather
      than speculatively hoist every backend, generalized the parity harness
      (`CRISPASR_PARITY_BACKEND` + a CONTENT check that ignores punctuation/case)
      and ran it CLI-vs-session on the locally-available backends. Result on jfk
      (16 kHz, no resample artifact): **parakeet PASS** (unified); **qwen3,
      moonshine, nemotron all PASS(content)** — CLI and session produce identical
      transcriptions, differing only by punctuation (the harness's
      `--no-punctuation` CLI flag has no session equivalent — cosmetic, not a
      dispatch bug). So the #257-class divergence was parakeet-specific (its
      CAP_INTERNAL_CHUNKING long-audio path); the other backends' simpler session
      paths already agree with their adapters. **No further unification needed
      now.** ⚠ Caveat: audited on SHORT audio only; parakeet's divergence surfaced
      on LONG/chunked audio, so a long-clip audit of any backend with a bespoke
      long-audio session path (qwen3's ~212-line block) is the remaining check —
      the generalized harness makes it a one-liner. The parakeet gate
      (`CRISPASR_SESSION_UNIFIED_DISPATCH`) stays opt-in.

      **Long-audio audit run (2026-07-16).** Ran the harness on a 225 s / 60 s
      clip. Finding: on long audio CLI and session DO diverge — e.g. moonshine
      CLI = 8 segments (dispatcher chunks at 30 s) vs session = 1 segment. Root
      cause (verified in `crispasr_c_api.cpp`): the raw `crispasr_session_transcribe`
      is a LOW-LEVEL "transcribe this buffer" primitive that does NOT auto-chunk —
      `moonshine_transcribe_with_probs(ctx, pcm, n_samples)` runs one pass over
      the whole buffer (degraded + very slow on a short-segment model like
      moonshine), while the CLI/server dispatcher adds chunking on top. **Parakeet
      is the exception** — its session branch has bespoke inline long-audio
      handling (chunked_merge), which is exactly what let #257's divergence exist.
      So this is an ARCHITECTURAL layering fact, not a per-backend bug: session
      callers must chunk long audio themselves or use `transcribe_chunked`.
      A uniform fix (auto-chunk in the session for every backend, or push chunking
      below the session) is a real design decision for the maintainer, not a
      drive-by — deferred, documented here + in bindings docs. Short-audio parity
      (the common session use) is clean (Phase 1b audit above).
- [x] **Phase 2** — proactive encoder memory policy — DONE (opt-in). Pure
      `parakeet_est_singlepass_peak_mb` / `parakeet_singlepass_fits_budget`
      (`test-parakeet-strategy`, +2 cases) proactively pick streamed over
      single-pass when the O(T²) bias would exceed
      `CRISPASR_PARAKEET_VRAM_BUDGET_MB` — before allocating, layered over the
      reactive OOM fallback. A/B: on the reporter's 225 s clip the estimate came
      out **1931 MiB (T=2812, H=8)** — matching the reporter's actual **1911.98
      MiB** — and `budget=1500` correctly switched to streamed (full 267-word
      transcript). Default (no budget) unchanged. **CUDA memory proof pending**
      (can't OOM on M1); the estimate matching the reporter's number is the
      evidence so far.
- [ ] **Phase 2** — unified encoder memory policy (proactive, replaces ad-hoc gates)
- [x] **Phase 3** — diff-harness parity in CI — **ALREADY IMPLEMENTED** (verified,
      no new code). The initial PLAN under-counted the existing harness. On audit,
      `.github/workflows/regression.yml` already runs: the dry-run pin gate on PR,
      a nightly per-backend matrix via `tests/regression/run_one.py`, per-stage
      `cos_min` thresholds, exact-transcript + CER/WER tolerance, AND a full
      TTS→ASR roundtrip gate (21 `tts_backends`, WER≤max) — the exact "cos 0.99 →
      hallucination" / "component cos but audio garbage" coverage this phase
      wanted. The pure functions (`_levenshtein`, `compute_transcript_metrics`,
      `parse_diff_stdout`, `evaluate_stage_thresholds`) are unit-tested in
      `tests/regression/test_driver_smoke.py` (model-free, runs in CI). Nothing to
      add without redundancy. Future extension if desired: a distinct word-OVERLAP
      (present-in) verdict alongside WER for very-noisy roundtrips.
- [~] **Phase 4** — server throughput — primitive DONE, integration scoped.
      Landed the reusable, thread-safe `core_pool::WorkerPool<T>` (RAII lease,
      blocking acquire, `test-worker-pool` 17 assertions incl. a blocking-until-
      release concurrency case) + the `CRISPASR_SERVER_WORKERS` gate design.
      **Full server integration deferred to 4b with a hard constraint I verified
      by reading the code:** `do_transcribe`'s single `model_mutex` guards not
      just the backend but the SHARED, explicitly non-re-entrant post-processing
      contexts (punctuation `fireredpunc`/`pcs`, truecaser — comment at
      server L708). So a correct pool must ALSO pool (or fine-grain-lock) those,
      not just the backend — otherwise concurrent workers race the truecaser.
      That's a real `do_transcribe` locking refactor + a concurrent load test,
      not a drop-in; not shipping it half-verified. 4b plan: (a) split
      `do_transcribe` into ASR (pooled, lock-free) + post-process (own mutex or
      per-worker ctx); (b) build the pool of N workers at startup; (c) A/B N=1 vs
      N=2 throughput with identical transcripts; GPU-worker concurrency
      (per-context Metal queues) validated per-platform.
- [x] **Phase 4b** — WorkerPool wired into the server — DONE (gated, correctness-
      verified; throughput is workload-dependent). `CRISPASR_SERVER_WORKERS=N`
      builds N independent backends; a "pure ASR" request (explicit language, no
      aligner, no punctuation/truecaser) routes to a pooled worker and runs
      concurrently, while anything touching the shared LID/aligner/post-processing
      stays on the primary backend + `model_mutex` (serialized, unchanged). `/load`
      returns 409 when the pool is active (pooled workers hold the startup model).
      Default N=1 → `asr_pool` null → zero behaviour change.

      **A/B (honest):** correctness holds — 2 concurrent requests returned
      identical, correct transcripts, and they demonstrably ran in parallel. But
      **throughput is workload-bound**: on this CPU (M1) with the memory-bandwidth-
      bound parakeet-tdt-1.1b-q4_k, 2 concurrent requests took **62 s vs 16 s
      single** — *worse* than the ~32 s a serial pair would take, i.e. the two
      instances contend for memory bandwidth. So the pool is a **latency/
      throughput win only where a single request under-utilises the box** (spare
      cores, a GPU not saturated by one stream, smaller models, or I/O-bound
      mixes), and a net loss on a saturated memory-bound CPU. Kept **default-off**
      per the "flip only when it wins on speed AND quality" rule; documented as an
      opt-in for deployments with headroom. (GPU-worker concurrency —
      per-context Metal/CUDA queues — still wants a per-platform check.)

Execution order is deliberately **0 before 1**: the parity test is the guard that
proves the dispatch unification changes nothing observable. Each phase merges to
`main` green before the next starts.

---

## Phase 0 — cross-surface parity harness + test

**Why:** #257 needed the identical fix in CLI adapter, server, and session C-ABI
because they are three separate code paths. A test that pins "all surfaces agree"
would have caught the wiring gap on commit 1, and is the precondition for safely
unifying them (Phase 1).

**Deliverable:** `tests/test-surface-parity.sh` (live, needs a model) — runs the
same clip through (a) `crispasr` CLI, (b) an in-process session via the Python
binding, (c) the HTTP server, and asserts identical segment text/offsets. Plus a
pure **unit test** for the params→`whisper_params` marshalling helper Phase 1 adds.

**Env gate:** none (test infra). Gated to run only when `CRISPASR_MODELS_DIR` +
a parakeet model are present (label `live;parity`).

**A/B method:** the test itself is the A/B — CLI vs session vs server on the same
seed/clip; diff the JSON `transcription[]`.

**Acceptance:** three surfaces byte-identical on parakeet (F16 + Q4_K) for a short
clip and a `--chunk-seconds 7` clip; unit test green.

## Phase 1 — collapse the dual dispatch

**Why:** `crispasr_session_transcribe*` reimplements each backend's transcribe
inline (dev-guide HARD RULE #6) instead of calling the `CrispasrBackend` adapter
the CLI/server use. Every fix/feature/default risks landing in one path but not
the other (JA-detection was patched in ~5 places; #257 segmentation in 3).

**Approach:** add a session→`whisper_params` marshaller (sticky `source/target_lang`,
chunk, att-context, hotwords, …) and route `crispasr_session_transcribe*` through
`backend->transcribe()` for backends that have an adapter. Delete the inline
per-backend branch once parity holds. Start with **parakeet** (freshest), then
canary/cohere/granite/etc.

**Env gate:** `CRISPASR_SESSION_UNIFIED_DISPATCH` — `1` routes through the adapter
(new), `0` keeps the inline path (old). Default `0` until parity proven per
backend, then flip to `1` and keep `0` as the A/B escape hatch.

**A/B method:** Phase-0 parity test with the gate `0` vs `1` — must be byte-identical
before flipping. Judge decoded output (HARD RULE #3), F16 + Q4_K.

**Unit tests:** marshaller mapping (session fields → `whisper_params`), incl. the
sticky-language and chunk/att-context/overlap fields; a regression that the gate
default is the proven value.

**Acceptance:** parakeet session output identical gate 0 vs 1 across the parity
matrix; measured LOC deleted from the inline branch reported in this doc.

## Phase 2 — unified encoder memory policy

**Why:** windowed local attention is bit-exact + ~3× faster + lower memory yet
still opt-in (`--att-context` + `CRISPASR_FC_WINDOWED_ATTN`), and we just bolted a
*reactive* single-pass-OOM fallback on top (issue #257). Three scattered gates +
a catch-and-retry instead of one decision.

**Approach:** a `parakeet_pick_encode_strategy(T, backend, free_vram?)` that chooses
single-pass / windowed / streamed proactively (bound the O(T²) bias you can't
afford before allocating it). Keep the reactive fallback as a backstop.

**Env gate:** `CRISPASR_PARAKEET_MEM_POLICY` = `auto` (new default) | `single` |
`windowed` | `streamed` | `off` (current reactive-only behaviour). Never removes
the existing `--att-context` / `--chunk-seconds` / `CRISPASR_FC_WINDOWED_ATTN`.

**A/B method:** back-to-back on the reporter's 225 s clip + a long clip; decoded
output equality vs single-pass within tolerance + peak-footprint (`phys_footprint`
on macOS) and, where possible, a CUDA cross-check (R5 open item).

**Unit tests:** the pure strategy-selection function (T, caps, vram → strategy)
across boundary cases; no model needed.

**Acceptance:** `auto` never OOMs where single-pass would, output within tolerance,
peak memory ≤ single-pass; policy table documented.

## Phase 3 — diff-harness parity in CI

**Why:** the `dump_reference → crispasr-diff` methodology is the crown jewel but
run by hand. Automating it catches the "cos 0.99 snowballs into a hallucination"
class (and the TTS "component cos=0.999 but audio garbage" case) on every push.

**Approach:** a CI job (nightly / label-gated) that, per backend with a hosted
`ref.gguf` (dataset `cstr/crispasr-regression-fixtures`), runs the per-stage diff
+ the decoded-output roundtrip and reds on cos / word-overlap regression. Extend
the manifest beyond ASR-transcript-only to include a TTS→ASR roundtrip gate.

**Env gate:** n/a (CI infra); thresholds configurable via the manifest.

**A/B method:** self-checking (compares runtime vs hosted reference).

**Unit tests:** the threshold/verdict logic (cos_min, word-overlap → PASS/FAIL)
as a pure function.

**Acceptance:** at least parakeet + one TTS backend gated in CI; a deliberately
broken quant reds the job.

## Phase 4 — server throughput

**Why:** the server serialises on a single `model_mutex` — one request at a time.

**Approach:** request batching / a small worker pool / KV reuse. Bigger lift;
scoped last.

**Env gate:** `CRISPASR_SERVER_WORKERS=N` (default 1 = current behaviour).

**A/B method:** concurrent-request throughput + latency, N=1 vs N>1, identical
transcripts.

**Unit tests:** the queue/dispatch logic (no model).

**Acceptance:** throughput scales with workers, transcripts unchanged vs N=1.
