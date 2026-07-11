// test-generation-health.cpp — unit tests for the generation-health regression gate.
// Tests the objective quality checks in core/generation_health.h without needing
// any model GGUF — pure string/numeric logic.

#include <catch2/catch_test_macros.hpp>
#include "core/generation_health.h"

using namespace core_generation_health;

// ---- check_not_empty ----

TEST_CASE("gen-health: non-empty text passes", "[unit][gen-health]") {
    auto r = check_not_empty("Hello world");
    REQUIRE(r.pass);
}

TEST_CASE("gen-health: empty string fails", "[unit][gen-health]") {
    auto r = check_not_empty("");
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("empty") != std::string::npos);
}

TEST_CASE("gen-health: null fails", "[unit][gen-health]") {
    auto r = check_not_empty(nullptr);
    REQUIRE_FALSE(r.pass);
}

TEST_CASE("gen-health: whitespace-only fails", "[unit][gen-health]") {
    auto r = check_not_empty("   \n\t  ");
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("whitespace") != std::string::npos);
}

// ---- check_duration_plausibility ----

TEST_CASE("gen-health: plausible duration passes", "[unit][gen-health]") {
    // "And so my fellow Americans" ≈ 26 chars, 11s audio → ~2.4 chars/sec
    auto r = check_duration_plausibility("And so my fellow Americans", 11.0f);
    REQUIRE(r.pass);
}

TEST_CASE("gen-health: too-short transcript fails", "[unit][gen-health]") {
    // 2 chars for 60s audio → 0.03 chars/sec
    auto r = check_duration_plausibility("Hi", 60.0f);
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("too short") != std::string::npos);
}

TEST_CASE("gen-health: too-long transcript fails", "[unit][gen-health]") {
    // 5000 chars for 1s audio → 5000 chars/sec
    std::string long_text(5000, 'x');
    auto r = check_duration_plausibility(long_text.c_str(), 1.0f);
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("too long") != std::string::npos);
}

// ---- check_no_ngram_loop ----

TEST_CASE("gen-health: normal text has no loop", "[unit][gen-health]") {
    auto r = check_no_ngram_loop("And so my fellow Americans ask not what your country can do for you");
    REQUIRE(r.pass);
}

TEST_CASE("gen-health: repeated word triggers loop", "[unit][gen-health]") {
    auto r = check_no_ngram_loop("hey hey hey hey hey hey this is bad");
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("hey") != std::string::npos);
}

TEST_CASE("gen-health: short repetition is OK", "[unit][gen-health]") {
    // 3 repeats is under the default threshold of 4
    auto r = check_no_ngram_loop("yes yes yes no");
    REQUIRE(r.pass);
}

TEST_CASE("gen-health: custom repeat threshold", "[unit][gen-health]") {
    auto r = check_no_ngram_loop("ok ok ok", /*max_consecutive_repeats=*/2);
    REQUIRE_FALSE(r.pass);
}

// ---- check_not_truncated ----

TEST_CASE("gen-health: within max_tokens passes", "[unit][gen-health]") {
    auto r = check_not_truncated(42, 512);
    REQUIRE(r.pass);
}

TEST_CASE("gen-health: at max_tokens fails", "[unit][gen-health]") {
    auto r = check_not_truncated(512, 512);
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("truncated") != std::string::npos);
}

// ---- check_tts_duration ----

TEST_CASE("gen-health: plausible TTS duration passes", "[unit][gen-health]") {
    // "Hello world" = 2 words, 0.48s at 48kHz = 23040 samples → 0.24 sec/word
    auto r = check_tts_duration(23040, 48000, 2);
    REQUIRE(r.pass);
}

TEST_CASE("gen-health: TTS too short fails", "[unit][gen-health]") {
    // 2 words, 480 samples at 48kHz = 0.01s → 0.005 sec/word
    auto r = check_tts_duration(480, 48000, 2);
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("too short") != std::string::npos);
}

TEST_CASE("gen-health: TTS too long fails", "[unit][gen-health]") {
    // 1 word, 480000 samples at 48kHz = 10s → 10 sec/word
    auto r = check_tts_duration(480000, 48000, 1);
    REQUIRE_FALSE(r.pass);
    REQUIRE(r.detail.find("too long") != std::string::npos);
}
