// crispasr_voice_clone_policy.h — what counts as a voice CLONE.
//
// The predicate behind two gates that must agree on every surface:
//   * speaker-consent   — CLI --i-have-rights, server "consent_attestation"
//   * Art. 50(4) audible disclosure — the spoken AI-disclosure prefix
//
// Kept weight-free and IO-free so it can be unit-tested without a model, a
// voice file, or a server process (tests/test-voice-clone-policy.cpp), for the
// same reason crispasr_marking_policy.h is: a gate nobody can test on the tier
// CI actually runs is a gate that ships wrong.
//
// WHY THIS EXISTS
// ---------------
// Both gates used to spell "is this a clone?" as `voice ends with .wav`,
// inline, in two places. That predicate was wrong in three ways, and each way
// silently produced an unattested, undisclosed clone of a real person's voice:
//
//   1. The TADA one-command clone flow bakes `victim.wav` into a temp .gguf and
//      REWRITES --voice to point at it (crispasr_run.cpp) before the gate runs.
//      The most explicit cloning command in the CLI scored as "not a clone".
//   2. chatterbox clones ONLY through a baked .gguf voice pack (there is no
//      .wav path for it at all — models/bake-chatterbox-voice-from-wav.py makes
//      the pack). A headline cloning backend could therefore never trip either
//      gate. The same held for --make-ref output and for any hand-baked pack.
//   3. The server accepts a BARE voice name resolved against --voice-dir, so
//      {"voice": "victim"} reached the same .wav file as {"voice": "victim.wav"}
//      while scoring as "not a clone".
//
// A .gguf baked from someone's recording is exactly as much a deepfake as the
// recording it was baked from. The suffix is an implementation detail.
//
// WHAT IS *NOT* A CLONE
// ---------------------
// Not every voice pack is a clone. kokoro / qwen3-tts / miotts / vibevoice ship
// synthetic or upstream-licensed preset voices as .gguf, and `tada-ref-<lang>`
// packs are shipped references. Demanding --i-have-rights for
// `--voice kokoro-voice-af_heart.gguf` would gate a preset behind a speaker-
// consent attestation nobody can meaningfully give, and would break every
// documented example. So packs are presets by default and clones when they SAY
// they are: the bakers stamp `crispasr.voice.cloned_from_recording` into packs
// they derive from a user recording, and the gate reads it back.
//
// The honest limitation: a pack baked by a CrispASR older than that stamp
// carries no provenance and reads as a preset. Re-bake it to gate it. The two
// paths where the runtime KNOWS the provenance without asking the file — an
// inline bake this run, and a recording passed directly as --voice — do not
// depend on the stamp at all.

#pragma once

#include <string>

namespace crispasr_voice {

// Case-insensitive suffix test. Local so this header stays dependency-free.
inline bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = s[s.size() - suffix.size() + i];
        const char b = suffix[i];
        const char la = (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a;
        const char lb = (b >= 'A' && b <= 'Z') ? (char)(b - 'A' + 'a') : b;
        if (la != lb)
            return false;
    }
    return true;
}

// A raw reference recording handed straight to a cloning backend.
//
// NOT just .wav: zonos accepts .mp3 and .flac references
// (crispasr_backend_zonos.cpp), and adding a decoder to another adapter is a
// one-line change that nobody would think to mirror in a consent gate. So this
// covers every container the audio loader can read, and errs toward "clone":
// a false positive costs an attestation the caller can give, a false negative
// ships an ungated clone of a real person.
inline bool is_recording_reference(const std::string& voice) {
    static const char* kAudioExt[] = {".wav", ".mp3",  ".flac", ".ogg",  ".opus", ".m4a", ".mp4",
                                      ".aac", ".aiff", ".aif",  ".webm", ".wma",  ".amr", ".caf"};
    for (const char* ext : kAudioExt)
        if (ends_with_ci(voice, ext))
            return true;
    return false;
}

// A baked voice pack. Says nothing on its own about whether it is a clone —
// see the header comment; that is what the provenance stamp is for.
inline bool is_voice_pack(const std::string& voice) {
    return ends_with_ci(voice, ".gguf");
}

