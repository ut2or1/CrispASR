# CrispASR and the EU AI Act

CrispASR's position under **Regulation (EU) 2024/1689** (the EU AI Act): which
obligations attach to this software, which ones this project discharges in code,
which ones it cannot discharge for you, and where the enforcement actually lives
so it doesn't rot.

**This is not legal advice.** The classifications below are this project's
reasoned position on its own software, not a regulator's ruling. If you deploy
CrispASR commercially in the EU, get your own review — especially for the
sections marked **deployer duty**.

---

## 1. Who is who

The Act assigns duties by role, not by who wrote the code.

| Role | Who that is here |
|---|---|
| **Provider** of the AI system | Whoever *places it on the market or puts it into service under their own name*. For the upstream models, that's OpenAI / NVIDIA / Mistral / Alibaba / … For a product you ship that embeds CrispASR, **that's you**. |
| **Deployer** | Whoever uses an AI system under their own authority — you, running `crispasr` on real people's audio. Most Art. 50 duties land here. |
| This project | Publishes a runtime under MIT. It is neither the provider of the underlying models nor the deployer of your pipeline. Its job is to make sure the runtime doesn't *stop* you complying, and to hand you the machinery you need. |

The practical consequence: **CrispASR can mark its output, but it cannot inform
your users.** Sections 5 and 6 split accordingly.

---

## 2. Timeline

| Obligation | Applies from |
|---|---|
| Art. 5 prohibited practices; Art. 4 AI literacy | **2 Feb 2025** — in force |
| Ch. V general-purpose AI models | 2 Aug 2025 — in force |
| Art. 50 transparency (most of it) | **2 Aug 2026** |
| Art. 50(2) marking, for generative systems placed on the market *before* 2 Aug 2026 | **2 Dec 2026** (per the Parliament/Council provisional agreement of 7 May 2026) |
| Annex III high-risk regime (Ch. III) | **2 Dec 2027** — deferred from 2 Aug 2026 by the Digital Omnibus |

Two cautions. The Annex III deferral was agreed in June 2026 but takes effect on
publication in the Official Journal — **treat it as likely, not as law**. And the
deferral moved only the *high-risk* regime: the Art. 5 prohibitions were never
deferred, and neither were the Art. 50 duties for emotion recognition and
deepfakes.

CrispASR was placed on the market before 2 Aug 2026, so its Art. 50(2) marking
deadline is 2 Dec 2026. It has marked its output since well before that.

---

## 3. The open-source exemption, and what it does not cover

Art. 2(12) exempts AI systems released under free and open-source licences.
CrispASR is MIT. That exemption is narrower than it sounds — it **does not
apply** to:

- **prohibited practices** (Art. 5),
- **high-risk systems** (Art. 6 / Annex III),
- **Art. 50 transparency obligations.**

So the FOSS licence buys nothing for exactly the three areas that a speech
toolkit touches. Everything below is assessed as if the exemption did not exist,
because for these purposes it doesn't.

This is also why "just gate it behind a flag" is not a sufficient answer to an
Annex III capability. A flag governs the *deployer's* context; the provider
obligation attaches to placing a high-risk system on the market at all.

---

## 4. Prohibited practices (Art. 5) — in force since Feb 2025

| Practice | CrispASR |
|---|---|
| **Art. 5(1)(f)** — inferring emotions of natural persons in **workplace or education** settings | **Capability removed entirely.** See §4.1. |
| **Art. 5(1)(g)** — biometric categorisation to deduce race, political opinions, trade-union membership, religion, sex life or sexual orientation | Not implemented. CrispASR infers no protected attribute. See §4.3 on language ID. |
| **Art. 5(1)(h)** — real-time remote biometric identification in publicly accessible spaces for law enforcement | Not implemented, and structurally prevented: the named-voiceprint path refuses to run in streaming/live mode at all. See [`diarization-speakers.md`](diarization-speakers.md). |
| Art. 5(1)(a)–(e) — subliminal manipulation, exploitation of vulnerability, social scoring, crime prediction, untargeted facial scraping | Out of scope for a speech runtime. |

### 4.1 Emotion recognition — removed, not gated

