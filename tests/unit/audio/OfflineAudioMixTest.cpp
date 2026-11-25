// Export sound (OfflineAudioMix): a 1 s clip with a known sound is written at test time
// with Mp4Writer, and the mix is compared with decoding that file straight through.

#include <gtest/gtest.h>

#include "audio/AudioDecoder.hpp"
#include "audio/AudioFrame.hpp"
#include "audio/OfflineAudioMix.hpp"
#include "video/Mp4Writer.hpp"

#include "../TestPlatform.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int RATE = 48000;
constexpr int FPS = 30;
constexpr double CLIP_SECONDS = 1.0;
constexpr double PI = 3.14159265358979323846;
constexpr size_t FRAMES = 2400;  // 50 ms per check
constexpr float TOLERANCE = 2e-3f;

// A tone whose level rises over the clip, so every position sounds different (a steady
// tone repeats and would hide a wrong offset). 440.25 Hz ends the clip at a peak, so a
// Loop wrap without a crossfade would step from 0.8 to 0.
float sourceSample(int64_t i, int ch) {
    const double t = static_cast<double>(i) / RATE;
    const double level = 0.2 + 0.6 * t;
    const double s = level * std::sin(2.0 * PI * 440.25 * t);
    return static_cast<float>(ch == 0 ? s : -0.5 * s);
}

bool writeFixture(const fs::path& path, bool audio, std::string& error) {
    Mp4Writer writer;
    Mp4WriterOptions options;
    options.width = 64;
    options.height = 64;
    options.fps = FPS;
    options.audio = audio;
    if (!writer.open(path, options, error)) return false;
    const std::vector<uint8_t> frame(64 * 64 * 4, 128);
    const int perFrame = RATE / FPS;
    std::vector<float> pcm(static_cast<size_t>(perFrame) * 2);
    for (int f = 0; f < static_cast<int>(CLIP_SECONDS * FPS); ++f) {
        if (!writer.writeVideoFrame(frame.data(), 64 * 4, false)) return false;
        if (!audio) continue;
        for (int i = 0; i < perFrame; ++i) {
            const int64_t n = static_cast<int64_t>(f) * perFrame + i;
            pcm[i * 2] = sourceSample(n, 0);
            pcm[i * 2 + 1] = sourceSample(n, 1);
        }
        if (!writer.writeAudio(pcm.data(), perFrame)) return false;
    }
    // The sound runs 0.1 s past the picture, as in real files: what a Loop wrap fades out.
    if (audio) {
        const int extra = RATE / 10;
        std::vector<float> tail(static_cast<size_t>(extra) * 2);
        const int64_t start = static_cast<int64_t>(CLIP_SECONDS * RATE);
        for (int i = 0; i < extra; ++i) {
            tail[i * 2] = sourceSample(start + i, 0);
            tail[i * 2 + 1] = sourceSample(start + i, 1);
        }
        if (!writer.writeAudio(tail.data(), extra)) return false;
    }
    fs::path written;
    return writer.finish(written, error);
}

class OfflineAudioMixTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        folder_ = fs::temp_directory_path() / ("qm_offlinemix_" + std::to_string(testProcessId()));
        fs::create_directories(folder_);
        std::string error;
        ready_ = writeFixture(clipPath(), true, error) && writeFixture(silentPath(), false, error);
        if (!ready_) {
            error_ = error;
            return;
        }
        // The reference: the whole clip decoded straight through, indexed by sample.
        AudioDecoder decoder;
        ready_ = decoder.open(clipPath(), RATE, 2);
        bool first = true;
        for (AudioFrame frame = decoder.decodeNextFrame(); frame.isValid(); frame = decoder.decodeNextFrame()) {
            if (first) {
                referenceStart_ = std::llround(frame.pts * RATE);
                first = false;
            }
            reference_.insert(reference_.end(), frame.samples.begin(), frame.samples.end());
        }
    }
    static void TearDownTestSuite() {
        std::error_code ec;
        fs::remove_all(folder_, ec);
        reference_.clear();
    }
    void SetUp() override {
        if (!ready_) GTEST_SKIP() << "fixture could not be written: " << error_;
    }

    static fs::path clipPath() { return folder_ / "tone.mp4"; }
    static fs::path silentPath() { return folder_ / "picture_only.mp4"; }

    // The decoded file at clip time `seconds`, `frames` frames, times `gain`.
    static std::vector<float> expected(double seconds, size_t frames, float gain = 1.0f) {
        std::vector<float> out(frames * 2, 0.0f);
        const int64_t first = std::llround(seconds * RATE) - referenceStart_;
        for (size_t i = 0; i < frames * 2; ++i) {
            const int64_t index = first * 2 + static_cast<int64_t>(i);
            if (index >= 0 && index < static_cast<int64_t>(reference_.size())) out[i] = reference_[index] * gain;
        }
        return out;
    }

    static OfflineAudioClip clip(LoopMode mode, float speed = 1.0f, float gain = 1.0f) {
        OfflineAudioClip c;
        c.path = clipPath();
        c.duration = CLIP_SECONDS;
        c.loopMode = mode;
        c.speed = speed;
        c.gain = gain;
        return c;
    }

    static std::vector<float> render(OfflineAudioMix& mix, double t0, size_t frames = FRAMES) {
        std::vector<float> out(frames * 2, 7.0f);  // render must overwrite
        mix.render(t0, frames, out.data());
        return out;
    }

    static float peak(const std::vector<float>& v) {
        float p = 0.0f;
        for (float s : v) p = std::max(p, std::abs(s));
        return p;
    }

    static float maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
        float d = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(a[i] - b[i]));
        return d;
    }

    static inline fs::path folder_;
    static inline bool ready_ = false;
    static inline std::string error_;
    static inline std::vector<float> reference_;
    static inline int64_t referenceStart_ = 0;
};

}  // namespace

