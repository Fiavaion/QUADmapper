#include <gtest/gtest.h>
#include "audio/AudioDecoder.hpp"
#include "audio/AudioFrame.hpp"
#include "AudioTestMedia.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace quadmapper {
namespace test {

class AudioDecoderTest : public ::testing::Test {
protected:
    AudioDecoder decoder;
};

TEST_F(AudioDecoderTest, DefaultState) {
    EXPECT_FALSE(decoder.isOpen());
    EXPECT_TRUE(decoder.isAtEnd());
    EXPECT_EQ(decoder.getOutputSampleRate(), 0);
    EXPECT_EQ(decoder.getOutputChannels(), 0);
}

TEST_F(AudioDecoderTest, OpenNonExistentFile) {
    EXPECT_FALSE(decoder.open("nonexistent_file.mp4"));
    EXPECT_FALSE(decoder.isOpen());
}

TEST_F(AudioDecoderTest, OpenInvalidPath) {
    EXPECT_FALSE(decoder.open(""));
    EXPECT_FALSE(decoder.isOpen());
}

TEST_F(AudioDecoderTest, OpenDirectoryFails) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    EXPECT_FALSE(decoder.open(tempDir));
    EXPECT_FALSE(decoder.isOpen());
}

TEST_F(AudioDecoderTest, CloseWithoutOpen) {
    EXPECT_NO_THROW(decoder.close());
    EXPECT_FALSE(decoder.isOpen());
}

TEST_F(AudioDecoderTest, DoubleClose) {
    decoder.close();
    EXPECT_NO_THROW(decoder.close());
}

TEST_F(AudioDecoderTest, DecodeWithoutOpen) {
    AudioFrame frame = decoder.decodeNextFrame();
    EXPECT_FALSE(frame.isValid());
}

TEST_F(AudioDecoderTest, SeekWithoutOpen) {
    EXPECT_FALSE(decoder.seek(1.0));
}

TEST_F(AudioDecoderTest, GetInfoWithoutOpen) {
    const AudioInfo& info = decoder.getInfo();
    EXPECT_FALSE(info.isValid());
    EXPECT_EQ(info.channels, 0);
    EXPECT_EQ(info.sampleRate, 0);
}

TEST_F(AudioDecoderTest, GetCurrentTimeWithoutOpen) {
    EXPECT_DOUBLE_EQ(decoder.getCurrentTime(), 0.0);
}

TEST_F(AudioDecoderTest, MoveConstruction) {
    AudioDecoder decoder1;
    AudioDecoder decoder2(std::move(decoder1));
    EXPECT_FALSE(decoder2.isOpen());
}

TEST_F(AudioDecoderTest, MoveAssignment) {
    AudioDecoder decoder1;
    AudioDecoder decoder2;
    decoder2 = std::move(decoder1);
    EXPECT_FALSE(decoder2.isOpen());
}

// ---- Real media (D:\testing Videos; skipped when absent) --------------------

using audio_test::mediaPath;

// A whole stream, decoded; timestamps are checked for continuity on the way.
struct DecodedStream {
    std::vector<float> samples;
    int channels = 0;
    int64_t frames = 0;
    double firstPts = -1.0;
    double maxGap = 0.0;  // largest |pts - expected| between consecutive frames, seconds
};

static DecodedStream decodeAll(AudioDecoder& decoder) {
    DecodedStream out;
    double expected = -1.0;
    for (AudioFrame frame = decoder.decodeNextFrame(); frame.isValid(); frame = decoder.decodeNextFrame()) {
        if (out.firstPts < 0.0) out.firstPts = frame.pts;
        if (expected >= 0.0) out.maxGap = std::max(out.maxGap, std::abs(frame.pts - expected));
        expected = frame.pts + frame.duration();
        out.channels = frame.channels;
        out.frames += frame.sampleCount;
        out.samples.insert(out.samples.end(), frame.samples.begin(), frame.samples.end());
    }
    return out;
}

