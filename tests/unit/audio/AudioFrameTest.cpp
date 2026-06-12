#include <gtest/gtest.h>
#include "audio/AudioFrame.hpp"

namespace quadmapper {
namespace test {

TEST(AudioFrameTest, DefaultConstructionIsInvalid) {
    AudioFrame frame;
    EXPECT_FALSE(frame.isValid());
    EXPECT_EQ(frame.channels, 0);
    EXPECT_EQ(frame.sampleRate, 0);
    EXPECT_EQ(frame.sampleCount, 0);
    EXPECT_EQ(frame.pts, 0.0);
    EXPECT_TRUE(frame.samples.empty());
}

TEST(AudioFrameTest, ValidFrameReturnsTrue) {
    AudioFrame frame;
    frame.samples = {0.1f, 0.2f, 0.3f, 0.4f};
    frame.channels = 2;
    frame.sampleRate = 44100;
    frame.sampleCount = 2;  // 2 samples per channel
    frame.pts = 1.5;

    EXPECT_TRUE(frame.isValid());
}

TEST(AudioFrameTest, InvalidWithNoChannels) {
    AudioFrame frame;
    frame.samples = {0.1f, 0.2f};
    frame.channels = 0;
    frame.sampleRate = 44100;
    frame.sampleCount = 2;

    EXPECT_FALSE(frame.isValid());
}

TEST(AudioFrameTest, InvalidWithNoSampleRate) {
    AudioFrame frame;
    frame.samples = {0.1f, 0.2f};
    frame.channels = 2;
    frame.sampleRate = 0;
    frame.sampleCount = 1;

    EXPECT_FALSE(frame.isValid());
}

TEST(AudioFrameTest, InvalidWithNoSampleCount) {
    AudioFrame frame;
    frame.samples = {0.1f, 0.2f};
    frame.channels = 2;
    frame.sampleRate = 44100;
    frame.sampleCount = 0;

    EXPECT_FALSE(frame.isValid());
}

TEST(AudioFrameTest, InvalidWithEmptySamples) {
    AudioFrame frame;
    frame.channels = 2;
    frame.sampleRate = 44100;
    frame.sampleCount = 10;

    EXPECT_FALSE(frame.isValid());
}

TEST(AudioFrameTest, DurationCalculation) {
    AudioFrame frame;
    frame.channels = 2;
    frame.sampleRate = 44100;
    frame.sampleCount = 44100;  // 1 second worth of samples per channel
    frame.samples.resize(44100 * 2);

    EXPECT_DOUBLE_EQ(frame.duration(), 1.0);
}

TEST(AudioFrameTest, DurationWithZeroSampleRate) {
    AudioFrame frame;
    frame.channels = 2;
    frame.sampleRate = 0;
    frame.sampleCount = 1000;

    EXPECT_DOUBLE_EQ(frame.duration(), 0.0);
}

TEST(AudioFrameTest, DurationSubSecond) {
    AudioFrame frame;
    frame.sampleRate = 48000;
    frame.sampleCount = 4800;  // 0.1 seconds

    EXPECT_NEAR(frame.duration(), 0.1, 0.0001);
}

TEST(AudioFrameTest, TotalSamplesCalculation) {
    AudioFrame frame;
    frame.channels = 2;
    frame.sampleCount = 1024;

    EXPECT_EQ(frame.totalSamples(), 2048u);
}

TEST(AudioFrameTest, TotalSamplesMono) {
    AudioFrame frame;
    frame.channels = 1;
    frame.sampleCount = 512;

    EXPECT_EQ(frame.totalSamples(), 512u);
}

TEST(AudioFrameTest, TotalSamplesSurround) {
    AudioFrame frame;
    frame.channels = 6;  // 5.1 surround
    frame.sampleCount = 100;

    EXPECT_EQ(frame.totalSamples(), 600u);
}

TEST(AudioFrameTest, ClearResetsAllFields) {
    AudioFrame frame;
    frame.samples = {0.1f, 0.2f, 0.3f, 0.4f};
    frame.channels = 2;
    frame.sampleRate = 44100;
    frame.sampleCount = 2;
    frame.pts = 5.0;

    frame.clear();

    EXPECT_TRUE(frame.samples.empty());
    EXPECT_EQ(frame.channels, 0);
    EXPECT_EQ(frame.sampleRate, 0);
    EXPECT_EQ(frame.sampleCount, 0);
    EXPECT_EQ(frame.pts, 0.0);
    EXPECT_FALSE(frame.isValid());
}

// AudioInfo tests
TEST(AudioInfoTest, DefaultConstructionIsInvalid) {
    AudioInfo info;
    EXPECT_FALSE(info.isValid());
    EXPECT_EQ(info.channels, 0);
    EXPECT_EQ(info.sampleRate, 0);
    EXPECT_EQ(info.duration, 0.0);
}

TEST(AudioInfoTest, ValidWithChannelsAndSampleRate) {
    AudioInfo info;
    info.channels = 2;
    info.sampleRate = 44100;

    EXPECT_TRUE(info.isValid());
}

TEST(AudioInfoTest, InvalidWithOnlyChannels) {
    AudioInfo info;
    info.channels = 2;
    info.sampleRate = 0;

    EXPECT_FALSE(info.isValid());
}

TEST(AudioInfoTest, InvalidWithOnlySampleRate) {
    AudioInfo info;
    info.channels = 0;
    info.sampleRate = 44100;

    EXPECT_FALSE(info.isValid());
}

} // namespace test
} // namespace quadmapper
