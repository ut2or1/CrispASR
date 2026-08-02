// test-voice-clone-policy.cpp — what counts as a voice clone, and which
// containers can carry a C2PA manifest.
//
// Two gates hang off the first question (speaker consent: --i-have-rights /
// "consent_attestation"; and the Art. 50(4) audible AI disclosure), and the
// watertight marking floor hangs off the second. Both predicates are pure, so
// they are guarded here — on the unit tier CI actually runs — rather than only
// through a live server with a model loaded. Same reasoning as
// test-marking-policy.cpp, and the same lesson behind it (#312).
//
// Every "BYPASS" case below is a real one: it passed the old
// `voice ends with .wav` predicate as "not a clone", and each one produced an
// unattested, undisclosed clone of a real person's voice. They are the cases
// that prove this gate can go red.

#include "crispasr_marking_policy.h"
#include "crispasr_voice_clone_policy.h"
#include "crispasr_watermark_stats.h"

#include <catch2/catch_test_macros.hpp>

using crispasr_voice::classify;
using crispasr_voice::CloneDecision;

// Convenience: the common case where nothing was baked this run and the pack
// carries no provenance stamp — i.e. exactly what the old predicate saw.
static CloneDecision plain(const std::string& voice) {
    return classify(voice, /*baked_from_wav_this_run=*/false, /*pack_declares_clone=*/false);
}

TEST_CASE("no voice is not a clone", "[unit][compliance]") {
    REQUIRE_FALSE(plain("").is_clone);
}

TEST_CASE("a recording reference is a clone", "[unit][compliance]") {
    REQUIRE(plain("speaker.wav").is_clone);
    REQUIRE(std::string(plain("speaker.wav").reason) == "recording-reference");
    // Case-insensitive: the old predicate special-cased .WAV by hand, and any
    // other casing (.Wav) slipped through it.
    REQUIRE(plain("speaker.WAV").is_clone);
    REQUIRE(plain("speaker.Wav").is_clone);
    REQUIRE(plain("/abs/path/to/victim.wav").is_clone);
}

TEST_CASE("BYPASS 1: a voice baked from a wav this run is a clone", "[unit][compliance]") {
    // The TADA one-command clone bakes victim.wav into a temp .gguf and
    // REWRITES --voice to point at it before the gate runs. Suffix-only, the
    // most explicit cloning command in the CLI scored as "not a clone": no
    // --i-have-rights demanded, no [CONSENT] line, no spoken AI disclosure.
    const CloneDecision d = classify("/cache/tada-inline-voice.gguf",
                                     /*baked_from_wav_this_run=*/true, /*pack_declares_clone=*/false);
    REQUIRE(d.is_clone);
    REQUIRE(std::string(d.reason) == "baked-from-wav");
    // ... and the same path without that knowledge is what used to happen.
    REQUIRE_FALSE(plain("/cache/tada-inline-voice.gguf").is_clone);
}

TEST_CASE("BYPASS 2: a pack that declares it was baked from a recording is a clone", "[unit][compliance]") {
    // chatterbox clones ONLY through a baked .gguf — it has no .wav cloning
    // path at all — so a headline cloning backend could never trip either gate.
    // Same for --make-ref output and any hand-baked pack.
    const CloneDecision d = classify("my_voice.gguf",
                                     /*baked_from_wav_this_run=*/false, /*pack_declares_clone=*/true);
    REQUIRE(d.is_clone);
    REQUIRE(std::string(d.reason) == "pack-provenance");
}

TEST_CASE("an unstamped pack is a preset, not a clone", "[unit][compliance]") {
    // Deliberate: kokoro / qwen3-tts / miotts / vibevoice ship synthetic or
    // upstream-licensed preset voices as .gguf, and tada-ref-<lang> packs are
    // shipped references. Gating those behind a speaker-consent attestation
    // nobody can meaningfully give would break every documented example.
    // The honest cost is that a pack baked before the stamp existed reads as a
    // preset — re-bake it to gate it.
    REQUIRE_FALSE(plain("kokoro-voice-af_heart.gguf").is_clone);
    REQUIRE_FALSE(plain("tada-ref-de.gguf").is_clone);
    REQUIRE(std::string(plain("kokoro-voice-af_heart.gguf").reason) == "");
}

TEST_CASE("baked-from-wav outranks everything the file says", "[unit][compliance]") {
    // Runtime knowledge of provenance beats file metadata: a pack the runtime
    // just baked from a recording is a clone even if the pack forgot to say so.
    REQUIRE(classify("x.gguf", true, false).is_clone);
    REQUIRE(classify("x.wav", true, false).is_clone);
    REQUIRE(std::string(classify("x.wav", true, false).reason) == "baked-from-wav");
}

TEST_CASE("BYPASS 4: non-wav reference recordings are clones too", "[unit][compliance]") {
    // zonos accepts .mp3 and .flac references (crispasr_backend_zonos.cpp), so a
    // .wav-only predicate left `--voice victim.mp3` ungated on that backend.
    for (const char* v : {"victim.mp3", "victim.flac", "victim.m4a", "victim.ogg", "victim.OPUS"}) {
        INFO("voice=" << v);
        REQUIRE(plain(v).is_clone);
        REQUIRE(std::string(plain(v).reason) == "recording-reference");
    }
}

TEST_CASE("BYPASS 5: legacy packs are classified by producer architecture", "[unit][compliance]") {
    // A pack baked before the provenance stamp existed cannot be retro-stamped
    // once published. Fall back to what the pack IS: these architectures have
    // exactly one producer in-repo and it takes a user WAV.
    for (const char* arch : {"chatterbox-voice", "qwen3tts.voicepack"}) {
        INFO("arch=" << arch);
        const CloneDecision d = classify("legacy.gguf", false, /*pack_declares_clone=*/false, arch);
        REQUIRE(d.is_clone);
        REQUIRE(std::string(d.reason) == "pack-architecture");
    }
}