SenseVoice-Small is upstream a multi-task model: its CTC head emits an
`<|HAPPY|>` / `<|ANGRY|>` / `<|SAD|>` / `<|FEARFUL|>` / … marker alongside the
transcript. Surfacing that value would make CrispASR an **emotion recognition
system** under Art. 3(39) — "an AI system for the purpose of identifying or
inferring emotions or intentions of natural persons on the basis of their
biometric data."

That is prohibited outright in workplace and education settings, and high-risk
(Annex III(1)(c)) everywhere else. CrispASR's headline use cases include meeting
transcription with speaker diarization — which is *precisely* the workplace
context Art. 5(1)(f) names.

A consent flag was considered and rejected. It would have addressed Art. 5 (the
system would no longer be placed on the market *for* workplace emotion
inference) but not Annex III(1)(c), which attaches to shipping the capability at
all and is not covered by the FOSS exemption. Retaining it would have committed
this project to the full Chapter III provider regime by Dec 2027 — risk
management system, data governance, technical documentation, human oversight,
accuracy and robustness testing, quality management system, conformity
assessment, CE marking, EU-database registration, post-market monitoring and
serious-incident reporting — or to a documented Art. 6(3) derogation assessment
that still requires registration. For one low-accuracy field on one backend out
of 54, that trade is not close.

**What was removed** (audit of 2026-08-01):

- `emotion` field dropped from `sensevoice_result` (`src/sensevoice.h`) and from
  `crispasr_segment` (`examples/cli/crispasr_backend.h`).
- `"emotion"` key dropped from both JSON writers (`cli.cpp`,
  `crispasr_output.cpp`).
- `sensevoice_transcribe()` now **strips the annotation prefix**. It previously
  returned it verbatim, so every session-ABI consumer — Python, Rust, Go, Dart,
  Java, C#, JS, WASM — received transcripts that literally began
  `<|en|><|HAPPY|><|Speech|><|withitn|>`. That was simultaneously a transcript
  corruption bug and an emotion inference leaking onto every binding.

The marker is still *parsed*, because that is what keeps it out of the
transcript text — the classified value is then discarded. `sensevoice_result.raw`
still holds the unfiltered model output for byte-exact diff-harness parity
against the FunASR reference; nothing in CrispASR surfaces it and nothing should.

Guarded by `tests/test-no-emotion-recognition.cpp` (unit tier — no model
needed). SenseVoice keeps its ASR, its 50+ language coverage and its native
language ID, which is what it was worth having for.

### 4.2 What is *not* emotion recognition, and why

Three things in the tree look adjacent to §4.1 and are not the same thing.
Stated explicitly so the next reader doesn't have to re-derive it:

- **Emotion *conditioning* in TTS.** chatterbox's `exaggeration` scalar, Zonos's
  8-dimensional emotion vector, Irodori's emoji prosody control,
  kartoffel-orpheus's `{Speaker} - {Emotion}: text` syntax. These *generate*
  expressive speech from an operator-supplied parameter. Art. 3(39) is about
  "identifying or inferring emotions **of natural persons** on the basis of
  their biometric data" — there is no natural person and no inference. Steering
  a synthetic voice is not recognition, in the same way a `<angry>` tag in a
  screenplay isn't.
- **SenseVoice audio-event tags.** `<|Speech|>`, `<|BGM|>`, `<|Applause|>`, and
  also `<|Laughter|>` and `<|Cry|>`, surfaced as `audio_event`. Recital 18
  excludes "the mere detection of readily apparent expressions, gestures or
  movements ... unless they are used for identifying or inferring emotions."
  Detecting that laughter occurred in a recording is an acoustic-event label;
  it is not a claim about the speaker's emotional state, and nothing here maps
  it to one. **Don't build that mapping downstream** — the moment you read
  `Laughter` as "happy", you are the one operating an emotion recognition
  system, and §4.1's analysis becomes yours.
- **`sensevoice_result.raw`.** Holds the unfiltered model output, marker
  included, for byte-exact diff-harness parity against the FunASR reference.
  Nothing in CrispASR reads it and nothing should; it is a parity-testing field,
  not a workaround.

