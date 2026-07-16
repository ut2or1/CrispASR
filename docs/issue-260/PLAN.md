# Issue #260 — qwen3-tts "tinny noise" / characteristic spectrogram pattern

## NOW — active work

### LATEST (2026-07-16): native C2PA signers — DONE, replacing c2pa-rs for WAV
Branch `feat/c2pa-native-js` (3 commits ahead of the base; origin/main has since
diverged with unrelated FASTCONV/parity work → **rebase needed before merge**).

- **JS/WebCrypto signer** `bindings/javascript/c2pa.mjs` — pure WebCrypto ES256 +
  hand-built CBOR/JUMBF/COSE/RIFF. Validates in the c2pa-rs reference reader
  (only `signingCredential.untrusted`). Tests: 12 hermetic unit + 2 live parity
  (`npm test`; ctests `test-c2pa-js-unit/-parity`). ✅
- **C++ signer** `src/core/crispasr_c2pa_native.{h,cpp}` — same manifest, ES256 via
  vendored BSD-2 micro-ecc (`third_party/uecc/`, RFC-6979 deterministic) + header
  `crispasr_sha256.h`. Wired as the primary WAV path in `crispasr_c2pa_sign_pem`
  (c2pa-rs now optional, MP3/M4A only). Built as its own C++17 static lib
  `crispasr_c2pa_native` linked into crispasr-lib. Tests: 6 Catch2 unit +
  `c2pa_native_parity.sh` live parity. ✅
- **WASM win**: native C++ signer compiles under emscripten (verified), so
  `c2paSign("audio/wav")` now works WITHOUT `--c2pa` / the ~10 MB c2pa-rs stack.