TEST_CASE("preset architectures stay ungated", "[unit][compliance]") {
    // Converted from upstream voicepacks / .pt files — no recording involved.
    // Gating these would demand a speaker-consent attestation nobody can give
    // and break every documented preset example.
    for (const char* arch : {"kokoro-voice", "vibevoice-voice", "", "some-future-arch"}) {
        INFO("arch=" << arch);
        REQUIRE_FALSE(classify("preset.gguf", false, false, arch).is_clone);
    }
    // tada refs are ambiguous (shipped tada-ref-<lang> AND user --make-ref share
    // the architecture), so they are covered by the stamp, not by the arch list.
    REQUIRE_FALSE(classify("tada-ref-de.gguf", false, false, "crispasr.reference").is_clone);
    REQUIRE(classify("myref.gguf", false, /*stamped=*/true, "crispasr.reference").is_clone);
}

// ---------------------------------------------------------------------------
// Watermark score statistics. The detector is a 32-bin sign-agreement test, so
// unwatermarked audio scores 0.5 on average and its tail is exactly computable.
// ---------------------------------------------------------------------------

TEST_CASE("the old 0.65 threshold was a 1-in-18 false positive", "[unit][compliance]") {
    // 0.65 needs 21/32 agreements, which clean audio reaches by chance 5.5% of
    // the time — and it was reported as "AI-GENERATED WATERMARK DETECTED".
    // Measured on real speech: 4.8% of 55 unwatermarked clips. This is the bug.
    const double p = crispasr_wm_stats::p_value(21.0f / 32.0f, 32);
    REQUIRE(p > 0.05);
    REQUIRE(p < 0.06);
    REQUIRE(crispasr_wm_stats::classify(0.6562f, 32) != crispasr_wm_stats::Verdict::Detected);
}

TEST_CASE("binomial tail matches the exact distribution", "[unit][compliance]") {
    REQUIRE(crispasr_wm_stats::null_tail_probability(0, 32) == 1.0);
    REQUIRE(crispasr_wm_stats::null_tail_probability(33, 32) == 0.0);
    // Symmetry: P(X >= 16) for n=32 is just over half (the median mass).
    REQUIRE(crispasr_wm_stats::null_tail_probability(16, 32) > 0.5);
    // Known values, cross-checked against Python's math.comb.
    REQUIRE(crispasr_wm_stats::null_tail_probability(26, 32) < 0.0003);
    REQUIRE(crispasr_wm_stats::null_tail_probability(26, 32) > 0.0002);
}

TEST_CASE("verdict bands: chance-level scores are never evidence", "[unit][compliance]") {
    using V = crispasr_wm_stats::Verdict;
    REQUIRE(crispasr_wm_stats::classify(0.0f, 32) == V::NotDetected);
    REQUIRE(crispasr_wm_stats::classify(0.5f, 32) == V::NotDetected);    // the null MEAN
    REQUIRE(crispasr_wm_stats::classify(0.5625f, 32) == V::NotDetected); // p = 0.30
    REQUIRE(crispasr_wm_stats::classify(0.6875f, 32) == V::Inconclusive);
    REQUIRE(crispasr_wm_stats::classify(0.75f, 32) == V::Detected); // p = 0.0035
    REQUIRE(crispasr_wm_stats::classify(1.0f, 32) == V::Detected);
}

TEST_CASE("suffix helpers are case-insensitive and anchored", "[unit][compliance]") {
    REQUIRE(crispasr_voice::is_recording_reference("a.WAV"));
    REQUIRE_FALSE(crispasr_voice::is_recording_reference("wav"));
    REQUIRE_FALSE(crispasr_voice::is_recording_reference("a.wav.gguf"));
    REQUIRE(crispasr_voice::is_voice_pack("a.wav.gguf"));
    REQUIRE_FALSE(crispasr_voice::is_voice_pack(".gguf.wav"));
}

// ---------------------------------------------------------------------------
// Watertight marking floor: which containers can carry a C2PA manifest.
// ---------------------------------------------------------------------------

using crispasr_marking::container_marking_for_format;

TEST_CASE("containers that carry a manifest allow the watermark opt-out", "[unit][compliance]") {
    REQUIRE(container_marking_for_format("wav").carries_c2pa);
    REQUIRE(std::string(container_marking_for_format("wav").c2pa_mime) == "audio/wav");
    // MP3 carries one via ID3v2.4 GEOB and the native signer has always handled
    // it — server-side signing was just hardcoded to the WAV branch, so every
    // non-WAV response shipped with no provenance at all.
    REQUIRE(container_marking_for_format("mp3").carries_c2pa);
    REQUIRE(std::string(container_marking_for_format("mp3").c2pa_mime) == "audio/mpeg");
}

TEST_CASE("containers that carry no manifest force the watermark on", "[unit][compliance]") {
    // These are the responses that were fully unmarked under an attested
    // --no-watermark: no manifest possible, and the mark stripped anyway.
    for (const char* fmt : {"pcm", "f32", "aac", "opus"}) {
        INFO("response_format=" << fmt);
        REQUIRE_FALSE(container_marking_for_format(fmt).carries_c2pa);
        REQUIRE(std::string(container_marking_for_format(fmt).c2pa_mime) == "");
    }
}

TEST_CASE("an unknown format falls back to WAV, matching the handler", "[unit][compliance]") {
    // The handlers' trailing `else` emits WAV. If this fell back the other way
    // the floor would be computed for a container that is never produced.
    REQUIRE(container_marking_for_format("").carries_c2pa);
    REQUIRE(container_marking_for_format("something-new").carries_c2pa);
}