### 4.3 Why language identification is not biometric categorisation

CrispASR infers the *language being spoken*, not a property of the speaker.
Art. 5(1)(g) bites on categorisation that *deduces* race, ethnic origin or
another protected attribute. A language label is an attribute of the audio
signal, and the mapping from language to any protected attribute is neither
performed nor implied.

If you build something downstream that uses a detected language as a proxy for
national or ethnic origin, that inference is yours and Art. 5(1)(g) is your
problem. Don't.

---

## 5. High-risk (Annex III) — CrispASR is designed to stay outside it

| Annex III point | Status |
|---|---|
| **1(a)** remote biometric identification | **Deliberately not implemented.** See below. |
| **1(b)** biometric categorisation by sensitive attributes | Not implemented (§4.3). |
| **1(c)** emotion recognition | **Removed** (§4.1). |

Point 1(a) is the one a diarization feature could drift into. The constraints
that keep the named-voiceprint path outside it are load-bearing, not stylistic:

- **Off by default** behind `--speaker-db-consent`; enrollment hard-fails
  (exit 25) without it.
- **Closed claimed roster only** — `--expect-speakers` is mandatory. The tool
  confirms which *asserted, enrolled, consenting* participants speak. It cannot
  answer "who is this unknown voice?"; the open-ended 1:N search that defines an
  identification system is not implemented, at the CLI or at the C API.
- **Active involvement** — enrollment is a deliberate act by the enrolled person,
  with the consent attestation recorded in the `.spkr` profile. Art. 3(41)'s RBI
  definition turns on identification *without* active involvement.
- **Post-processing only** — no real-time or streaming identification path
  exists (cf. Art. 5(1)(h)).

The default and recommended path — `--diarize-speakers` — computes embeddings
per recording, clusters them into `(speaker 0)` / `(speaker 1)` labels, and
discards them. It identifies nobody and stores nothing.

Full write-up, including the GDPR Art. 9 obligations that apply to the named
path regardless of AI Act classification: [`diarization-speakers.md`](diarization-speakers.md).

**If you re-enable 1:N identification, remove the roster requirement, or add
real-time matching, you are building an Annex III(1)(a) high-risk system and
this document stops describing your software.**

---

## 6. Transparency (Art. 50)

### 6.1 Art. 50(2) — machine-readable marking of synthetic audio

**Provider duty, discharged in code.** Every path that produces synthesized or
substantially altered audio marks it, by default, on every surface:

| Surface | Marking |
|---|---|
| CLI (`--tts-output`, `--tts-stream`) | audio watermark + C2PA manifest |
| HTTP server (`/v1/audio/speech`) | audio watermark + C2PA manifest |
| C ABI (`crispasr_session_synthesize`, `_streaming`, `_speech_to_speech`) | audio watermark |
| WASM / JS (`ttsSynthesize`, `ttsSpeechToSpeech`) | audio watermark; `c2paSign()` available |
| All language bindings | inherit the C ABI — they cannot reach an unmarked path by accident |

Two marking technologies, deliberately:

- **C2PA Content Credentials** — the interoperable, standards-based manifest, and
  the one a third-party tool will actually read. Native implementation
  (`third_party/c2pa-audio`), verified interoperable with the c2pa-rs reference
  reader in both directions. WAV (RIFF `C2PA` chunk), MP3 (ID3v2.4 GEOB), M4A/MP4
  (ISO BMFF `uuid` box). Raw ADTS `.aac` and Ogg `.opus` cannot carry a manifest,
  so they are remuxed to MP4 when C2PA is active.
- **Audio watermark** — survives re-encoding, transcoding and container loss,
  which a manifest does not. Spread-spectrum by default (band-limited to
  ~1.5–4.8 kHz so it stays inaudible); **AudioSeal** neural watermarking via
  `--watermark-model auto` for the stronger option.

**The watertight floor.** Opting out is possible but cannot produce a fully
unmarked file, on either surface. The rule is the same in both places: the
audio watermark may be dropped only when a C2PA manifest still marks the
output, so whenever the chosen container can't carry a manifest the watermark
is forced back on.

