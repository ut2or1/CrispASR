#include "core/audio_resample.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

float interior_mean(const std::vector<float>& samples) {
    constexpr size_t edge = 128;
    REQUIRE(samples.size() > 2 * edge);

    double sum = 0.0;
    for (size_t i = edge; i < samples.size() - edge; ++i) {
        sum += samples[i];
    }
    return static_cast<float>(sum / static_cast<double>(samples.size() - 2 * edge));
}

// Largest interior deviation from `level`. The mean alone cannot see ripple:
// a filter whose taps are half missing still averages close to the right
// value while every individual sample wobbles, so assert the worst sample.
float interior_max_dev(const std::vector<float>& samples, float level) {
    constexpr size_t edge = 128;
    REQUIRE(samples.size() > 2 * edge);

    double worst = 0.0;
    for (size_t i = edge; i < samples.size() - edge; ++i) {
        worst = std::max(worst, std::abs((double)samples[i] - (double)level));
    }
    return static_cast<float>(worst);
}

} // namespace

TEST_CASE("polyphase resampling preserves DC gain", "[unit][audio-resample]") {
    constexpr float level = 0.25f;
    const std::vector<float> input(48000, level);

    SECTION("48 kHz to 24 kHz") {
        const auto output = core_audio::resample_polyphase(input.data(), static_cast<int>(input.size()), 48000, 24000);
        REQUIRE(output.size() == 24000);
        REQUIRE(interior_mean(output) == Catch::Approx(level).margin(5e-4f));
    }

    SECTION("24 kHz to 16 kHz") {
        const auto output = core_audio::resample_polyphase(input.data(), static_cast<int>(input.size()), 24000, 16000);
        REQUIRE(output.size() == 32000);
        REQUIRE(interior_mean(output) == Catch::Approx(level).margin(5e-4f));
    }

    SECTION("24 kHz to 48 kHz") {
        const auto output = core_audio::resample_polyphase(input.data(), static_cast<int>(input.size()), 24000, 48000);
        REQUIRE(output.size() == 96000);
        REQUIRE(interior_mean(output) == Catch::Approx(level).margin(5e-4f));
    }

    // The source-separation surface (--separate) resamples the separator's
    // 44.1 kHz stems against 48 kHz host audio. This is the non-trivial
    // downsample (gcd 300 → L=147, M=160) that the up=max(L,M) bug silently
    // over-attenuated, so guard it explicitly.
    SECTION("48 kHz to 44.1 kHz") {
        const auto output = core_audio::resample_polyphase(input.data(), static_cast<int>(input.size()), 48000, 44100);
        REQUIRE(output.size() == 44100);
        REQUIRE(interior_mean(output) == Catch::Approx(level).margin(5e-4f));
    }
}

// #334. A constant input must come out constant: the polyphase sum for every
// output phase is a slice of the SAME Kaiser-windowed sinc, so with the whole
// filter applied each phase sums to 1/L and the ×L compensation makes every
// output sample exactly `level` — no averaging, no window, exact arithmetic.
//
// The per-sample form is the point. `resample_polyphase` walked the input
// window as `j_center ± num_zeros`, but the filter spans `half_len/L =
// num_zeros * max(L, M) / L` INPUT samples, so whenever M > L (any
// downsample) the outer taps were never visited. The DC mean absorbed that
// (0.9984 vs 1.0, inside the 5e-4 margin above — a tolerance wider than the
// defect), while the per-phase sums each landed somewhere different, which is
// ripple. Assert the worst sample, at a margin an order of magnitude below
// the defect it is guarding.
TEST_CASE("polyphase resampling is ripple-free on a constant input", "[unit][audio-resample]") {
    constexpr float level = 0.25f;
    constexpr float kMaxDev = 5e-5f; // buggy build: 5.4e-4 at 24→16 kHz — 10x this margin
    const std::vector<float> input(48000, level);

    struct Case {
        int src;
        int dst;
    };
    // Downsamples (M > L) are the truncated ones; the upsamples are the
    // control — they were already exact and must stay that way.
    const Case cases[] = {
        {24000, 16000}, {48000, 16000}, {32000, 16000}, {44100, 16000}, {48000, 24000},
        {22050, 16000}, {16000, 24000}, {8000, 16000},  {24000, 48000},
    };
    for (const Case& c : cases) {
        CAPTURE(c.src, c.dst);
        const auto output = core_audio::resample_polyphase(input.data(), static_cast<int>(input.size()), c.src, c.dst);
        REQUIRE(output.size() > 256);
        REQUIRE(interior_max_dev(output, level) < kMaxDev);
    }
}
