// test-speaker-identity-gguf.cpp — the speaker-identity STAMP survives a real
// GGUF round trip.
//
// Everything in test-speaker-identity.cpp is pure: it proves the predicates and
// the table are right, with no file ever opened. That is the correct shape for
// policy, and it is exactly the shape that cannot notice a read path which is
// never reached.
//
// This project has shipped that failure before (#324: a fix landed inert
// because a silent str.replace matched nothing, and every test still passed).
// So the stamp gets a test that writes an actual GGUF, hands the path to the
// actual reader the CLI and server call, and checks the value comes back.
//
// Cheap enough for the unit tier: gguf_init_empty + four keys + a temp file, no
// model, no tensors, no network.

#include "crispasr_voice_provenance.h"

#include "ggml.h"
#include "gguf.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

using crispasr_voice::read_model_speaker_identity;
using crispasr_voice::SpeakerIdentity;

namespace {

// Write a metadata-only GGUF carrying `identity` (omitted when null), and
// return its path. Unique per case so the reader's memoisation — which is keyed
// by path — cannot leak an answer from one case into the next.
std::string write_stamped_gguf(const std::string& tag, const char* identity) {
    const std::string path = (std::filesystem::temp_directory_path() / ("crispasr-sid-" + tag + ".gguf")).string();
    gguf_context* g = gguf_init_empty();
    REQUIRE(g != nullptr);
    gguf_set_val_str(g, "general.architecture", "test-tts");
    gguf_set_val_str(g, "general.name", "stamp-fixture");
    if (identity) {
        gguf_set_val_str(g, crispasr_voice::speaker_identity_key(), identity);
    }
    REQUIRE(gguf_write_to_file(g, path.c_str(), /*only_meta=*/true));
    gguf_free(g);
    return path;
}

struct TempFile {
    std::string path;
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_CASE("a real_person stamp round-trips through a GGUF", "[unit][compliance]") {
    // The whole point of stamping: the checkpoint carries its own answer, so
    // the runtime stops having to infer it from the file name.
    TempFile f{write_stamped_gguf("realperson", "real_person")};
    REQUIRE(read_model_speaker_identity(f.path) == SpeakerIdentity::RealPerson);
}

TEST_CASE("a synthetic stamp round-trips through a GGUF", "[unit][compliance]") {
    TempFile f{write_stamped_gguf("synthetic", "synthetic")};
    REQUIRE(read_model_speaker_identity(f.path) == SpeakerIdentity::Synthetic);
}

TEST_CASE("an unstamped model reads as unknown, not as a failure", "[unit][compliance]") {
    // The normal case for every checkpoint published before the stamp existed.
    // It must fall through to the legacy table quietly, not error.
    TempFile f{write_stamped_gguf("unstamped", nullptr)};
    REQUIRE(read_model_speaker_identity(f.path) == SpeakerIdentity::Unknown);
}

TEST_CASE("a garbage stamp value is unknown, not trusted", "[unit][compliance]") {
    // Same rule as the CLI flag: a value nobody recognises must not become a
    // weaker duty than whoever wrote it intended.
    TempFile f{write_stamped_gguf("garbage", "definitely-not-a-real-person")};
    REQUIRE(read_model_speaker_identity(f.path) == SpeakerIdentity::Unknown);
}

TEST_CASE("a missing or empty path is unknown, not a crash", "[unit][compliance]") {
    // The server calls this per request with whatever --model was; it must not
    // throw on a path that has gone away.
    REQUIRE(read_model_speaker_identity("") == SpeakerIdentity::Unknown);
    REQUIRE(read_model_speaker_identity("/nonexistent/definitely-not-here.gguf") == SpeakerIdentity::Unknown);
}

TEST_CASE("a non-GGUF file is unknown, not a crash", "[unit][compliance]") {
    const std::string path = (std::filesystem::temp_directory_path() / "crispasr-sid-notgguf.bin").string();
    {
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        REQUIRE(fp != nullptr);
        std::fputs("this is not a gguf file", fp);
        std::fclose(fp);
    }
    TempFile f{path};
    REQUIRE(read_model_speaker_identity(f.path) == SpeakerIdentity::Unknown);
}

TEST_CASE("the stamp reaches the resolver and produces a disclosure", "[unit][compliance]") {
    // End to end through the join the surfaces actually make: stamped file ->
    // reader -> resolver -> duty. A reader that works while nothing consults it
    // is the inert-fix failure mode this file exists to rule out.
    TempFile f{write_stamped_gguf("endtoend", "real_person")};
    const SpeakerIdentity resolved = crispasr_voice::resolve_speaker_identity(
        /*override=*/SpeakerIdentity::Unknown, /*pack=*/SpeakerIdentity::Unknown,
        /*backend=*/SpeakerIdentity::Unknown, read_model_speaker_identity(f.path));
    REQUIRE(resolved == SpeakerIdentity::RealPerson);
    REQUIRE(crispasr_voice::requires_spoken_disclosure(/*is_clone=*/false, resolved));
    // ...and it does NOT drag the consent gate along with it.
    REQUIRE_FALSE(crispasr_voice::requires_consent_attestation(false, resolved));
}

TEST_CASE("the memoised reader returns the same answer twice", "[unit][compliance]") {
    // The cache is an optimisation for the server's hot path; it must not be
    // able to change an answer. Also covers the case where the file disappears
    // between calls — a served request should not start disclosing differently
    // because someone tidied a model directory mid-run.
    const std::string path = write_stamped_gguf("memoised", "real_person");
    REQUIRE(read_model_speaker_identity(path) == SpeakerIdentity::RealPerson);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(read_model_speaker_identity(path) == SpeakerIdentity::RealPerson);
}