| Surface | Enforcement |
|---|---|
| CLI | `crispasr_enforce_cli_watermark_floor()` — per process, keyed on the output file extension. `--no-watermark` on a `.opus` output is overridden, with a printed notice. |
| Server | `crispasr_marking::container_marking_for_format()` — per *response*, since `response_format` is chosen per request. `pcm` / `f32` / `aac` / `opus` and the raw streaming path force the watermark on. |

The server's floor is per-response rather than per-process deliberately:
mutating the process-global flag would race across `--server-workers` threads.

This used to be a CLI-only guarantee. The server had no floor at all, so
`--no-watermark --accept-marking-responsibility` plus
`"response_format": "mp3"` returned audio with no watermark and — because C2PA
signing was hardcoded to the WAV branch — no manifest either, while the CLI in
the same configuration forced the mark back on. Same operator, same
attestation, two different floors. The server now signs every container that
can carry a manifest (WAV and MP3 today) and forces the watermark on the ones
that can't.

Any opt-out
(`--no-watermark` / `--no-c2pa` / `--no-spoken-disclaimer`) additionally requires
`--accept-marking-responsibility`, which writes a `[MARKING]` audit line
recording that the operator took the disclosure duty on themselves. The server
refuses to *start* with an opt-out flag and no attestation.

On the ABI, `crispasr_session_synthesize_raw()` returns unmarked PCM — for
callers that must resample, mix or concatenate *before* marking — and is
hard-refused (returns `nullptr`) until
`crispasr_session_accept_marking_responsibility()` has been called. Such callers
must then mark the result themselves via `crispasr_watermark_embed()`.

**Not marked, and why.** Art. 50(2) exempts systems performing "an assistive
function for standard editing" or not substantially altering the input data or
its semantics:

- **Transcription (ASR)** — produces a record of what a human actually said. No
  synthetic content is generated.
- **Translation** — generated text, but semantics-preserving by construction;
  this project reads it as standard assistive processing. The conservative
  reading is not obviously wrong, so if you publish machine-translated text at
  scale, consider marking it yourself.
- **Denoising and source separation** (rnnoise, HTDemucs, Mel-Band RoFormer) —
  removes or isolates existing signal rather than generating new content.
- **Speech restoration and upscaling** (Sidon, VoxCPM2 AudioVAE) — these *do*
  generate signal, and they **are** marked; they route through the watermarked
  S2S path.

### 6.2 Art. 50(4) — deepfake disclosure

**Deployer duty.** If you generate a voice clone, you must disclose that the
audio is artificially generated, **clearly and distinguishably, at first
exposure at the latest**. The Commission's guidance is explicit that this needs a
*visible or audible* label — a machine-readable watermark alone does not satisfy
Art. 50(4).

CrispASR gives you the audible label: voice-cloned output gets a **spoken
AI-disclosure prefix**, synthesized in a neutral voice, prepended to the clip.
It is on by default and skipping it requires an attestation
(`--no-spoken-disclaimer` + `--accept-marking-responsibility` at the CLI;
`"marking_attestation"` in the request body, or a server launched with
`--accept-marking-responsibility`, on the API).

**What counts as a clone.** Both this gate and the speaker-consent gate
(`--i-have-rights` / `consent_attestation`) hang off one predicate, in
`examples/cli/crispasr_voice_clone_policy.h`. A voice is a clone when:

1. it is a **`.wav` reference** handed straight to a cloning backend;
2. this run **baked it from a recording** — the TADA one-command flow bakes
   `ref.wav` into a temp `.gguf` and rewrites `--voice` to it; or
3. the **pack declares** it (`crispasr.voice.cloned_from_recording`), which the
   bakers stamp into any pack they derive from a real recording.

That predicate used to be spelled *"the path ends in `.wav`"*, inline, in two
places. It was wrong in three ways, and each one silently produced an
unattested, undisclosed clone of a real person's voice: the rewrite in (2)
erased the evidence before the gate ran; **chatterbox clones only through a
baked `.gguf`** and has no `.wav` path at all, so a headline cloning backend
could never trip either gate (the same held for `--make-ref` output); and the
server accepted a bare name, so `{"voice": "victim"}` reached the same file as
`{"voice": "victim.wav"}` while scoring as "not a clone". A `.gguf` baked from
someone's recording is exactly as much a deepfake as the recording. The suffix
is an implementation detail.

