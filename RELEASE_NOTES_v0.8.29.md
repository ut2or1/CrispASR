# CrispASR v0.8.29

125 commits. The theme is **things that did not work at all** — not slowly, not
imprecisely, but not at all — found by reading the issue tracker rather than the
code:

* `--diarize-method pyannote` refused to download its own segmentation model, so
  it produced no speaker turns for anyone.
* `verbose_json` discarded every speaker label it was asked to produce, after
  paying for the diarization that computed them.
* Parakeet long-form could silently drop 45 contiguous seconds of a 230 s
  transcript.
* Every GGUF load leaked its weight mapping.

**If you use `--diarize`, parakeet on audio longer than 30 s, or any language
binding, this release changes behaviour you have been living with.**

Where a section covers something already released, it says so — this release
adds regression coverage for several earlier fixes, and knowing which ones you
already have is more useful than a longer list.

---

## Diarization

### `--diarize-method pyannote` never worked out of the box

The managed download for `pyannote-seg-3.0.gguf` was tagged with the licence
string `"other"`, which the registry classifies as restricted. So the default
`--sherpa-segment-model auto` refused to fetch its own segmentation model:

```
crispasr: model 'pyannote-seg-3.0.gguf' is released under a restricted licence:
  Licence: other (...)
crispasr: refusing to download without explicit licence acceptance.
```

and diarization then fell through to `sherpa needs --sherpa-segment-model` and
produced **no speaker turns at all**. The tag was simply wrong —
`cstr/pyannote-v3-segmentation-GGUF` is `license: mit` and ungated, matching the
upstream `pyannote/segmentation-3.0` it was exported from.

`"other"` remains restricted; that is the correct default for an unknown
licence. Only this one entry was mislabelled. A 600 s clip now diarizes into 6
speakers where it previously produced none.

### `verbose_json` discarded every speaker label (#326)

`crispasr_segments_to_openai_verbose_json` never emitted a `speaker` field — the
only occurrence of the word in that function was a comment. A request with
`--diarize` ran segmentation, ran the embedder, and threw the entire result away
on the way out. `diarized_json` carried labels; `verbose_json`, which is what
every OpenAI-compatible client asks for, did not.

It is emitted now, only when non-empty, using the same normalisation
`diarized_json` has always used — so both give `"A"` / `"B"` rather than one of
them leaking the internal `"(speaker 0) "` with its trailing space. A client
that does not know the field sees byte-for-byte the schema it saw before.

`response_format=text` and the default `json` genuinely cannot carry a label.
Combining either with `--diarize` now says so instead of silently charging you
for the stage.

### The embedder is 1.6–2.0× faster (#326)

After v0.8.28 chunked pyannote segmentation (32.8 s → 5.5 s on a 2888 s file),
the speaker embedder became the dominant cost — one forward per segment,
strictly sequential. It cannot be sped up with `-t`: on TitaNet those
per-segment graphs never engage ggml's threads at all. Measured on an M1,
interleaved, CPU time rather than wall:

```
2 s segments    -t 1  61.19 ms/seg, user 2.60s / real 2.69s
                -t 4  57.92 ms/seg, user 2.46s / real 2.55s
```

User CPU tracks wall within 3 % at every thread count. So segments are now
embedded across workers, each with its own model instance (a ggml backend is not
safe for concurrent use). End-to-end on the real pipeline, 47 segments:

| workers | embed stage |
|---|---|
| 1 | 13159 / 13197 / 19339 ms |
| 4 | 7411 / 8050 / 9911 ms |

Output is byte-identical between worker counts. Default is
`min(hardware_concurrency, 4)` — it tracks performance cores and then flattens,
so a larger cap buys memory rather than throughput. `CRISPASR_SPEAKER_EMBED_WORKERS`
overrides; `0` or `1` opts out.

There is also a per-stage timing line under `CRISPASR_DIARIZE_DEBUG`. The
embedder's share swings from ~3 % to ~60 % of a run depending on segment count,
so a total runtime cannot tell you whether it is your bottleneck.

---

## Transcription correctness

### Parakeet long-form dropped whole spans of 30–300 s audio (#350)