TEST(AudioDecoderMediaTest, OpensTheAudioOfAVideo) {
    const auto clip = mediaPath("5sec.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder decoder;
    ASSERT_TRUE(decoder.open(clip));
    const AudioInfo& info = decoder.getInfo();
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.sampleRate, 48000);
    EXPECT_EQ(info.codecName, "aac");
    EXPECT_NEAR(info.duration, 5.0, 0.05);
    EXPECT_EQ(decoder.getOutputChannels(), 2);  // 0 = keep the stream's format
    EXPECT_EQ(decoder.getOutputSampleRate(), 48000);
}

TEST(AudioDecoderMediaTest, DecodesTheWholeStreamContiguously) {
    const auto clip = mediaPath("5sec.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder decoder;
    ASSERT_TRUE(decoder.open(clip, 48000, 2));
    const DecodedStream stream = decodeAll(decoder);
    EXPECT_TRUE(decoder.isAtEnd());
    EXPECT_EQ(stream.channels, 2);
    EXPECT_NEAR(static_cast<double>(stream.frames) / 48000.0, 5.0, 0.03);
    EXPECT_NEAR(stream.firstPts, 0.0, 0.001);
    EXPECT_LT(stream.maxGap, 0.001);
    for (float v : stream.samples) ASSERT_TRUE(std::isfinite(v));
}

TEST(AudioDecoderMediaTest, ResamplesToTheRequestedRate) {
    const auto clip = mediaPath("5sec.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder decoder;
    ASSERT_TRUE(decoder.open(clip, 44100, 1));
    const DecodedStream stream = decodeAll(decoder);
    EXPECT_EQ(stream.channels, 1);
    EXPECT_NEAR(static_cast<double>(stream.frames) / 44100.0, 5.0, 0.03);
    EXPECT_LT(stream.maxGap, 0.002);
}

TEST(AudioDecoderMediaTest, SeekLandsAtOrJustBeforeTheTarget) {
    const auto clip = mediaPath("5sec.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder decoder;
    ASSERT_TRUE(decoder.open(clip, 48000, 2));
    for (double target : {2.5, 0.0, 4.2, 1.0}) {
        ASSERT_TRUE(decoder.seek(target));
        const AudioFrame frame = decoder.decodeNextFrame();
        ASSERT_TRUE(frame.isValid()) << target;
        EXPECT_LE(frame.pts, target + 1e-6) << target;
        EXPECT_GT(frame.pts + frame.duration(), target - 0.2) << target;  // within a packet or two
    }
    // Seeking back after the end starts over.
    decodeAll(decoder);
    ASSERT_TRUE(decoder.isAtEnd());
    ASSERT_TRUE(decoder.seek(0.0));
    EXPECT_FALSE(decoder.isAtEnd());
    EXPECT_TRUE(decoder.decodeNextFrame().isValid());
}

// After a seek the sound must be what a decode from the start gives at the same time:
// an AAC packet decoded straight after a flush lacks its overlap with the one before
// (the first ~10 ms come out wrong), so seek() pre-rolls. Checked at 0 (where the
// priming packet is the pre-roll) and mid-file.
TEST(AudioDecoderMediaTest, SoundAfterASeekMatchesAContinuousDecode) {
    const auto clip = mediaPath("keying woman.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder continuous;
    ASSERT_TRUE(continuous.open(clip, 48000, 2));
    std::vector<AudioFrame> frames;
    for (AudioFrame f = continuous.decodeNextFrame(); f.isValid() && f.pts < 6.0; f = continuous.decodeNextFrame()) {
        frames.push_back(std::move(f));
    }
    ASSERT_GT(frames.size(), 100u);

    AudioDecoder seeking;
    ASSERT_TRUE(seeking.open(clip, 48000, 2));
    for (double target : {0.0, 3.0, 1.5}) {
        SCOPED_TRACE(target);
        ASSERT_TRUE(seeking.seek(target));
        int compared = 0;
        for (AudioFrame f = seeking.decodeNextFrame(); f.isValid() && f.pts < target + 0.5;
             f = seeking.decodeNextFrame()) {
            if (f.pts + f.duration() <= target) continue;  // pre-roll, trimmed by callers
            const auto same = std::find_if(frames.begin(), frames.end(), [&](const AudioFrame& c) {
                return std::abs(c.pts - f.pts) < 1e-6;
            });
            ASSERT_NE(same, frames.end()) << "no continuous frame at pts " << f.pts;
            ASSERT_EQ(same->samples.size(), f.samples.size());
            float worst = 0.0f;
            for (size_t i = 0; i < f.samples.size(); ++i) {
                worst = std::max(worst, std::abs(f.samples[i] - same->samples[i]));
            }
            EXPECT_LT(worst, 1e-4f) << "frame at " << f.pts;
            ++compared;
        }
        EXPECT_GT(compared, 15);
    }
}

TEST(AudioDecoderMediaTest, VideoWithoutAudioHasNoStream) {
    const auto clip = mediaPath("4k_10Sec_Timecode.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder decoder;
    EXPECT_FALSE(decoder.open(clip, 48000, 2));
    EXPECT_FALSE(decoder.isOpen());
}

TEST(AudioDecoderMediaTest, OpensTheAudioOfAHapMovie) {
    const auto clip = mediaPath("Hap/5sec_hap.mov");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder decoder;
    ASSERT_TRUE(decoder.open(clip, 48000, 2));
    const DecodedStream stream = decodeAll(decoder);
    EXPECT_NEAR(static_cast<double>(stream.frames) / 48000.0, 5.01, 0.05);
}

// 7.1 to stereo: swresample's matrix puts the left-side channels (FL, SL, BL) on the
// left, the right-side ones on the right, the centre on both, and leaves the LFE out.
// The audition file speaks one channel at a time; second by second, compare which
// source channels carry the sound with where the stereo mix puts it.
TEST(AudioDecoderMediaTest, DownmixesSevenOneToStereoBySide) {
    const auto clip = mediaPath("7.1auditionOutLeader_v2_rtb.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioDecoder surround;
    ASSERT_TRUE(surround.open(clip, 48000, 0));
    ASSERT_EQ(surround.getOutputChannels(), 8);
    AudioDecoder stereo;
    ASSERT_TRUE(stereo.open(clip, 48000, 2));
    ASSERT_EQ(stereo.getOutputChannels(), 2);
    const DecodedStream s8 = decodeAll(surround);
    const DecodedStream s2 = decodeAll(stereo);
    ASSERT_GT(s8.frames, 48000 * 20);
    ASSERT_NEAR(static_cast<double>(s2.frames), static_cast<double>(s8.frames), 4800.0);

    // FFmpeg's 7.1 order: FL FR FC LFE BL BR SL SR.
    const int leftSide[] = {0, 4, 6};
    const int rightSide[] = {1, 5, 7};
    int leftSeconds = 0, rightSeconds = 0, centreSeconds = 0;
    const int64_t seconds = std::min(s8.frames, s2.frames) / 48000;
    for (int64_t sec = 0; sec < seconds; ++sec) {
        double ch[8] = {}, left = 0.0, right = 0.0;
        for (int64_t f = sec * 48000; f < (sec + 1) * 48000; ++f) {
            for (int c = 0; c < 8; ++c) ch[c] += s8.samples[f * 8 + c] * s8.samples[f * 8 + c];
            left += s2.samples[f * 2] * s2.samples[f * 2];
            right += s2.samples[f * 2 + 1] * s2.samples[f * 2 + 1];
        }
        double total = 0.0, l = 0.0, r = 0.0;
        for (int c = 0; c < 8; ++c) total += ch[c];
        for (int c : leftSide) l += ch[c];
        for (int c : rightSide) r += ch[c];
        if (total < 1.0) continue;  // a quiet second
        if (l > 0.9 * total) {
            ++leftSeconds;
            EXPECT_GT(left, 10.0 * right) << "second " << sec;
        } else if (r > 0.9 * total) {
            ++rightSeconds;
            EXPECT_GT(right, 10.0 * left) << "second " << sec;
        } else if (ch[2] > 0.9 * total) {
            ++centreSeconds;
            EXPECT_NEAR(left / (left + right), 0.5, 0.05) << "second " << sec;
        }
    }
    // The file walks round every speaker.
    EXPECT_GE(leftSeconds, 2);
    EXPECT_GE(rightSeconds, 2);
    EXPECT_GE(centreSeconds, 1);
    for (float v : s2.samples) ASSERT_LE(std::abs(v), 1.5f);
}

} // namespace test
} // namespace quadmapper