4. its **`general.architecture`** names a producer that only ever bakes from a
   recording (`chatterbox-voice`, `qwen3tts.voicepack`) — the legacy fallback
   for packs made before the stamp existed, which cannot be retro-stamped once
   published.

**Not every pack is a clone.** kokoro and vibevoice packs are converted from
upstream voicepacks and `.pt` prompts with no recording involved; gating those
behind a speaker-consent attestation nobody can meaningfully give would break
every documented example. So an unrecognised architecture stays a preset.

`crispasr.reference` (TADA) is deliberately **not** on the list: the shipped
`tada-ref-<lang>` packs and user `--make-ref` output share it, and gating the
shipped ones would break `-l de` auto-download. Those are covered by the stamp
going forward, so **a TADA reference baked before the stamp reads as a
preset** — re-bake it to gate it. Cases (1) and (2) never depend on the stamp.

Every producer that consumes a recording now gates and stamps at bake time:
`--make-ref`, `models/bake-chatterbox-voice-from-wav.py`,
`models/bake-qwen3-tts-voice-pack.py` and
`models/convert-tada-ref-to-gguf.py` all require `--i-have-rights`.

Baking is itself the cloning step. `--make-ref` sat *before* the TTS block's
consent gate and returned early, which made it the one way to build a reusable
voiceprint with no attestation demanded anywhere; the two Python voice-pack
bakers had no gate at all.

Note the deliberate `#312` design: an unattested opt-out is **denied, not
refused**. You still get your audio — with the default disclaimer — plus
`X-Crispasr-*` response headers and an audit-log line saying so. Serving the
stronger default can never emit weaker-than-default output, while a hard 400
would only break clients one field out of date.

**On the C ABI and WASM, Art. 50(4) is yours.** Those paths watermark
(Art. 50(2) ✅) but do **not** prepend the spoken disclaimer. That is a
deliberate limit, not an oversight: the CLI produces a *neutral-voice*
disclaimer by clearing `tts_voice` per call, and several backends need
adapter-specific handling to honour it. On the ABI the voice has already been
applied to the backend context, and there is no portable way to un-apply it —
synthesizing anyway would risk speaking the disclosure **in the cloned voice**,
which makes the fake more convincing rather than less.

The ABI gives you the pieces instead:

| Call | Use |
|---|---|
| `crispasr_session_disclaimer_text()` | The canonical string, identical to the one the CLI speaks. Render it as a **visible** label — Art. 50(5) requires disclosures to meet accessibility requirements, and audio-only is not accessible to a deaf user. |
| `crispasr_session_get_disclaimer_pcm()` | The disclosure synthesized in the neutral voice, for you to prepend. **Must be called before `set_voice()`** installs a clone; it returns `NULL` afterwards rather than risk the cloned-voice failure above. |

Supported order: open session → `get_disclaimer_pcm()` → `set_voice()` →
`synthesize()` → prepend.

Synthesizing with a clone voice and no attestation logs a one-time `[MARKING]`
line naming the duty. It does not refuse —
`crispasr_session_accept_marking_responsibility()` silences it.

**Cloning consent is not gated on the ABI.** The CLI (`--i-have-rights`) and the
server (`consent_attestation`) both hard-refuse without it; the ABI logs a
`[CONSENT]` line and proceeds. Three reasons: consent to clone a voice is a
personality-rights and GDPR matter rather than an AI Act duty; the caller is an
integrator who has read the header; and #312 is the standing lesson on what a
hard refusal does to a surface with many downstream clients. The audit trail is
the deliverable there, not the block.

### 6.3 Art. 50(1) — disclosure of AI interaction

**Deployer duty.** If your system interacts directly with natural persons, tell
them they're talking to an AI, unless it's obvious to a reasonably well-informed
person. A CLI transcription tool is obvious. A voice agent built on CrispASR's
S2S backends is not — that one is on you.

### 6.4 Art. 50(3) — emotion recognition / biometric categorisation