`parakeet-tdt-0.6b-v3` returned **66 % of a 230 s English transcript**, missing a
contiguous 45 s that decodes verbatim in isolation. Two independent causes, the
first being a routing bug: `crispasr_session_transcribe_chunked[_lang]` documents
`chunk_seconds = 0` as "use per-model defaults", but the unified dispatch
(default ON since v0.8.24) mapped it to "the caller never asked for chunking", so
an explicitly chunked long-form call fell through to the 300 s single-pass cap
and took one full-length decode.

### Transcripts could come out of time order (#356/#357)

Gap-fill recoveries were clamped rather than merged, so a covering segment could
emit timestamps that ran backwards. Segments are now split at recovery points so
output is monotone, the fill's tokens are carried into recovered segments, and a
warning fires when a transcript is not in time order — wired into the session
ABI as well as the CLI, not just one of the two.

### Parakeet long-form is 2.1× faster (#353)

The long-form window is decoupled from the single-pass cap, and encode/decode are
pipelined across both longform windows and VAD slices — at or above FluidAudio's
GPU mode on the same hardware. Three defects in the split encode/decode pipeline
were found and fixed while landing it.

---

## Memory

### The GGUF weight mapping was leaked on every load

`core_gguf::load_weights` maps the whole GGUF and hands the region to the device
on the zero-copy path. `buffer_from_host_ptr` has no deallocator parameter and
Metal passes `deallocator:nil`, so freeing the backend buffer released the
device-side view and left the host mapping in place — the loader dropped its
only handle to a region it still owned.

The deleted comment argued this was affordable because the kernel can evict
file-backed pages under pressure. That is false for this mapping: it is
`MAP_PRIVATE` with `PROT_READ|PROT_WRITE`, so macOS resolves the copy as each
page faults in and merely *reading* the weights privatizes them. Fixed in the
loader, in `crisp_audio`, in the split loader's overflow chunks, and in every
remaining backend.

---

## Text-to-speech

### CosyVoice3 — cloning through the session API (#334)

The four fixes behind that report — the CAMPPlus fused-bias bug that made every
WAV clone use a wrong speaker embedding (cos 0.737 → 0.999997), the missing
minimum decode length behind the "chipmunk effect", the per-reference caching
that cut a three-sentence request from 66.0 s to 45.7 s, and a resampler that
truncated its filter on every downsample — **shipped in v0.8.26**. If you are on
v0.8.26 or later you already have them.

What is new here is the last gap that thread left open: the session C ABI
required a transcript for a WAV and returned `-2` without one, because it cannot
run ASR itself. It still cannot — but the CLI already caches its auto-transcript
beside the clip, and that cache lives in `core/` so every consumer can share it.
A clip prepared once through the CLI now clones through Python, Rust and Go too:

| | |
|---|---|
| session `set_voice(wav)`, no cache | `-2`, now with an explanation |
| CLI run without `--ref-text` | auto-transcribes, caches it |
| same session call, unchanged | succeeds — ASR round-trip returns the requested line exactly |

The bare `-2` is now an explanation, which matters here because an *approximate*
transcript is worse than none: the talker infers the speaker's rate from it and
rushes or truncates the line.

### OmniVoice — fragments at the ends of phrases (#363)

OmniVoice is a masked *iterative* generator: it is handed a target length up
front and fills those frames. There is no EOS and no way to ask for more room,
so a length too small for the text does not produce a rushed clip — it produces
the start of the utterance and a fragment where the end belongs. That is why the
damage always lands at the end.

With a voice prompt that length came from `weight(ref_text) / ref_T`, entirely
caller-controlled and never validated. Same 2.60 s reference, same 22-word line,
changing only `--ref-text`:

| `--ref-text` | result |
|---|---|
| matches the audio | 6.60 s, complete |
| 3.2× too long | **2.04 s** of *"and then continued running through the field for f and then continued running the field for a very long time"* |

A reference implying an impossible rate is now rejected in favour of the built-in
anchor, with a warning naming the measured rate. The band (0.25–1.50 weighted
chars/frame) is deliberately wide — it rejects a transcript that does not
describe the audio at all, not an unusual speaker.

### Chatterbox V3

Pinned multilingual V3 ported with parity gates, cross-lingual cloning languages
honoured, and locale-independent config reads in the converters.

