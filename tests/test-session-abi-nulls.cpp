// test-session-abi-nulls.cpp — model-free contracts of the session C-ABI (#332).
//
// Two families, both runnable with no model and no audio fixture:
//
//   1. NULL-session getters return their documented "no data" values instead
//      of dereferencing. These are the first calls every binding makes; a
//      crash here takes down the host VM (JVM / Dart isolate / Ruby), so the
//      NULL contract is load-bearing, not defensive decoration.
//   2. crispasr_diarize_segments_abi argument validation and the model-free
//      diarize methods (Energy / VadTurns), exercised through the SAME
//      48-byte options mirror the language bindings hand-maintain. This is
//      deliberately a fourth mirror of the append-only layout: if the C
//      struct grows and this file is not updated in the same commit, the
//      layout static_asserts in crispasr_c_api.cpp fail the build before
//      this test can even lie about passing (#324's miss, #332's fix).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

extern "C" {
struct crispasr_session;
int crispasr_session_input_sample_rate(crispasr_session* s);
int crispasr_session_output_sample_rate(crispasr_session* s);
int crispasr_session_input_channels(crispasr_session* s);
int crispasr_session_output_channels(crispasr_session* s);
int crispasr_session_set_pcm_sample_rate(crispasr_session* s, int rate);

// Hand-written mirror of the append-only ABI structs, byte-identical to the
// ones in crispasr_c_api.cpp (pinned there by static_asserts) and to the
// mirrors in crispasr-sys, flutter and the Go cgo preamble.
struct diarize_seg_abi {
    int64_t t0_cs;
    int64_t t1_cs;
    int32_t speaker;
    int32_t _pad;
};
struct diarize_opts_abi {
    int32_t method;
    int32_t n_threads;
    int64_t slice_t0_cs;
    const char* pyannote_model_path;
    const char* foxnose_embedder_path;
    int32_t min_speakers;
    int32_t max_speakers;
    int32_t num_speakers;
    int32_t _pad2;
};
int crispasr_diarize_segments_abi(const float* left_pcm, const float* right_pcm, int32_t n_samples, int32_t is_stereo,
                                  diarize_seg_abi* segs, int32_t n_segs, const diarize_opts_abi* opts);
}

namespace {

diarize_opts_abi default_opts(int method) {
    diarize_opts_abi o = {};
    o.method = method;
    o.n_threads = 1;
    return o;
}

} // namespace

TEST_CASE("NULL session: every #332 getter returns its no-data value", "[unit][abi]") {
    CHECK(crispasr_session_input_sample_rate(nullptr) == 0);
    CHECK(crispasr_session_output_sample_rate(nullptr) == 0);
    CHECK(crispasr_session_input_channels(nullptr) == 0);
    CHECK(crispasr_session_output_channels(nullptr) == 0);
    CHECK(crispasr_session_set_pcm_sample_rate(nullptr, 16000) == -1);
}

TEST_CASE("diarize ABI rejects invalid arguments with -1", "[unit][diarize]") {
    std::vector<float> pcm(16000, 0.01f);
    diarize_seg_abi seg = {0, 100, -1, 0};
    diarize_opts_abi opts = default_opts(2); // VadTurns

    CHECK(crispasr_diarize_segments_abi(nullptr, nullptr, 16000, 0, &seg, 1, &opts) == -1);
    CHECK(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, nullptr, 1, &opts) == -1);
    CHECK(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, &seg, 0, &opts) == -1);
    CHECK(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, &seg, 1, nullptr) == -1);

    diarize_opts_abi bad_method = default_opts(5); // one past FoxNose
    CHECK(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, &seg, 1, &bad_method) == -1);
    bad_method.method = -1;
    CHECK(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, &seg, 1, &bad_method) == -1);
}

TEST_CASE("VadTurns alternates speakers across a >600 ms gap", "[unit][diarize]") {
    std::vector<float> pcm(16000 * 4, 0.01f);
    diarize_seg_abi segs[2] = {
        {0, 100, -1, 0},   // 0.0 – 1.0 s
        {200, 300, -1, 0}, // 2.0 – 3.0 s: 1 s gap
    };
    diarize_opts_abi opts = default_opts(2);
    REQUIRE(crispasr_diarize_segments_abi(pcm.data(), nullptr, (int32_t)pcm.size(), 0, segs, 2, &opts) == 0);
    CHECK(segs[0].speaker != segs[1].speaker);
    CHECK(segs[0].speaker >= 0);
    CHECK(segs[1].speaker >= 0);
}

TEST_CASE("Energy picks the louder channel; ties stay unlabelled", "[unit][diarize]") {
    // Segment 0 (0–1 s): left loud, right quiet. Segment 1 (1–2 s): reversed.
    // Segment 2 (2–3 s): equal energy — inside the 1.1x margin, so -1.
    const int sr = 16000;
    std::vector<float> left(sr * 3, 0.0f), right(sr * 3, 0.0f);
    for (int i = 0; i < sr; i++) {
        left[i] = 0.5f;
        right[i] = 0.05f;
        left[sr + i] = 0.05f;
        right[sr + i] = 0.5f;
        left[2 * sr + i] = 0.3f;
        right[2 * sr + i] = 0.3f;
    }
    diarize_seg_abi segs[3] = {{0, 100, -1, 0}, {100, 200, -1, 0}, {200, 300, -1, 0}};
    diarize_opts_abi opts = default_opts(0); // Energy
    REQUIRE(crispasr_diarize_segments_abi(left.data(), right.data(), sr * 3, 1, segs, 3, &opts) == 0);
    CHECK(segs[0].speaker == 0);
    CHECK(segs[1].speaker == 1);
    CHECK(segs[2].speaker == -1);
}

TEST_CASE("Energy on mono input succeeds but labels nothing", "[unit][diarize]") {
    std::vector<float> pcm(16000, 0.2f);
    diarize_seg_abi seg = {0, 100, -1, 0};
    diarize_opts_abi opts = default_opts(0);
    REQUIRE(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, &seg, 1, &opts) == 0);
    CHECK(seg.speaker == -1);
}

TEST_CASE("slice_t0 maps absolute segment times into the buffer", "[unit][diarize]") {
    // The buffer starts at absolute t = 10 s. A segment at 10–11 s must land
    // on samples [0, 16000) — labelled from the left-loud first second — and
    // one at 11–12 s on the right-loud second.
    const int sr = 16000;
    std::vector<float> left(sr * 2, 0.0f), right(sr * 2, 0.0f);
    for (int i = 0; i < sr; i++) {
        left[i] = 0.5f;
        right[i] = 0.05f;
        left[sr + i] = 0.05f;
        right[sr + i] = 0.5f;
    }
    diarize_seg_abi segs[2] = {{1000, 1100, -1, 0}, {1100, 1200, -1, 0}};
    diarize_opts_abi opts = default_opts(0);
    opts.slice_t0_cs = 1000;
    REQUIRE(crispasr_diarize_segments_abi(left.data(), right.data(), sr * 2, 1, segs, 2, &opts) == 0);
    CHECK(segs[0].speaker == 0);
    CHECK(segs[1].speaker == 1);
}

TEST_CASE("FoxNose with a missing embedder fails with 1, not a crash", "[unit][diarize]") {
    std::vector<float> pcm(16000, 0.01f);
    diarize_seg_abi seg = {0, 100, -1, 0};
    diarize_opts_abi opts = default_opts(4); // FoxNose
    opts.foxnose_embedder_path = "/nonexistent/wespeaker.gguf";
    CHECK(crispasr_diarize_segments_abi(pcm.data(), nullptr, 16000, 0, &seg, 1, &opts) == 1);
}