TEST_F(OfflineAudioMixTest, ReferenceHoldsTheWrittenSound) {
    // Guards the fixture: the decoded file is the rising tone, not silence or noise.
    const std::vector<float> ref = expected(0.5, FRAMES);
    std::vector<float> source(FRAMES * 2);
    for (size_t i = 0; i < FRAMES; ++i) {
        source[i * 2] = sourceSample(static_cast<int64_t>(0.5 * RATE) + static_cast<int64_t>(i), 0);
        source[i * 2 + 1] = sourceSample(static_cast<int64_t>(0.5 * RATE) + static_cast<int64_t>(i), 1);
    }
    EXPECT_LT(maxDifference(ref, source), 0.05f);  // AAC at 192 kb/s
    EXPECT_GT(peak(ref), 0.4f);
}

TEST_F(OfflineAudioMixTest, FileWithoutSoundIsRefused) {
    OfflineAudioMix mix;
    OfflineAudioClip c = clip(LoopMode::LOOP);
    c.path = silentPath();
    std::string error;
    EXPECT_FALSE(mix.addClip(c, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(mix.clipCount(), 0u);
}

TEST_F(OfflineAudioMixTest, LoopWrapsToTheClipPosition) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP), error)) << error;
    const std::vector<float> out = render(mix, 1.25);
    EXPECT_LT(maxDifference(out, expected(0.25, FRAMES)), TOLERANCE);
    // Not merely "some sound": the clip at 0.75 s is different.
    EXPECT_GT(maxDifference(out, expected(0.75, FRAMES)), 0.05f);
}

TEST_F(OfflineAudioMixTest, OnceIsSilentAfterTheEnd) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::ONCE), error)) << error;
    EXPECT_LT(maxDifference(render(mix, 0.25), expected(0.25, FRAMES)), TOLERANCE);
    EXPECT_EQ(peak(render(mix, 1.2)), 0.0f);
    EXPECT_EQ(peak(render(mix, 5.0)), 0.0f);
}

TEST_F(OfflineAudioMixTest, SpeedOtherThanOneIsSilent) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP, 2.0f), error)) << error;
    EXPECT_EQ(peak(render(mix, 0.25)), 0.0f);
}

TEST_F(OfflineAudioMixTest, PingPongReverseLegIsSilent) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::PING_PONG), error)) << error;
    EXPECT_LT(maxDifference(render(mix, 0.25), expected(0.25, FRAMES)), TOLERANCE);
    EXPECT_EQ(peak(render(mix, 1.25)), 0.0f);  // second leg runs backwards
    EXPECT_LT(maxDifference(render(mix, 2.25), expected(0.25, FRAMES)), TOLERANCE);  // forward again
}

TEST_F(OfflineAudioMixTest, GainScales) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP, 1.0f, 0.5f), error)) << error;
    EXPECT_LT(maxDifference(render(mix, 0.5), expected(0.5, FRAMES, 0.5f)), TOLERANCE);
}

TEST_F(OfflineAudioMixTest, ClipsSumAndAreClippedToFullScale) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP), error)) << error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP), error)) << error;
    EXPECT_EQ(mix.clipCount(), 2u);
    const std::vector<float> out = render(mix, 0.8);
    std::vector<float> want = expected(0.8, FRAMES, 2.0f);
    bool clipped = false;
    for (float& s : want) {
        clipped = clipped || std::abs(s) > 1.0f;
        s = std::clamp(s, -1.0f, 1.0f);
    }
    ASSERT_TRUE(clipped) << "the check needs a sum above full scale";
    EXPECT_LT(maxDifference(out, want), 2.0f * TOLERANCE);
    EXPECT_LE(peak(out), 1.0f);
}

TEST_F(OfflineAudioMixTest, NonContiguousRenderAfterContiguousOnes) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP), error)) << error;
    // Contiguous blocks of 1024 frames (not a divisor of anything in the clip).
    constexpr size_t BLOCK = 1024;
    for (size_t b = 0; b < 30; ++b) {
        const double t0 = static_cast<double>(b * BLOCK) / RATE;
        ASSERT_LT(maxDifference(render(mix, t0, BLOCK), expected(t0, BLOCK)), TOLERANCE) << "block " << b;
    }
    EXPECT_LT(maxDifference(render(mix, 0.1), expected(0.1, FRAMES)), TOLERANCE);  // back
    EXPECT_LT(maxDifference(render(mix, 3.9), expected(0.9, FRAMES)), TOLERANCE);  // far ahead
}

TEST_F(OfflineAudioMixTest, LoopWrapHasNoStep) {
    OfflineAudioMix mix;
    std::string error;
    ASSERT_TRUE(mix.addClip(clip(LoopMode::LOOP), error)) << error;
    // 20 ms either side of the first wrap.
    const std::vector<float> out = render(mix, CLIP_SECONDS - 0.02, 1920);
    float largestStep = 0.0f;
    for (size_t i = 1; i < 1920; ++i) largestStep = std::max(largestStep, std::abs(out[i * 2] - out[(i - 1) * 2]));
    // The tone moves at most 2 pi 440 / 48000 x 0.85 = 0.05 a sample; a hard cut from
    // the peak at the end to the start would jump by about 0.8.
    EXPECT_LT(largestStep, 0.1f);
    // Past the fade, the new pass is the clip's start.
    const std::vector<float> after(out.begin() + (960 + 480) * 2, out.end());
    EXPECT_LT(maxDifference(after, expected(0.01, 480)), TOLERANCE);
}