---

## Windows

**Both fixes in this section shipped in v0.8.25.** What is new in v0.8.29 is
that they are now guarded, because both were the kind of defect a green build
does not catch.

### The MSVC `glint` build depended on luck (#327, fixed in v0.8.25)

`glint/src/simd.hpp` called `__cpuidex` without including `<intrin.h>`. Our
`windows-latest` job builds glint and passed throughout, because some MSVC
toolsets pull that header in transitively — so whether this compiled came down
to your toolset. Two further bugs sat in the same six lines: the branch gated an
AVX1-only code path on the **AVX2** bit, so every Sandy/Ivy Bridge part silently
ran SSE2 under MSVC while GCC ran it with AVX; and it enabled AVX without
checking `OSXSAVE`/`XGETBV`.

None of it had a test, and `tools/sync-glint.sh` overwrites `glint/` wholesale
from upstream — so the fix survived only as long as upstream kept it. The new
`test-glint-simd-detect` lives in `tests/` for that reason, includes `simd.hpp`
first so the header must be self-sufficient, and cross-checks the x86 result
against `__builtin_cpu_supports` (the arm that would have caught the AVX2-bit
bug: two toolchains must agree about the same machine).

### Sherpa diarization hung forever on Windows CUDA (#328, fixed in v0.8.25)

The subprocess command line was built with POSIX single quotes, which `cmd.exe`
does not interpret, so sherpa received literal apostrophes inside its model
paths; the command also appended `2>/dev/null`, which is not Windows
redirection. Fixed by spawning through `CreateProcessA` with MSVC quoting and a
timeout — implemented on the POSIX side too, where the parameter had been
accepted and silently ignored.

The two quoters lived in the arms of an `#ifdef _WIN32`, so the Windows rules
never compiled on Linux or macOS. CI *did* compile them on `windows-latest` —
green, the whole time it was emitting command lines `cmd.exe` could not parse —
but nothing ever ran them. They are now ordinary functions compiled everywhere,
round-tripped against a reference implementation of `CommandLineToArgvW`'s
documented algorithm, so the quoting is checked on every CI host.

## Packaging

### The Windows CUDA runtime ships separately (#342)

The three NVIDIA runtime DLLs (`cudart64`, `cublas64`, `cublasLt64`) change far
less often than we cut releases. **This is the first release that actually
publishes the split assets** — the code landed one day after v0.8.28 was tagged,
and `release.yml` only runs on a tag, so it had never executed.

| asset | contains |
|---|---|
| `crispasr-windows-x86_64-cuda.zip` | CLI, self-contained |
| `crispasr-windows-x86_64-cuda-non-cuda.zip` | CLI, **without** the three DLLs |
| `libcrispasr-windows-x86_64-cuda.tar.gz` | libs + headers, self-contained |
| `libcrispasr-windows-x86_64-cuda-non-cuda.tar.gz` | libs + headers, **without** them |
| `cudart64_*.dll`, `cublas64_*.dll`, `cublasLt64_*.dll` | the three, on their own |
| `crispasr-windows-x86_64-cuda-runtime-sha256.txt` | SHA-256 of each |

The dev-lib package — at 874 MB the largest asset we publish, and so the one the
request was most about — was missed when the CLI package got this treatment; it
is included now. Both jobs also assert the three DLLs actually arrived, since
both copied them with `-ErrorAction SilentlyContinue` and a wrong `CUDA_PATH`
would have produced a silently CPU-only package named "cuda".

### Graceful degradation via `GGML_BACKEND_DL` (#355)

The `-cuda` tarball carries `libcuda.so.1` as a hard `DT_NEEDED`, so on a host
without the NVIDIA driver the loader kills the process at exit 127 before
`main()` and the advertised CPU fallback never runs.
`-DGGML_BACKEND_DL=ON -DBUILD_SHARED_LIBS=ON` now configures, links and runs.

Measured on M1 — parakeet + Silero VAD over a 300 s clip, all four runs 608
words:

| build | device | transcript |
|---|---|---|
| default | Metal | **X** |
| DL, `libggml-metal.so` present | Metal | **X** — byte-identical |
| default, `-ng` | CPU | **Y** |
| DL, `libggml-metal.so` removed | CPU | **Y** — byte-identical |

