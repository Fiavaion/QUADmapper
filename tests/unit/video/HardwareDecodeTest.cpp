// Hardware decoding (ADR-055): a 1080p H.264 clip decodes on the GPU (D3D11VA) into
// NV12 frames that match software decoding bit for bit (H.264 decoding is exact); a
// GPU failure, at open or mid-stream, carries on in software from the same frame; one
// D3D11 device serves every decoder.
//
// Needs the local test media and a GPU with a D3D11 video decoder; SKIPs otherwise.

#include <gtest/gtest.h>

#include "video/VideoDecoder.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;

namespace {

const std::filesystem::path H264_CLIP = "D:/testing Videos/greenCrystals_15Sec.mp4";          // 1080p25
const std::filesystem::path H264_SHORT_GOP = "D:/testing Videos/greenCrystals_15Sec_4x3.mp4";  // Ping-Pong-able

DecodeOptions hardwareOptions(int failAfter = -1) {
    DecodeOptions options;
    options.hardware = true;
    options.yuvOutput = true;
    options.failHardwareAfterFrames = failAfter;
    return options;
}

DecodeOptions softwareOptions() {
    DecodeOptions options;
    options.yuvOutput = true;
    return options;
}

int frameIndex(const VideoFrame& frame, double fps) {
    return static_cast<int>(std::lround(frame.pts * fps));
}

// Sample value (8-bit layouts) at x, y of the luma plane.
int luma(const YuvPicture& p, int x, int y) {
    return p.planes[0][static_cast<size_t>(y) * p.strides[0] + x];
}

// Cb, Cr at chroma position cx, cy, from either an interleaved or a planar layout.
void chroma(const YuvPicture& p, int cx, int cy, int& cb, int& cr) {
    if (p.semiPlanar()) {
        const uint8_t* row = p.planes[1] + static_cast<size_t>(cy) * p.strides[1];
        cb = row[2 * cx];
        cr = row[2 * cx + 1];
    } else {
        cb = p.planes[1][static_cast<size_t>(cy) * p.strides[1] + cx];
        cr = p.planes[2][static_cast<size_t>(cy) * p.strides[2] + cx];
    }
}

// Every sample of two 8-bit 4:2:0 pictures is equal, whatever their layouts.
::testing::AssertionResult samePicture(const YuvPicture& a, const YuvPicture& b) {
    if (a.width != b.width || a.height != b.height) return ::testing::AssertionFailure() << "sizes differ";
    for (int y = 0; y < a.height; ++y) {
        if (std::memcmp(a.planes[0] + static_cast<size_t>(y) * a.strides[0],
                        b.planes[0] + static_cast<size_t>(y) * b.strides[0], a.width) != 0) {
            return ::testing::AssertionFailure() << "luma row " << y << " differs";
        }
    }
    for (int cy = 0; cy < a.chromaHeight(); ++cy) {
        for (int cx = 0; cx < a.chromaWidth(); ++cx) {
            int acb, acr, bcb, bcr;
            chroma(a, cx, cy, acb, acr);
            chroma(b, cx, cy, bcb, bcr);
            if (acb != bcb || acr != bcr) {
                return ::testing::AssertionFailure() << "chroma " << cx << "," << cy << " differs";
            }
        }
    }
    return ::testing::AssertionSuccess();
}

// Open with hardware decoding, or skip the test where the machine cannot.
bool openHardware(VideoDecoder& decoder, const std::filesystem::path& clip, int failAfter = -1) {
    if (!std::filesystem::exists(clip)) return false;
    if (!decoder.open(clip, hardwareOptions(failAfter))) return false;
    return true;
}

}  // namespace

TEST(HardwareDecode, H264DecodesOnTheGpuIntoNv12MatchingSoftware) {
    VideoDecoder hardware;
    if (!openHardware(hardware, H264_CLIP)) GTEST_SKIP() << "test clip not present";
    if (!hardware.isHardwareDecoding()) {
        GTEST_SKIP() << "no GPU decoding here: " << hardware.getDecodeNote();
    }
    EXPECT_TRUE(hardware.getDecodeNote().empty());
    EXPECT_EQ(VideoDecoder::hardwareDevicesCreated(), 1);

    VideoDecoder software;
    ASSERT_TRUE(software.open(H264_CLIP, softwareOptions()));
    EXPECT_FALSE(software.isHardwareDecoding());

    const double fps = hardware.getInfo().frameRate;
    for (int i = 0; i < 30; ++i) {
        const VideoFrame hw = hardware.decodeNextFrame();
        const VideoFrame sw = software.decodeNextFrame();
        ASSERT_TRUE(hw.isValid() && sw.isValid()) << "frame " << i;
        EXPECT_EQ(hw.yuv.layout, YuvLayout::NV12);
        EXPECT_EQ(sw.yuv.layout, YuvLayout::YUV420P);
        EXPECT_TRUE(hw.data.empty());  // no RGB conversion on the CPU
        EXPECT_EQ(frameIndex(hw, fps), i);
        EXPECT_EQ(frameIndex(sw, fps), i);
        EXPECT_EQ(hw.yuv.matrix, sw.yuv.matrix);
        EXPECT_EQ(hw.yuv.fullRange, sw.yuv.fullRange);
        EXPECT_TRUE(samePicture(hw.yuv, sw.yuv)) << "frame " << i;
    }
    EXPECT_TRUE(hardware.isHardwareDecoding());

    // Seeking keeps working on the GPU and stays frame-exact.
    ASSERT_TRUE(hardware.seek(10.0));
    ASSERT_TRUE(software.seek(10.0));
    const VideoFrame hw = hardware.decodeNextFrame();
    const VideoFrame sw = software.decodeNextFrame();
    ASSERT_TRUE(hw.isValid() && sw.isValid());
    EXPECT_EQ(frameIndex(hw, fps), frameIndex(sw, fps));
    EXPECT_EQ(frameIndex(hw, fps), static_cast<int>(std::lround(10.0 * fps)));
    EXPECT_TRUE(samePicture(hw.yuv, sw.yuv));
}