- **WASM end-to-end VERIFIED (2026-07-16)**: built libwhisper with `third_party/c2pa`
  hidden (configure: "C2PA signing disabled" → native-only). `Module.c2paSign(wav,
  "audio/wav")` in Node signed 9644→11324 bytes; c2pa-python reader validated it
  (only `signingCredential.untrusted`, gen CrispASR / c2pa.created /
  trainedAlgorithmicMedia / Es256). Proves WAV Content Credentials in wasm with
  ZERO c2pa-rs. (Local emscripten 6.0.2 needs `-DCMAKE_CXX_SCAN_FOR_MODULES=OFF` to
  get past cohere.cpp's C++20 module scan — unrelated toolchain quirk.)
- **Merged to origin/main** (rebased clean). All three signers (JS, C++, wasm) done.

- **Root cause FOUND & reproduced (model-free): the built-in spread-spectrum
  watermark, not the qwen3-tts port.** Every TTS output from the CLI is
  unconditionally watermarked (`crispasr_run.cpp:2374`,
  `crispasr_wm_dispatch::embed`). With no AudioSeal `--watermark-model`, this
  falls back to `crispasr_watermark_embed_impl` (`examples/cli/crispasr_watermark.h`).
- **The audible regression**: commit `8b81c0fc0` (2026-06-07) raised the default
  `alpha` from `0.005` → `0.08` (16×) "for robust detection." At 0.08 the
  watermark paints a **fixed comb of 32 key-derived frequency bins** onto every
  1024-sample frame. ~20 of those 32 bins sit between **4.4 kHz and 11.8 kHz**,
  where clean TTS speech has almost no energy — so the comb stands out as tinny
  high-frequency noise and shows as steady horizontal lines on a spectrogram
  (exactly the reporter's description).
- **Why aggregate SNR hid it**: the commit claimed "~38 dB SNR, imperceptible."
  Measured on a clean speech-like signal: aggregate 41 dB, **in-band (<4 kHz)
  47.5 dB — but 4–11.9 kHz band only 3.3 dB SNR** (noise ≈ as loud as signal
  there). The energy is dumped where the ear hears it against near-silence.
  The code deliberately injects into empty bins ("For bins with no energy,
  inject at 0-phase so detection can still see the magnitude") — that line is
  what makes it audible.
- **Backend-agnostic**: the watermark is applied at a generic call site for ALL
  TTS backends, so kokoro/f5/etc. get the identical comb. It is NOT a qwen3
  bug; qwen3's clean HF-matched output just makes the comb obvious vs the HF
  reference the reporter A/B'd against.
- **Instant mitigation for the reporter**: set env `CRISPASR_NO_WATERMARK=1`
  (only escape today — there is no `--no-watermark` CLI flag).

### Reproduction artifacts (scratchpad)
- `wm_probe.cpp` — feeds a clean speech-like signal through the exact header;
  prints the 32 fixed comb frequencies + band SNRs.
- `spectrogram_compare.png` — clean vs alpha=0.08: comb lines at 4.4–11.8 kHz.
- Real-model A/B (in progress): qwen3-tts-1.7b-customvoice-q8_0, `--seed 42`,
  `CRISPASR_NO_WATERMARK=1` vs default → diff = watermark only.

## Fix — chosen direction: "2 then 1" + evaluate a permissive SOTA tool

### DONE — approach 2 (band-limit + lower alpha)  [commit on this branch]
- `wm_params()` in `crispasr_watermark.h`: comb `hi_bin` `n_fft/2-1` → `n_fft/5`
  (~4.8 kHz), default `alpha` `0.08` → `0.05`. Both embed + detect read
  `wm_params()` so they agree. `CRISPASR_WATERMARK_LEGACY=1` restores the old
  wideband/loud path (A/B + re-detect old marks). `embed_impl` alpha default
  `-1`=auto; `alpha==0` stays a true no-op.
- Real qwen3 clip: **above-5 kHz SNR 17.7 → 51.7 dB** (tinny region gone),
  detection 0.94 → 0.81 (>0.65 threshold), clean 0.44. All watermark unit
  tests pass (+ new speech-like #260 guard).

### Approach 1 (psychoacoustic masking) — EVALUATED, NOT SHIPPED (negative result)
Prototyped per-bin masker-proportional nudging (nudge ∝ local masker, not global
RMS) + variants (full-band, band-capped, with a detection floor). Measured on the
real qwen3 clip + synthetics. Conclusion: **classical masking does not beat the v2
band-limit for this spread-spectrum scheme.**
- Pure masking can't watermark low-masker content → pure-tone detection collapses
  to ~0.53 (nothing to hide under); synthetic-speech detection marginal (~0.69).
- Adding the detection *floor* needed to fix that re-injects flat energy into
  near-silent bins → above-5 kHz SNR falls back to 22–34 dB (a milder version of
  the #260 bug). Masking + robust detection are in direct conflict here.
- The masker-tracking property IS real (watermark 16.8 dB quieter in quiet frames,
  ~29 dB below local signal) — but that is exactly what a NEURAL watermark learns
  end-to-end. So the real "masking done right" = AudioSeal, not hand-tuned DSP.
- Decision: keep v2 (band-limit) as the zero-dependency classical fallback.

### Approach 2b (neural SOTA = AudioSeal) — DIFF HARNESS APPLIED, bug LOCALIZED
- License clean: `facebook/audioseal` is **MIT code AND weights, UNGATED** — no
  registration for users or devs; auto-downloadable like any CrispASR model.
- Produced a GGUF (`models/convert-audioseal-to-gguf.py`, 89 MB, 73 gen + 40 det
  tensors) and ran the harness:
  - **Generator: PERFECT** — `test_audioseal_cosine` full-output cos **1.000000**,
    watermark-only cos **1.000000**, RMS ratio 1.000 vs the PyTorch reference.
  - **Detector: WAS BROKEN, NOW FIXED.** Round-trip returned 0.4988 (chance) over
    "18 frames". Root cause: the detector head output is (T, C=18) but the
    post-processing assumed (C, T) — it sliced 2 elements along ne[0] (2 time
    samples, not the 2 detection classes), softmaxed the wrong axis, and read
    ne[1]=18 (the channel count) as the frame count. The Python detector shape
    (1,18,16000) gave the layout. Fixed the axis handling (detection = channels
    0,1 transposed to (2,T), softmax over classes, class 1; message = channels
    2-17 time-averaged). Now: round-trip **1.0000** over 16000 frames, clean
    **0.0024**, generator cos still 1.0.
  - The old round-trip test never caught this (it computed the detection prob but
    never asserted it — line 143 was a comment). Test now asserts >0.9 watermarked
    AND <0.5 clean → a real regression guard.
- **Productionized (opt-in SOTA upgrade):** GGUF hosted at `cstr/audioseal-GGUF`
  (MIT, ungated, README + Meta attribution); registry entry added; CLI resolver
  wired so `--watermark-model auto` downloads + loads it. End-to-end CLI verified:
  detect on an AudioSeal-watermarked clip → **0.9999**, clean → **0.0518**.
- **Perf:** AudioSeal embed ~72 ms/s of audio (+ 24k<->16k resample); the built-in
  spread-spectrum default is ~1.8 ms/s (~40x cheaper). Both negligible vs synthesis,
  but this is why classical stays the always-on default and AudioSeal is opt-in.
- Remaining (optional): 16-bit message-payload round-trip parity; a CUDA RTF A/B
  only if AudioSeal is ever considered for default-on (it is not — opt-in).

### Not done, deliberately
- `--no-watermark` CLI flag — **declined (EU AI Act Art. 50)**: an easy opt-out
  undermines the machine-readable AI-content marking obligation. Keep the
  `CRISPASR_NO_WATERMARK=1` env as a debug-only escape.

## Measured (both confirm the comb)
- Synthetic clean speech-like signal: broadband 41 dB, in-band(<4k) 47.5 dB,
  **above-band(4–11.9k) 3.3 dB** — comb clearly visible on spectrogram.
- REAL qwen3-tts-1.7b-customvoice-q8_0 clip ("The quick brown fox…", seed 42,
  2.48 s): broadband **38.8 dB** (matches the commit's claim), in-band 44.9 dB,
  **above-band(4–11.9k) 17.5 dB** — comb visible in the silence gaps where the
  clean clip is black. This is the tinny tone audible during pauses.

## Status
- [x] Root cause identified (built-in spread-spectrum watermark, alpha bump 8b81c0fc0)
- [x] Model-free reproduction + spectrogram (scratchpad wm_probe.cpp)
- [x] Real qwen3-tts A/B clip + spectrogram (single model run, watermark applied offline)
- [x] Confirmed backend-agnostic (generic call site crispasr_run.cpp:2374; key-derived comb)
- [ ] Fix direction confirmed with maintainer (product/provenance policy)
- [ ] Implement + A/B: detection confidence retained AND comb inaudible