X and Y differ in two tokens; the `-ng` row shows that is ordinary CPU-vs-Metal
reduction-order divergence, present in the default build too.

**This remains opt-in and the prebuilt tarballs are unchanged.** ggml's CPU
registry does not expose the threadpool setter through `get_proc_address`, and
`ggml_graph_plan` has no DL equivalent, so a DL build cannot install the shared
worker pool and re-plans per call. Flipping the `-cuda` release leg needs an A/B
on real CUDA, HIP and Vulkan hardware; only Metal and CPU are verified.

---

## Bindings

- **The chat C ABI is bound in Python, Go, Java, Rust and Dart** on one shared
  contract (#361, #362), with cancellation via a session callback, prompt-token
  counting, and a close contract that waits for in-flight calls before freeing
  the session. Prompt handling branches from the shared prefix instead of
  flushing, and long prompts decode in prompt-batch-sized pieces.
- **Source separation** is bound in Rust and Go (#359).
- **The TTS speech-token floor** is exposed on every surface (#360).
- **`input_sample_rate` / `output_sample_rate`** are now bound in Python, Go and
  Dart (#321). They had been added to the five bindings that lacked
  `speech_to_speech`, which skipped the three that already had it — so those
  three kept s2s while never gaining the getter that says what to feed it.
- **Rust `cargo test` runs**, and the two bugs that made it fail are fixed.

---

## Alignment

`--align-only --text-file -` reads the transcript from **stdin** (#317), so an
embedder like Subtitle Edit does not have to spill it to a temp file. Piping an
`.srt` works too — the file path decides "this is an SRT" from the extension,
stdin sniffs the content, and the two agree byte-for-byte.

The error when no transcript is given now names the four ways to supply one, and
says that `-m`/`--backend`, `--vad` and `--max-len` belong to transcription and
are unused here — every one of which appeared in the failing command line in that
report.

---

## Also in this release

- **Pascal / CUDA (#302):** UE4M3 initialisation is gated instead of the CPU ISA,
  the GPU-host CPU baseline is portable, and the optimised CPU helper compiles on
  Windows.
- **Qwen3-TTS on HIP (#337):** GPU talker prefill runs on one backend, and a
  teacher-forced parity harness runs on Kaggle. **Not closed** — the divergence
  investigation is ongoing.
- **`--server-workers` note:** the worker pool serves pure-ASR requests only, so
  raising it for TTS gives N full model instances and no concurrency. The server
  now says so at startup.
- **MOSS valid-frame metadata** exposed in the stable C ABI.
- **Model licence policy** enforced for managed models; LID fallback policy kept
  in sync.

## New guards

Several of the above shipped because nothing was watching. These now run at PR
time:

- `check-cpu-backend-include.py` — a header used at 100+ sites must be reachable
  in all of them. Reads `#if` nesting rather than evaluating it, so one run
  covers every platform; a local build structurally cannot catch this class of
  bug, because the guards hiding it are the ones satisfied on the build machine.
- `check-cuda-split-packaging.py` — every job bundling the CUDA runtime must also
  ship a split archive, attach it to the release, assert the DLLs arrived, and
  pin the same CUDA version.
- `check-doc-anchors.py` — six dead in-repo anchors found and fixed.
- PowerShell here-string delimiters in workflow `run:` blocks — `release.yml`
  only executes on a tag, so a parse error there first appears *in* the release.
- `test-cpu-backend-shim`, `test-glint-simd-detect`, `test-subprocess-quoting`,
  `test-omnivoice-duration`, `test-embed-parallel`, `test-tts-ref-cache`, plus a
  Dart binding CI job.

Unit suite: **1682 tests**, up from 1645.

## Known limitations

- The prebuilt `-cuda` tarballs still cannot fall back to CPU (#355) — use
  `crispasr-linux-x86_64.tar.gz` if you might run without a GPU.
- Qwen3-TTS HIP divergence (#337) is open.
- `--diarize-embedder` remains the dominant diarization cost on files with many
  segments even after this speedup.
- Synthesis is serialised server-side; scale with N processes, not
  `--server-workers`.
