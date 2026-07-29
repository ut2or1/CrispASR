# CrispASR v0.8.24

A correctness release, and an unusually pointed one: several of these bugs were
**shipped fixes that had never actually run**. The #308 capitalisation fix lived
in a dead copy of the file. The dictionary auto-download was behind a macro that
is never defined on the target that compiles it, so English G2P had been falling
through to letter-to-sound rules for anyone without a pre-seeded cache. The CI job
guarding the exact build path #314 broke had been green throughout — it runs GCC
only, and GCC downgrades to a warning the very error clang raises. And a v0.8.22
safety gate was rejecting requests from every released Subtitle Edit build.

Headline work: a substantially rewritten **English G2P for Kokoro** (numbers were
silently dropped; the phonemes were the wrong alphabet), taking agreement with
misaki — Kokoro's own G2P — from **58% to 99.1%**; the real root cause of the
**MOSS-TTS 4B stop runaway** (it was the input tokens, not the forward pass);
**cross-lingual CosyVoice3**; and **VibeVoice speaker turns and timings**, which
the model had been answering all along and nobody parsed.

Drop-in from v0.8.23 — existing flags are unchanged.

## Fixed — Kokoro English pronunciation (#316)

Reported as *"speaks like old English"* and *"it just skips the 82"*. Three
independent defects, none of which `crispasr-diff` could see: the Kokoro harness
is phoneme-**in**, so it starts downstream of everything that was wrong and would
have shown perfect parity while the audio was broken. Scoping it took feeding
misaki's phonemes through our own acoustic path — same model, same voice, only
the phonemes differed, and the audio was correct. The acoustics were never at
fault.

**1. Numbers phonemized to nothing.** Digits appear in no dictionary and no
letter-to-sound rule, so `82` produced no phonemes and the word simply vanished.
`core/num2words_en.h` now spells them out following misaki's reading, which is
not the obvious one: `1234` is "twelve thirty four" but `1005` is "one thousand
five"; `1100` is "eleven hundred" while `1000` stays "one thousand". This is the
shared G2P, so **piper's numbers are fixed too**.

**2. The wrong phoneme alphabet.** Kokoro was trained on misaki's output (`ʧ`,
`O`, no length marks); we emit espeak IPA (`tʃ`, `oʊ`, `ː`) because `g2p_en` is
tuned to match espeak-ng for piper compatibility. Every symbol is *in* Kokoro's
vocab, so nothing dropped and nothing errored — the model just received tokens it
had never seen in training. `ː` is the RP length mark, which is most of "she
becomes British", and why `CRISPASR_KOKORO_G2P=espeak-only` helped without fixing
it. This is now structured as a phoneme **dialect** (`core/phoneme_dialect.h`)
rather than a Kokoro-local hack; `EspeakIpa` remains the identity default so piper
cannot regress.

**3. The dictionary auto-download had never worked.** Every download in
`phonemizer.cpp` is guarded by `#ifdef CRISPASR_BUILD`, and that macro is defined
only on `crispasr-lib` — never on the `kokoro` target, which is where the file is
compiled. So CMUdict never auto-downloaded either: a user without a pre-seeded
`~/.cache/crispasr/cmudict.dict` got **no English dictionary at all** and fell
through to the letter-to-sound rules, which render "this" as `θˈɪs`. That is a
large part of what #316 sounded like, independent of the other two bugs, and it
explains why the reporter found espeak-only better.

Then the harder half — agreement with misaki, measured at every step:

| stage | agreement |
|---|---|
| CMUdict + correct alphabet | 81.6% |
| + misaki's lexicon as Tier 0 | 88.5% * |
| + morphological fallback (stems → `-s`/`-ed`/`-ing`) | 98.1% * |
| + contextual function words (`the`/`to`/`a`) | 90.8% |
| + Unicode punctuation in the tokenizer | 93.0% |
| + capitalisation stress + phrase-final variants | **99.12%** |
| held-out 10k frequency wordlist, untuned | **99.30%** |

<sub>* measured on a harder obscure-word corpus at that stage.</sub>

The findings behind those were measured, not guessed:

- misaki derives stress from **capitalisation**, not part of speech —
  `and`/`And`/`AND` → `ænd`/`ˌænd`/`ˈænd`. I had concluded this needed a POS
  tagger; reading `Lexicon.__call__` showed otherwise. Worth 5.5 points.
- its `'None'` lexicon key is the **phrase-final** reading, not a POS tag:
  "…is she?" → `ʃˌi`.
- the lexicon stores **stems**, so only 46% of inflected forms appear verbatim.
  Recovering the rest by rule is worth ~10 points on its own.
- real prose is not ASCII: em dashes and curly quotes fused `always—most` into one
  token that came out of the LTS rules as `ˈælwAsmɑst`.

Deliberately left alone: `that`, `used`, `read` and `desert` need a real POS tag,
and `DEFAULT` is measurably the better single choice (`that` wants `ðæt` 68% of the
time against `ðˈæt` 31%), so flipping them loses two tokens for every one gained.

**The lexicon is fetched from upstream, not redistributed.** `load_misaki_json()`
reads misaki's own `us_gold.json` / `us_silver.json` from
`raw.githubusercontent.com/hexgrad/misaki`, pinned to a commit — the same route
`ensure_cmudict_loaded()` already used for CMUdict. CrispASR hosts none of it, so
you receive the data from hexgrad under hexgrad's terms. Pinned rather than
tracking `main` because a G2P whose pronunciations shift between runs is not
reproducible. When the fetch is unavailable the runtime falls back to the CMUdict
path, so this is an improvement, not a dependency.

<details>
<summary>Provenance note — worth reading before republishing this data</summary>

misaki is Apache-2.0, but upstream documents nothing about where the **data** came
from and ships no `NOTICE`, so the license tag is not by itself evidence about the
lexicon. Measured against espeak-ng 1.52 `en-us` output, alphabet-normalised, on
random 120-word samples: `us_silver.json` is **87% byte-identical** to espeak-ng
output, `us_gold.json` **48%**. Against CMUdict the word sets are largely disjoint
(39% of CMUdict present, 72% of misaki absent), so it is not a CMUdict derivative.
espeak-ng is GPL-3.0 *including* its pronunciation dictionary. Nothing in CrispASR
depends on resolving this, by design — we redistribute none of it. Engineering
judgement, not legal advice.

</details>

## New — `--tts-phonemes`

Synthesize a phoneme string verbatim, skipping the G2P entirely:

```bash
crispasr --backend kokoro -m kokoro.gguf --tts "unused" \
         --tts-phonemes "həlˈO wˈɜɹld" --tts-output out.wav
```

This is the seam that separates *"our G2P is wrong"* from *"our model is wrong"*
in a single run — it is how #316 was diagnosed — and it had existed only as a
stale comment. Honoured by **kokoro** and **piper** (piper's runtime had exposed a
phonemes-in entry point all along; its adapter just never reached it deliberately).
Any other backend **refuses** rather than quietly synthesizing the text, because a
silent fallback makes an A/B look like the phonemes changed nothing — the exact
wrong conclusion to hand someone debugging a pronunciation. The guard runs before
any model loads, so the refusal is immediate.

