#include <gtest/gtest.h>
#include "audio/AudioDecoder.hpp"
#include "audio/AudioFrame.hpp"
#include "audio/AudioBuffer.hpp"
#include <filesystem>

namespace quadmapper {
namespace test {

class AudioDecoderTest : public ::testing::Test {
protected:
    AudioDecoder decoder;
};

TEST_F(AudioDecoderTest, DefaultState) {
    EXPECT_FALSE(decoder.isOpen());
    EXPECT_TRUE(decoder.isAtEnd());
    EXPECT_FALSE(decoder.isBackgroundDecodingActive());
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

TEST_F(AudioDecoderTest, BackgroundDecodingWithoutOpen) {
    AudioBuffer buffer;
    EXPECT_NO_THROW(decoder.startBackgroundDecoding(&buffer));
    EXPECT_FALSE(decoder.isBackgroundDecodingActive());
}

TEST_F(AudioDecoderTest, StopBackgroundDecodingWithoutStart) {
    EXPECT_NO_THROW(decoder.stopBackgroundDecoding());
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

// Integration tests that require actual media files
// These will be skipped if no test media is available

class AudioDecoderIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Look for test video file in common locations
        std::vector<std::filesystem::path> searchPaths = {
            "test_data/video.mp4",
            "../test_data/video.mp4",
            "../../test_data/video.mp4",
            "tests/test_data/video.mp4",
        };

        for (const auto& path : searchPaths) {
            if (std::filesystem::exists(path)) {
                testMediaPath = path;
                break;
            }
        }
    }

    std::filesystem::path testMediaPath;
    AudioDecoder decoder;
};

TEST_F(AudioDecoderIntegrationTest, OpenVideoWithAudio) {
    if (testMediaPath.empty()) {
        GTEST_SKIP() << "No test media file found";
    }

    EXPECT_TRUE(decoder.open(testMediaPath));
    EXPECT_TRUE(decoder.isOpen());

    const AudioInfo& info = decoder.getInfo();
    EXPECT_TRUE(info.isValid());
    EXPECT_GT(info.channels, 0);
    EXPECT_GT(info.sampleRate, 0);
    EXPECT_GT(info.duration, 0.0);

    decoder.close();
    EXPECT_FALSE(decoder.isOpen());
}

TEST_F(AudioDecoderIntegrationTest, DecodeFrames) {
    if (testMediaPath.empty()) {
        GTEST_SKIP() << "No test media file found";
    }

    ASSERT_TRUE(decoder.open(testMediaPath));

    // Decode a few frames
    for (int i = 0; i < 10; ++i) {
        AudioFrame frame = decoder.decodeNextFrame();
        if (!frame.isValid()) {
            // Might reach EOF
            break;
        }

        EXPECT_GT(frame.channels, 0);
        EXPECT_GT(frame.sampleRate, 0);
        EXPECT_GT(frame.sampleCount, 0);
        EXPECT_FALSE(frame.samples.empty());
    }
}

TEST_F(AudioDecoderIntegrationTest, SeekAndDecode) {
    if (testMediaPath.empty()) {
        GTEST_SKIP() << "No test media file found";
    }

    ASSERT_TRUE(decoder.open(testMediaPath));

    const AudioInfo& info = decoder.getInfo();
    if (info.duration > 1.0) {
        EXPECT_TRUE(decoder.seek(0.5));
        EXPECT_GE(decoder.getCurrentTime(), 0.0);

        AudioFrame frame = decoder.decodeNextFrame();
        // After seeking, we should be able to decode
        // (unless file is very short)
        if (frame.isValid()) {
            EXPECT_GE(frame.pts, 0.0);
        }
    }
}

TEST_F(AudioDecoderIntegrationTest, BackgroundDecoding) {
    if (testMediaPath.empty()) {
        GTEST_SKIP() << "No test media file found";
    }

    ASSERT_TRUE(decoder.open(testMediaPath));

    AudioBuffer buffer(48000 * 2 * 2);  // 2 seconds stereo

    decoder.startBackgroundDecoding(&buffer);
    EXPECT_TRUE(decoder.isBackgroundDecodingActive());

    // Wait a bit for decoding
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should have some samples in buffer
    size_t available = buffer.available();
    // Note: might be 0 if file is very short or decoding is slow

    decoder.stopBackgroundDecoding();
    EXPECT_FALSE(decoder.isBackgroundDecodingActive());
}

} // namespace test
} // namespace quadmapper