Not applicable: CrispASR implements neither (§4.1, §4.3). Had the emotion field
been kept, every deployer would have inherited a duty to inform exposed persons
of its operation, on every recording.

### 6.5 Art. 50(5) — form of disclosure

All disclosures must be given at or before first interaction, be clear and
distinguishable, and conform to applicable accessibility requirements. The spoken
disclaimer is audible; if your product has a visual surface, mirror it there —
audio-only disclosure is not accessible to a deaf user.

### 6.6 Synthetic *text* — the one Art. 50(2) surface CrispASR does not mark

Art. 50(2) covers systems generating synthetic "audio, image, video **or text**".
Everything above is about audio, because that is what this project is for. Two
surfaces generate text, and the distinction between them matters:

| Surface | Reading |
|---|---|
| ASR transcription | Not synthetic. A record of what a human actually said. |
| Translation (`/v1/translate`, `--translate`) | Generated, but semantics-preserving; read here as Art. 50(2) standard assistive processing (§6.1). |
| Punctuation restoration, truecasing, ITN | Assistive editing of an existing transcript. |
| **`POST /v1/chat/completions`** (`--chat-model`) | **Open-ended generation. Not assistive editing, and not exempt.** |

The chat endpoint is opt-in — it exists only when the operator passes
`--chat-model`, and it serves whatever general-purpose LLM GGUF they point it
at. **CrispASR does not mark its output**, and there is no watermark-equivalent
for short-form text that survives a copy-paste; the Commission's own guidance
and the Code of Practice on AI-generated content acknowledge that machine-
readable text marking is weaker and less settled than the audio case (metadata
that travels with the response, not a signal inside the words).

So this is a **stated gap, not a discharged duty**. If you enable `--chat-model`
and put its output in front of users, the Art. 50(2) marking duty and any
Art. 50(1) interaction disclosure are yours. Marking metadata on the response is
the practical option today.

Enabling a conversational endpoint also changes the §6.3 answer: "a CLI
transcription tool is obvious" does not cover a chat API. And note that shipping
a text-generating endpoint does not make this project a GPAI provider — the
model is the operator's choice and its provider's responsibility (§7).

### 6.7 Reading `--detect-watermark` (it is a diagnostic, not proof)

The built-in spread-spectrum detector is a **sign-agreement test** over 32
pseudo-random spectral bins. On audio with no watermark each bin is a coin
flip, so the score is `Binomial(32, 0.5) / 32` — it averages **0.5, not 0**.

The verdict used to be `score > 0.65 => "AI-GENERATED WATERMARK DETECTED"`.
That bar is 21/32 agreements, which clean audio reaches by chance **5.5% of the
time** — about one unwatermarked file in eighteen was reported as watermarked,
in the confident past tense. Measured on real speech (55 clips from
`samples/*.wav`): 4.8% false positives, matching the theory.

It now reports the exact probability of reaching that score without a
watermark, and asserts detection only at **p < 0.01**:

| Verdict | Condition |
|---|---|
| `AI-GENERATED WATERMARK DETECTED` | p < 0.01 (≥ 24/32 bins) |
| `INCONCLUSIVE` | p < 0.20 — leaning positive, not evidence |
| `No watermark detected` | consistent with unwatermarked audio |

**Raising the bar cannot make this instrument strong**, and the docs should not
pretend otherwise. Measured true-positive rate on freshly watermarked speech:

| clip length | > 0.65 (p = 5.5%) | > 0.71875 (p = 1.0%) |
|---|---|---|
| 1.0 s | 78% | 18% |
| 2.5 s | 86% | 43% |
| 5.0 s | 80% | 40% |
| 10.0 s | 100% | 60% |

So more clips now land in `INCONCLUSIVE` — which is the correct answer, because
the detector genuinely cannot tell. Two consequences worth internalising:

- **A negative result is not evidence the audio is human-made.** It is mostly
  evidence the clip was short.
- **Use `--watermark-model auto` (AudioSeal) when the answer matters.** That is
  the sensitive detector; its score is a probability, so the p-value bands above
  do not apply to it and are not used.

None of this affects *marking*: embedding is unconditional and the watertight
floor (§6.1) does not consult the detector. This is about not overclaiming when
reading a file back.