Exposed on every surface: `set_tts_phonemes` (Python, Ruby, Rust),
`SetTTSPhonemes` (Go), `setTtsPhonemes` (Dart, Java), `SetTtsPhonemes` (C#),
`ttsSetPhonemes` (WASM), and a `"phonemes"` field on `POST /v1/audio/speech`.

## Fixed — MOSS-TTS 4B generated without stopping (#249)

The 4B model ran away past its stop token. The forward pass turned out to be
correct all along — f16 matched f32, flash matched eager, every op was
machine-precision against the reference. **The input tokens were wrong.**

We built the whole generation prompt as one string and tokenized it in a single
call. BPE is not compositional, so encoding text embedded in the template merged
the text boundary about two tokens differently than the reference implementation,
which encodes each segment separately. Those interior mis-tokenized tokens are
weighted near zero by early-layer attention but **amplified by the layer-10
attention sink**, drifting the backbone hidden state (cosine 0.99998 at block 9 →
0.9996 at block 10 → 0.991 at block 35) — enough to keep the two-way stop head's
gap high, so it never fired. Prompt assembly is now piece-wise, mirroring
`processing_moss_tts.py`.

The meta-lesson is recorded in `LEARNINGS.md`: check input-token parity before
blaming the forward pass. Three earlier diagnoses in this same investigation —
"an op bug", "a precision problem", "irreducible f32 accumulation" — were all
wrong, and each looked well-supported at the time.

Also fixed along the way: the eager-attention branch left its output as
`(hd,T,n_q)` while the shared reshape expects flash's `(hd,n_q,T)`, scrambling
heads with queries. And `moss-tts-local` now defaults its KV cache to **F32** —
the 4B's marginal stop head is KV-precision-sensitive.

## New — cross-lingual CosyVoice3 (#304)

CV3 clones reading a target language came out with a heavy accent of the
**reference voice's** language. CrispASR always ran zero-shot, prepending the
reference's own transcript — in its own language — which biases the phonetics.

With a target language set (`-l` / `--language`, or `-tl`) that differs from the
reference voice's, CrispASR now keeps the required prompt framing and the
reference **speech** tokens (which carry timbre) but drops the reference
transcript, so the target text drives the phonetics. Same-language clones keep the
full zero-shot prompt, which is the higher-fidelity default.

One subtlety cost a full validation round: dropping only the transcript collapsed
the AR decode to one or two tokens ("Thank you."). The orphaned reference *speech*
tokens — now with no matching text — were wrecking it. Upstream drops both, and so
do we.

The HTTP server also learned a `language` (or `target_lang`) field on
`/v1/audio/speech`, without which no Subtitle Edit-style client could select the
target language at all.

<details>
<summary>Native-Vulkan CosyVoice3 — still CPU-routed, now fully root-caused</summary>

The gated `CRISPASR_COSYVOICE3_VULKAN_NATIVE=1` path is not a broken ggml op
(`test-backend-ops` passes every flow/HiFT op on a real P100) and not the gallocr
dispatch (bit-identical to the scheduler on CPU). It is aggregate precision
sensitivity: in-tolerance floating-point deltas accumulating across a 22-layer DiT
× 6 CFM steps, amplified by the log-mel vocoder — flow mel cosine 0.961. The LM
alone is bit-correct on Vulkan (512/512 greedy tokens). The all-CPU route stays the
shipped default.

</details>

## Fixed — VibeVoice was answering with speaker turns and timings; nobody read them (#300)

VibeVoice-ASR is prompted for JSON and answers with
`[{"Start":0.0,"End":11.0,"Speaker":0,"Content":"…"}]`. The adapter handed that
entire blob back as one segment's `text`. The speaker turns and their timings were
present in every response and never parsed — `--stream` printed raw JSON, all
per-utterance times collapsed to a single window-spanning segment, and the
`"speaker"` field added for streaming could not fire for this backend at all.

My own closing note on #300 and three docs had claimed this was a no-op for
VibeVoice "because the speaker info is inline text". It is not inline text; it is
structured data.

Now parsed into one segment per utterance with real `t0`/`t1` and the speaker in
the structured field. The parser is a hand-written scanner rather than a strict
JSON parse on purpose: this is LLM output, and a decode that hits the token cap
ends mid-array — a strict parse of a truncated blob discards every *complete*
utterance before the cut, while the scanner loses only the unfinished object.
`CRISPASR_VIBEVOICE_RAW_TRANSCRIPT=1` restores the old single-blob behaviour for
callers already parsing it themselves.

**New ABI accessor** `crispasr_session_result_segment_speaker(result, i)`, since
`crispasr_session_seg` had nowhere to put the label — so the bindings had been
handing callers the raw blob even after the CLI could parse it. Exposed as
`speaker` / `Speaker` / `:speaker` across all seven wrappers, each probing for the
symbol so a newer wrapper still works against a pre-0.8.24 library. The ordinals
are **chunk-local**: `Speaker 1` from one call is not necessarily the same voice as
`Speaker 1` from the next.

## Fixed — the #308 punctuation fix had been living in a dead copy

Auditing backends for the native-punctuation flag turned up why #308 never
actually worked. `moonshine-streaming` and `canary-qwen` still printed
*"ANd so, my fellow Americans…"* — the exact double capital #308 fixed — and the
guard was plainly there in `src/fireredpunc.cpp`. Instrumenting that file and
rebuilding produced **no output at all**, which was the answer: there are two
copies, and `src/CMakeLists.txt` prefers the shared `crisp_punc/` library. #308's
fix had gone into the other one. Dead code for months while the shipping copy kept
the bug.

- the guard now exists in both copies, and `tests/test-punc-copies-in-sync.cpp`
  fails if they diverge again.
- a second defect the same trace exposed: the pass appended a mark to text that
  already ended in one — `"your country."` → `"your country.."`. It no longer
  stacks punctuation on punctuation, ASCII or full-width.
- `vibevoice`, `moonshine-streaming` and `mimo-asr` now declare
  `CAP_PUNCTUATION_NATIVE`, so the second pass is skipped on output an LLM decoder
  already punctuated and cased.

The audit *method* needed correcting too: `--no-punctuation` looks like the way to
read a model's raw output, but it strips punctuation after the fact, so every
natively-punctuating model looks unpunctuated under it. On that reading the entire
fleet needed the flag — which would have silently deleted the commas of every
backend that genuinely needs the pass.

## Fixed — Subtitle Edit voice cloning against CrispASR ≥ 0.8.22 (#312)

v0.8.22 made `"spoken_disclaimer": false` a hard 400 unless the request also
carried `marking_attestation`. Subtitle Edit only started sending that field on
2026-07-26, *after* its v5.1.0-rc16 release — so **every released SE build lost
Chatterbox voice cloning**. Despite the report's title this had nothing to do with
CUDA or Vulkan; the failure is pre-synthesis.

An unattested opt-out is now **denied, not refused**: the request is served with
the spoken disclaimer applied — the documented default — and told so via
`X-Crispasr-Spoken-Disclaimer` / `X-Crispasr-Marking-Warning` plus an audit line.
Serving the *stronger* default cannot emit weaker-than-default output, which was
the entire point of the gate, whereas refusing outright turned a client one field
behind into a client with no TTS at all. The CLI keeps its hard refusal (exit 12),
where the error can name the flag you would add to the command you just typed.

Also: a server launched with `--accept-marking-responsibility` now satisfies the
per-request gate, and the streaming path no longer ignores an honoured opt-out.
`marking_attestation` had never been documented — `docs/server.md` was telling
integrators to send exactly the request the server had started rejecting.

## Fixed — build failure on Termux / clang ≥ 16 (#314)

`-DCRISPASR_AMR_FETCH=ON` died with `ISO C++17 does not allow 'register' storage
class specifier`. opencore-amr is 2000s-era code that still declares locals
`register`, a storage class C++17 removed. `-Wno-register` is now scoped to the two
vendored AMR targets — not applied globally, which would suppress the same mistake
in our own code.

**Why CI missed it, which is the interesting part.** A `linux-amr-fetch` guard job
already existed for exactly this path and had stayed green throughout — because it
installs and uses **g++ only**, and GCC treats `register` in C++17 as a warning
where clang makes it an error. A guard job that runs one compiler family guards one
compiler family. It is now a `{gcc, clang}` matrix. `docs/install.md` gains an AMR
section documenting the build knobs and the pre-0.8.24 workaround.

## Fixed — VibeVoice long-form ASR truncation (#315)

PR #315 scaled the generation budget on the session path — which fixed LocalAI but
not the CLI or the HTTP server. Both route through the backend adapter, which used
`p.max_new_tokens > 0 ? …`, and since the default is 512 (`> 0`), that always
forced 512 and bypassed the duration-scaled resolver. Both surfaces now follow the
documented contract: not explicitly set → auto budget; `--max-new-tokens` →
honoured.

## Rust and Python packaging (#313)

- **`crispasr` and `crispasr-sys` are on crates.io.** Both consumption modes are
  documented: a git dependency (where `build.rs` cmakes `libcrispasr` from the
  checkout), or the registry release plus a pre-built library via
  `CRISPASR_LIB_DIR` — the crates.io package cannot vendor the C/C++ sources.
- `build.rs` no longer fails cryptically off-repo: `DOCS_RS` short-circuits, and an
  absent parent `CMakeLists.txt` produces an actionable message.
- Windows single-config builds (`build-windows.bat`, Ninja/NMake) put
  `crispasr.lib` directly under `build\src\`, which the import-lib probe missed —
  it only looked for the MSVC multi-config `src\Release\` layout used by the
  release bundle. Both are detected now.
- **Python wheels now bundle the native library.** `pip install crispasr` gets a
  working `libcrispasr` for linux x86_64/arm64, macOS arm64 (Metal) and Windows
  x86_64; GPU wheels (`+cuda`, `+vulkan`) are served from a PEP 503 index on
  GitHub Pages via `--extra-index-url`, and a pure-Python sdist remains the
  fallback elsewhere. This is the first release where that actually works — the
  first tag run failed its own install-and-load test on six of seven platforms,
  and fixing it turned up four separate defects (Linux wheels missing
  `libopenblas.so.0`; the Windows wheel containing no library at all, with its
  smoke test switched off so nothing said so; the CUDA driver wrongly treated as
  a missing dependency; and `_helpers.c` failing to compile on *every* wheel ever
  built, silently dropping the legacy `CrispASR` class).

## Also in this release

- `docs/bindings.md` gains a **Result field reference table**, the counterpart to
  the setter table, documenting which accessor is spelled what in each of the seven
  wrappers.
- `docs/feature-matrix.{md,html}` regenerated — they are auto-generated from
  `crispasr --list-backends-json` and had gone stale. The backend count is now 105.
- New unit tests: G2P (285 assertions / 20 cases), phonemes policy (17 / 3),
  marking policy (32 / 8), VibeVoice transcript parsing (32 / 8), punctuation-copy
  sync. Plus `tests/test-kokoro-g2p-live.sh`, which asserts a number is *audible*
  after an ASR round-trip — something no tensor-level check can see.

## Known gaps

- Number expansion is **English-only**; `g2p_de` / `g2p_fr` / `g2p_es` still drop
  digits.
- misaki's reduced vowels `ᵊ` / `ᵻ` are carried through the lexicon but not
  synthesized by the fallback rules.
- `lfm2-audio`, `fastconformer-ctc` and `wav2vec2` are still unaudited for
  `CAP_PUNCTUATION_NATIVE` — no local GGUFs to test against. The method is written
  down in `docs/contributing.md`.
- Heteronyms (`read`, `desert`, `live`) resolve to a single pronunciation; a real
  POS tagger is the only fix.

## Registry status for this release

| registry | v0.8.24 | |
|---|---|---|
| GitHub release | ✅ | 27 assets, same coverage as v0.8.23 |
| Docker | ✅ | |
| crates.io | ✅ | `crispasr` + `crispasr-sys` |
| pub.dev | ✅ | `crispasr` 0.8.24 — the first pub.dev release since 0.8.22; its automated-publishing tag pattern had been rejecting `v<version>` tags, which is why 0.8.23 never published |
| PyPI | ✅ | bundled CPU wheels for linux x86_64/arm64, macOS arm64, Windows x86_64, plus the sdist; GPU wheels on the Pages index |

### ⚠ macOS users: re-download `libcrispasr-macos-arm64.tar.gz` if you took it early

The macOS library bundle first attached to this release was built with no
deployment target, so it targeted the CI runner's OS and **required macOS 26.0**:

```
otool -l lib/libggml-metal.dylib   →   minos 26.0   sdk 26.5
nm -u  lib/libggml-metal.dylib     →   _OBJC_CLASS_$_MTLResidencySetDescriptor
```

On anything older it fails to load with `Symbol not found:
_OBJC_CLASS_$_MTLResidencySetDescriptor`. The asset has been rebuilt in place
with `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`: `minos` is now **11.0** and that symbol
is **weakly** bound, so older systems skip Metal residency sets at runtime
instead of failing to load, while macOS 26 still uses them. Nothing else about
the bundle changed. Only this one asset was replaced.
