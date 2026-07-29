# CrispASR v0.8.22

A big cycle: a **new multi-speaker meeting-ASR backend (Tiron)**, a **~26× speedup
for source separation**, **TTS provenance hardening**, and a batch of correctness
fixes across the ASR backends. Drop-in from v0.8.21 — one behavior change to note
(TTS provenance opt-out, below).

## New — Tiron: multi-speaker meeting ASR (#295)

`Trelis/tiron` (Apache-2.0) — Whisper **large-v3** fine-tuned to transcribe
meetings and emit **inline `<|speakerN|>` markers**, so a single pass gives you
the words *and* who said them. Runs on the whisper backend (`--backend tiron`);
the loader auto-detects the extended speaker vocab and switches on a
constrained-decoding grammar (a port of the upstream harness — plain greedy loses
~5 cpWER), fixed non-overlapping 30 s windows, and an onset guardrail.

Speaker indices are window-local; `--diarize` promotes them to stable
meeting-level `SPEAKER_NN` by clustering per-`(window, local-speaker)` voiceprints
(TitaNet/ECAPA + agglomerative cosine), hoisted into the library so the CLI and
server share it. Byte-exact vs the Python reference (f16 token stream). Models
(public, Apache-2.0): [`cstr/tiron-GGML`](https://huggingface.co/cstr/tiron-GGML)
(`tiron-f16.bin`, `tiron-q4_k.bin`).

```bash
crispasr --backend tiron -m auto -f meeting.wav            # inline <|speakerN|>
crispasr --backend tiron -m auto -f meeting.wav --diarize  # meeting-level SPEAKER_NN
```

## Performance — mel-band-roformer `--separate` ~26× faster (#296)

Source separation "hung" on Linux/Windows even on short clips (reported for the
mel-band-roformer vocals model). Root cause: the forward runs entirely on **CPU**
(the CUDA banner is just backend enumeration) and was fast only on macOS — the
matmul used BLAS only under `#if defined(__APPLE__)`, the inverse STFT was a naive
O(N²) DFT, the attention was scalar, and per-block weights were re-dequantized on
every call, all with no progress output.

Fixed: portable `cblas_sgemm` for `linear()` **and** the attention (Accelerate on
macOS, OpenBLAS elsewhere, preferring OpenBLAS over reference cblas); an
**FFT-based iSTFT** (31 s → 0.2 s); OpenMP over the band/time loops with BLAS pinned
to one thread; per-layer weight hoisting; and per-layer progress. **11 s of audio:
~24 min → ~56 s (~26×), output bit-identical (cos = 1.0).** The Windows CPU release
build now links OpenBLAS so the fast path ships (a GPU path is tracked for
full-length songs).

## New — TTS provenance hardening (C2PA / watermark)

Every TTS output is now **provenance-marked by default**, and the CLI is watertight
— no output path can be emitted fully unmarked. Opting out of C2PA (`--no-c2pa`)
now requires an **explicit marking attestation**: the caller affirms they take
responsibility for labeling AI-generated audio. New `synthesize_raw` and
`accept_marking_responsibility` are exposed across **Go, Python, C#, and Dart**.

> **Action required if you script `--no-c2pa`:** the opt-out is now gated by the
> attestation. Update your invocation/binding call to affirm marking
> responsibility, or accept the default (marked) output.

## Performance — F5-TTS levers (#294)

Optional, env-gated speed levers for the F5-TTS backend: `CRISPASR_F5_DIT_SKIP`
(DiTReducio temporal skip), `CRISPASR_F5_EMBED_GPU` (GPU InputEmbedding path), and
a gated F16-activation lever (Metal no-op). Plus upstream-faithful reference
preprocessing (silence-strip + 12 s clip) and a duration-rate clamp so a
mismatched reference can no longer truncate the output.

## Fixed — ASR correctness

- **canary-qwen no longer emits `!`-spam.** A corrupt/over-quantized weight could
  produce NaN logits, which the greedy argmax turned into token 0 (`!`) for the
  whole decode. The argmax is now NaN-robust (aborts with a diagnostic instead of
  spewing garbage), and the same guard was swept into 7 more decode backends
  (`lfm2_audio`, `m2m100`, `moonshine`, `moss_transcribe{,_diarize}`, `moss_audio`,
  `t5_translate`). The published `canary-qwen-2.5b-q4_k.gguf` was itself
  NaN-corrupt and has been **re-quantized and replaced** (q8_0 was always fine).
- **Linux build unbroken** — `tiron_link.h` used `std::string` without including
  `<string>`; libc++ hid it on macOS but libstdc++ (Linux/CI) failed to build.
- **FFmpeg transcode TU** (#297) — moved a C-linkage template out of an
  `extern "C"` block so it compiles under GCC.

## Upgrading

Drop-in from v0.8.21. The only behavior change is the TTS provenance opt-out gate
above — every other change is additive or a fix. If you use `--separate` on
Linux/Windows, install/keep OpenBLAS available (the release binaries bundle or link
it) to get the fast path.