---

## 7. General-purpose AI models (Ch. V)

This project publishes quantized GGUF conversions of third-party models to
Hugging Face (`cstr/*-GGUF`). That does **not** make it a GPAI provider:

- Most of the models are narrow speech models — ASR, TTS, diarization — and do
  not display the "significant generality" Art. 3(63) requires.
- Quantization is a format conversion, not training. Under the Commission's GPAI
  guidelines, a downstream modifier becomes the provider of a modified model only
  when the modification uses more than a third of the original training compute.
  Quantization uses approximately none.

Some backends derive from omni-modal bases (Voxtral, Qwen3-Omni, LFM2-Audio)
which may themselves be GPAI models — but the obligations there sit with their
original providers, not with a downstream requantizer. Model cards in
`hf_readmes/` carry upstream attribution and licence terms.

---

## 8. Feature classification, at a glance

| Feature | Classification | Enforcement |
|---|---|---|
| ASR transcription (54 backends) | Minimal risk; Art. 50(2) assistive exemption | — |
| Text translation | Minimal risk; semantics-preserving | — |
| Language identification | Not biometric categorisation | — |
| Denoise / source separation | Art. 50(2) assistive exemption | — |
| TTS synthesis (52 engines) | **Art. 50(2)** — marked | watermark + C2PA, default-on, watertight floor on CLI *and* server |
| Voice cloning (`.wav` ref, inline bake, or stamped pack) | **Art. 50(2) + 50(4)** | + spoken disclaimer + `--i-have-rights`; `test-voice-clone-policy` |
| Voice-pack baking (`--make-ref` + all 3 Python bakers) | The cloning step itself | `--i-have-rights`; stamps `crispasr.voice.cloned_from_recording` |
| `--detect-watermark` | Diagnostic, **not** a gate | reports an exact p-value; "DETECTED" needs p < 0.01 (§6.7) |
| Speech restoration / upscaling / S2S | **Art. 50(2)** — marked | watermark via S2S path, same per-response floor |
| **LLM chat (`--chat-model`)** | **Art. 50(2) synthetic text** | **not marked — stated gap, deployer duty (§6.6)** |
| Session-scoped diarization | Not biometric identification | embeddings discarded, no names |
| Named voiceprint profiles | Kept outside Annex III(1)(a) | `--speaker-db-consent`, closed roster, offline-only |
| Voice-based emotion inference | Art. 5(1)(f) / Annex III(1)(c) | **removed**; `test-no-emotion-recognition` |
| Emotion *conditioning* in TTS; audio-event tags | Not Art. 3(39) — no person, no inference | §4.2 |

---

## 9. Deployer checklist

Things CrispASR cannot do for you:

- [ ] **Art. 50(4)** — show or speak an AI-generated label for any synthetic voice you publish. Default-on at the CLI and server; **your job** on the C ABI, WASM and bindings, using `crispasr_session_disclaimer_text()` / `crispasr_session_get_disclaimer_pcm()` (§6.2).
- [ ] **Art. 50(1)** — disclose AI interaction in conversational products. A server started with `--chat-model` is one.
- [ ] **Art. 50(2) for text** — if you enable `POST /v1/chat/completions`, marking its output is yours; CrispASR marks audio only (§6.6).
- [ ] **Re-bake old TADA references** — `chatterbox-voice` and `qwen3tts.voicepack` legacy packs are caught by architecture, but a `crispasr.reference` pack baked before the stamp reads as a preset (§6.2).
- [ ] **Don't treat `--detect-watermark` as proof either way** — it is a weak diagnostic with a stated error rate, not evidence of provenance (§6.7).
- [ ] **Art. 4** — ensure the people operating the system have adequate AI literacy.
- [ ] **GDPR** — voice is personal data, and biometric data when used to identify. The named-voiceprint path is Art. 9 special-category: explicit consent, retention and deletion policy, transparency. Applies independently of the AI Act.
- [ ] Don't repurpose diarization or speaker matching for surveillance, law-enforcement identification, or scraped audio. Out of scope and unsupported.
- [ ] Don't reconstruct emotion inference from the raw model output. §4.1 removed it for a reason; `sensevoice_result.raw` is a parity-testing field, not a workaround.