// The GGUF metadata key a baker writes into a pack derived from a real
// recording. Read back by the gate; see crispasr_voice_provenance.h.
inline const char* provenance_key() {
    return "crispasr.voice.cloned_from_recording";
}

// LEGACY PACKS — packs baked before the stamp existed carry no provenance, and
// cannot be retro-stamped once published. For those, fall back to what the pack
// IS: its `general.architecture` names the producer that made it, and some
// producers only ever consume a reference recording.
//
// Listed here are the architectures whose ONLY producer in this repo takes a
// user WAV, and for which no preset pack exists to break:
//
//   chatterbox-voice     models/bake-chatterbox-voice-from-wav.py  (--input WAV)
//   qwen3tts.voicepack   models/bake-qwen3-tts-voice-pack.py       (name:wav:text)
//
// Deliberately NOT listed:
//   kokoro-voice, vibevoice-voice   — converted from upstream voicepacks/.pt,
//                                     no recording involved. Presets.
//   crispasr.reference (tada)       — AMBIGUOUS: both the shipped
//                                     tada-ref-<lang> packs and user --make-ref
//                                     output use it, and gating the shipped ones
//                                     would break `-l de` auto-download. Covered
//                                     going forward by the stamp instead; a
//                                     tada ref baked before it reads as a preset.
//   dots-tts-spk                    — a CAM++ speaker ENCODER (model weights),
//                                     never passed as --voice.
//
// This is classification by producer, not by filename: renaming a pack changes
// nothing. An unknown architecture is left as a preset — every recording-derived
// producer in this repo is enumerated above, so an unrecognised one is most
// likely a third-party or future preset, and defaulting unknown-to-clone would
// gate arbitrary packs on a guess.
inline bool architecture_is_recording_derived(const std::string& arch) {
    return arch == "chatterbox-voice" || arch == "qwen3tts.voicepack";
}

// The attestation recorded at bake time, when the baker had one. Audit trail
// only — an attestation in the pack does NOT satisfy the synthesis-time gate,
// because consent to bake and consent to publish this particular utterance are
// different questions.
inline const char* provenance_attestation_key() {
    return "crispasr.voice.consent_attestation";
}

struct CloneDecision {
    // Synthesizing with this voice clones an identifiable person: require the
    // consent attestation, prepend the spoken AI disclosure, emit [CONSENT].
    bool is_clone = false;
    // Stable token naming WHY, for the audit line and for tests. One of
    // "recording-reference" / "baked-from-wav" / "pack-provenance" /
    // "pack-architecture" / "" (none).
    const char* reason = "";
};

// `voice`                  — the --voice / "voice" value, resolved to the path
//                            the backend will actually open (bare server names
//                            resolved against --voice-dir first).
// `baked_from_wav_this_run` — this process baked `voice` from a user-supplied
//                            recording during this run (the TADA inline clone),
//                            so its .gguf suffix is an artefact of a WAV clone.
// `pack_declares_clone`     — the pack's provenance_key(), read from the file.
// `pack_architecture`       — the pack's general.architecture, for legacy packs
//                            baked before the stamp existed.
inline CloneDecision classify(const std::string& voice, bool baked_from_wav_this_run, bool pack_declares_clone,
                              const std::string& pack_architecture = std::string()) {
    CloneDecision d;
    if (voice.empty())
        return d;
    // Checked before the suffix tests: the inline-bake path rewrites `voice` to
    // a .gguf, and this flag is the only thing that still remembers it came
    // from a recording. Getting this order wrong is the original bug.
    if (baked_from_wav_this_run) {
        d.is_clone = true;
        d.reason = "baked-from-wav";
        return d;
    }
    if (is_recording_reference(voice)) {
        d.is_clone = true;
        d.reason = "recording-reference";
        return d;
    }
    if (pack_declares_clone) {
        d.is_clone = true;
        d.reason = "pack-provenance";
        return d;
    }
    // Legacy fallback: no stamp, but the producer named by the pack's
    // architecture only ever bakes from a recording.
    if (architecture_is_recording_derived(pack_architecture)) {
        d.is_clone = true;
        d.reason = "pack-architecture";
        return d;
    }
    return d;
}

} // namespace crispasr_voice
