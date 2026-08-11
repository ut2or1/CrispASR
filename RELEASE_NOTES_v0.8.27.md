# CrispASR v0.8.27

A repair release. v0.8.26 published **only one of its seven Linux binary
tarballs**, and this fixes the workflow bugs that caused it. It also carries a
memory-safety fix in the audio loader.

**If you are on Linux, v0.8.26 probably had no binary for you** — plain x86_64,
arm64, CUDA, CUDA 13, Vulkan and HIP were all missing, and only the AVX-512
build made it. Use this release instead.

## Fixed — six of seven Linux tarballs never built (#339)

Two independent shell bugs in `release.yml`:

- `build-linux` / `build-linux-arm64`: the OpenBLAS gate fired before the
  bundling step, so the job exited before producing a tarball.
- The CUDA / CUDA 13 / Vulkan / HIP legs failed separately.

Diagnosed against the actual failing run rather than by reading the workflow,
and all six legs are expected back in this release. The `libcrispasr-*` shared
library bundles, Windows, macOS and the wheels were unaffected in v0.8.26.

## Fixed — a malformed Ogg file could crash the audio loader

`crispasr_audio_load()` on untrusted input could dereference a null pointer in
the vendored stb_vorbis. A Vorbis comment header declares its entry count
*before* the array is allocated, so an attacker-sized count made the allocation
fail and left a non-zero length with a null pointer — which the teardown path
then indexed. ASAN: `SEGV in vorbis_deinit`.

Found by the smoke-fuzz job, then reproduced deterministically with a
102-byte crafted file rather than left to chance; that file is now a permanent
regression seed, and the fuzz job replays every fixed crash on each run instead
of hoping to rediscover it. It also uploads the crashing input on failure now —
previously only a stack trace survived, which for a stochastic job means the
reproduction is gone.

An earlier patch had already hardened the sibling path in the same function; it
could not help this one, so the guard now sits in the teardown where it covers
every path in.

## qwen3-tts — diagnostics for cross-backend work (#337)

Following the "GPU produces different audio" report, three levers that make
that class of question answerable:

- `CRISPASR_QWEN3_TTS_GREEDY=1` — argmax, so a token stream depends only on the
  logits.
- `CRISPASR_QWEN3_TTS_REPLAY_CODES=<file>` — 16 codec ids per frame, replayed
  instead of sampled. Teacher forcing, which is what makes a per-step
  comparison mean anything.
- `CRISPASR_QWEN3_TTS_DUMP_LOGITS=<dir>` — raw per-frame talker logits.

With those, CPU vs Metal over 49 steps with an identical trajectory:
**worst cosine 0.999870, mean 0.999940, zero argmax disagreements.** The
backends agree at every step given the same history — the divergence users see
free-running is trajectory divergence seeded by ~1e-4 arithmetic, not a
miscompute. `--temperature` also now reaches the talker; it had only ever
reached the code predictor.

## Diff harness

The Python reference for qwen3-tts gains per-step talker logits, and finally
produces `generated_codes` — a stage that had been declared since the backend
was written and never written to. `tools/reference_envs/qwen3-tts/` records the
transformers pin the upstream package needs (4.57.3; the shared env carries 5.x
and importing against it raises).

## Known gaps

- The cross-implementation per-step diff still has no C++ stage. It is blocked
  on a design choice, not wiring: a per-step talker input includes a
  `trailing_text_hidden` term that is prompt-derived state the reference does
  not dump and the runtime does not expose. Both options are written up in
  `PLAN.md`.
- On macOS a malformed file can still drive the AudioToolbox fallback into a
  large allocation. That is a resource limit in Apple's decoder rather than
  memory corruption, and the path does not exist on Linux.