Penalties for scale: Art. 5 breaches reach €35 M or 7 % of worldwide annual
turnover; most other breaches €15 M or 3 %.

---

## 10. Where the enforcement lives

For whoever maintains this next — the compliance behaviour is code, and code
rots:

| Concern | File |
|---|---|
| Spoken-disclaimer opt-out policy | `examples/cli/crispasr_marking_policy.h` (+ `tests/test-marking-policy.cpp`) |
| **What counts as a voice clone** | `examples/cli/crispasr_voice_clone_policy.h` (pure) + `crispasr_voice_provenance.h` (resolve + read the stamp) (+ `tests/test-voice-clone-policy.cpp`) |
| Which containers carry a manifest | `crispasr_marking::container_marking_for_format()` in `crispasr_marking_policy.h` |
| Voice-pack clone provenance stamp | written by `tada_encoder_write_ref_gguf()` + all 3 `models/*` voice bakers; read by `crispasr_voice::read_pack_provenance()` |
| Legacy pack classification by producer | `crispasr_voice::architecture_is_recording_derived()` |
| Watermark score → p-value / verdict | `examples/cli/crispasr_watermark_stats.h` (+ `tests/test-voice-clone-policy.cpp`) |
| Watermark embed / detect | `examples/cli/crispasr_watermark.h`, `crispasr_watermark_dispatch.h` |
| Watertight CLI marking floor | `crispasr_enforce_cli_watermark_floor()` in `examples/cli/crispasr_run.cpp` |
| C2PA signing | `src/core/crispasr_c2pa.h`, `third_party/c2pa-audio` |
| ABI marking attestation | `crispasr_session_accept_marking_responsibility()` in `src/crispasr_c_api.cpp` |
| Voice-clone consent gate | `--i-have-rights` (`crispasr_run.cpp`); `consent_attestation` (`crispasr_server.cpp`); `[CONSENT]` audit line only on the ABI |
| ABI clone disclosure | `crispasr_session_{disclaimer_text,get_disclaimer_pcm}()` (+ `tests/test-abi-clone-disclosure.cpp`) |
| Speaker-DB consent gate | `src/speaker_db.cpp`, `crispasr_speaker_db_open/enroll2` |
| Emotion-recognition exclusion | `tests/test-no-emotion-recognition.cpp` |

A third rule earned by the audit of 2026-08-02, alongside the two below:

3. **Classify by provenance, not by filename.** Every one of that audit's
   findings was the same mistake — a compliance predicate spelled as a string
   suffix (`ends with ".wav"`, `response_format == "wav"`). Suffixes are
   rewritten, resolved, re-encoded and renamed by code that has no idea a gate
   depends on them. Ask what the thing *is*.

Two rules learned the hard way, both worth keeping:

1. **A gate that CI can't run is a gate that ships wrong.** The `#312` marking
   policy went four days broken because its only coverage was a live server test.
   Compliance logic belongs in weight-free, model-free headers with unit tests.
2. **Prove the gate can go red.** Every guard here was verified by re-introducing
   the thing it forbids and watching it fail. A green test that cannot fail is
   indistinguishable from no test.

---

## References

- [Regulation (EU) 2024/1689 — full text](https://artificialintelligenceact.eu/the-act/)
- [Commission Guidelines on transparency obligations (Art. 50)](https://digital-strategy.ec.europa.eu/en/library/guidelines-transparency-obligations-providers-and-deployers-ai-systems)
- [Code of Practice on Transparency of AI-generated Content](https://digital-strategy.ec.europa.eu/en/policies/code-practice-ai-generated-content)
- [Commission FAQ on Art. 50](https://digital-strategy.ec.europa.eu/en/faqs/transparency-obligations-under-article-50-ai-act)
- [`diarization-speakers.md`](diarization-speakers.md) — speaker labels, GDPR Art. 9, the RBI boundary
- [`tts.md`](tts.md) — synthesis, cloning, `--i-have-rights`
- [`server.md`](server.md) — `consent_attestation`, `marking_attestation`, `X-Crispasr-*` headers