TEST(HardwareDecode, FrameHandlesOutliveTheDecoder) {
    VideoFrame kept;
    {
        VideoDecoder decoder;
        if (!openHardware(decoder, H264_CLIP)) GTEST_SKIP() << "test clip not present";
        kept = decoder.decodeNextFrame();
        ASSERT_TRUE(kept.isValid());
    }
    // The decoder (and its GPU pool) is gone; the frame's planes are still readable.
    long sum = 0;
    for (int x = 0; x < kept.yuv.width; ++x) sum += luma(kept.yuv, x, kept.yuv.height / 2);
    EXPECT_GT(sum, 0);
}

TEST(HardwareDecode, GpuFailureMidStreamCarriesOnInSoftwareFromTheSameFrame) {
    VideoDecoder decoder;
    if (!openHardware(decoder, H264_CLIP, /*failAfter=*/12)) GTEST_SKIP() << "test clip not present";
    if (!decoder.isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << decoder.getDecodeNote();

    VideoDecoder reference;
    ASSERT_TRUE(reference.open(H264_CLIP, softwareOptions()));
    const double fps = decoder.getInfo().frameRate;
    // The open probe decoded one GPU frame, so the failure lands on frame 11.
    for (int i = 0; i < 40; ++i) {
        const VideoFrame frame = decoder.decodeNextFrame();
        const VideoFrame want = reference.decodeNextFrame();
        ASSERT_TRUE(frame.isValid()) << "frame " << i;
        EXPECT_EQ(frameIndex(frame, fps), i) << "no frame skipped or repeated";
        EXPECT_EQ(frame.yuv.layout, i < 11 ? YuvLayout::NV12 : YuvLayout::YUV420P) << "frame " << i;
        EXPECT_TRUE(samePicture(frame.yuv, want.yuv)) << "frame " << i;
    }
    EXPECT_FALSE(decoder.isHardwareDecoding());
    EXPECT_NE(decoder.getDecodeNote().find("simulated"), std::string::npos) << decoder.getDecodeNote();
}

TEST(HardwareDecode, GpuFailureAtOpenStartsInSoftware) {
    VideoDecoder decoder;
    if (!openHardware(decoder, H264_CLIP, /*failAfter=*/0)) GTEST_SKIP() << "test clip not present";
    EXPECT_FALSE(decoder.isHardwareDecoding());
    const VideoFrame first = decoder.decodeNextFrame();
    ASSERT_TRUE(first.isValid());
    EXPECT_EQ(frameIndex(first, decoder.getInfo().frameRate), 0);
    EXPECT_EQ(first.yuv.layout, YuvLayout::YUV420P);
}

TEST(HardwareDecode, PlannedPlaybackFallsBackWithoutAGap) {
    VideoDecoder decoder;
    if (!openHardware(decoder, H264_CLIP, /*failAfter=*/20)) GTEST_SKIP() << "test clip not present";
    if (!decoder.isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << decoder.getDecodeNote();
    const double fps = decoder.getInfo().frameRate;

    DecodePlan plan;
    plan.timeline = 2.0;
    plan.duration = decoder.getInfo().duration;
    plan.loopMode = LoopMode::LOOP;
    decoder.startPlayback(plan, 8);

    std::vector<int> frames;
    for (int spins = 0; frames.size() < 60 && spins < 5000; ++spins) {
        const VideoFrame frame = decoder.getBufferedFrame();
        if (frame.isValid()) {
            frames.push_back(frameIndex(frame, fps));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    decoder.stopBackgroundDecoding();
    ASSERT_EQ(frames.size(), 60u);
    for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i], 50 + static_cast<int>(i)) << "at " << i;
    }
    EXPECT_FALSE(decoder.isHardwareDecoding());
}

TEST(HardwareDecode, PingPongReverseLegRunsOnTheGpu) {
    VideoDecoder decoder;
    if (!openHardware(decoder, H264_SHORT_GOP)) GTEST_SKIP() << "test clip not present";
    if (!decoder.isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << decoder.getDecodeNote();
    ASSERT_TRUE(decoder.checkReverse(DEFAULT_REVERSE_CACHE_BYTES).empty())
        << decoder.checkReverse(DEFAULT_REVERSE_CACHE_BYTES);
    const double fps = decoder.getInfo().frameRate;
    const double duration = decoder.getInfo().duration;
    const int last = static_cast<int>(std::lround(duration * fps)) - 1;

    // Start one second before the reflection: the end of the forward pass, then backwards.
    DecodePlan plan;
    plan.timeline = duration - 1.0;
    plan.duration = duration;
    plan.loopMode = LoopMode::PING_PONG;
    decoder.startPlayback(plan, 8);

    const int count = static_cast<int>(std::lround(3.0 * fps));
    std::vector<int> frames;
    for (int spins = 0; static_cast<int>(frames.size()) < count && spins < 20000; ++spins) {
        const VideoFrame frame = decoder.getBufferedFrame();
        if (frame.isValid()) {
            frames.push_back(frameIndex(frame, fps));
            EXPECT_EQ(frame.yuv.layout, YuvLayout::NV12);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    decoder.stopBackgroundDecoding();
    EXPECT_TRUE(decoder.getReverseError().empty()) << decoder.getReverseError();
    ASSERT_EQ(static_cast<int>(frames.size()), count);
    // Up to the last frame one by one, then down again one by one.
    int peak = 0;
    while (peak + 1 < count && frames[peak + 1] > frames[peak]) ++peak;
    EXPECT_EQ(frames[peak], last);
    for (int i = 1; i <= peak; ++i) EXPECT_EQ(frames[i], frames[i - 1] + 1) << "at " << i;
    for (int i = peak + 1; i < count; ++i) EXPECT_EQ(frames[i], frames[i - 1] - (i == peak + 1 ? 0 : 1)) << "at " << i;
    EXPECT_TRUE(decoder.isHardwareDecoding());
    EXPECT_GT(decoder.getReverseCachePeakBytes(), 0u);
}

// 10-bit HEVC (Main10) comes off the GPU as P010, and from software as YUV420P10; both
// carry the same 10-bit samples. The clip is not part of the test media: point
// QM_HEVC10_CLIP at one (e.g. ffmpeg -c:v libx265 -pix_fmt yuv420p10le).
TEST(HardwareDecode, TenBitHevcDecodesToP010MatchingSoftware) {
    const char* clip = std::getenv("QM_HEVC10_CLIP");
    if (!clip || !std::filesystem::exists(clip)) GTEST_SKIP() << "QM_HEVC10_CLIP not set";
    VideoDecoder hardware;
    ASSERT_TRUE(hardware.open(clip, hardwareOptions()));
    if (!hardware.isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << hardware.getDecodeNote();
    VideoDecoder software;
    ASSERT_TRUE(software.open(clip, softwareOptions()));
    for (int i = 0; i < 10; ++i) {
        const VideoFrame hw = hardware.decodeNextFrame();
        const VideoFrame sw = software.decodeNextFrame();
        ASSERT_TRUE(hw.isValid() && sw.isValid());
        ASSERT_EQ(hw.yuv.layout, YuvLayout::P010);
        ASSERT_EQ(sw.yuv.layout, YuvLayout::YUV420P10);
        for (int y = 0; y < hw.yuv.height; y += 7) {
            const auto* a = reinterpret_cast<const uint16_t*>(hw.yuv.planes[0] + static_cast<size_t>(y) * hw.yuv.strides[0]);
            const auto* b = reinterpret_cast<const uint16_t*>(sw.yuv.planes[0] + static_cast<size_t>(y) * sw.yuv.strides[0]);
            for (int x = 0; x < hw.yuv.width; ++x) {
                ASSERT_EQ(a[x] >> 6, b[x]) << "frame " << i << " at " << x << "," << y;
            }
        }
    }
}

TEST(HardwareDecode, OneSharedDeviceServesEveryDecoder) {
    if (!std::filesystem::exists(H264_CLIP)) GTEST_SKIP() << "test clip not present";
    {
        VideoDecoder a;
        ASSERT_TRUE(a.open(H264_CLIP, hardwareOptions()));
        if (!a.isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << a.getDecodeNote();
    }
    EXPECT_EQ(VideoDecoder::hardwareDevicesCreated(), 1);
    {
        VideoDecoder a;
        VideoDecoder b;
        ASSERT_TRUE(a.open(H264_CLIP, hardwareOptions()));
        ASSERT_TRUE(b.open(H264_CLIP, hardwareOptions()));
        EXPECT_TRUE(a.isHardwareDecoding());
        EXPECT_TRUE(b.isHardwareDecoding());
        a.close();
        EXPECT_TRUE(b.decodeNextFrame().isValid());  // b keeps the device a let go of
    }
    EXPECT_EQ(VideoDecoder::hardwareDevicesCreated(), 1);  // reused, never re-created
}
