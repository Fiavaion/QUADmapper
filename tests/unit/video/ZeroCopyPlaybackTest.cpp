// Zero-copy hardware video (ADR-056), end to end on real clips: the first frame of an
// 8-bit HEVC, a 10-bit HEVC and an H.264 clip, decoded on the GPU and left there, is
// delivered through WGL_NV_DX_interop2 and YuvConverter; every pixel matches the same
// frame copied to memory and uploaded (the ADR-055 readback path) and the frame decoded
// in software, within +-2 per channel (the planes carry the same samples). During playback
// every frame shown, across several wraps of the decoder's texture pool, is the frame
// handed over (compared with the same frame downloaded). A lock failure mid-stream falls
// back to the readback path for that source only, never showing a black or wrong frame,
// without reading a frame back on the main thread, and releasing the stream's GPU frames;
// a GPU frame pool that cannot be allocated or shared stops zero-copy instead of claiming it.
//
// Needs the local test media, a GPU with a D3D11 video decoder and a GL context whose
// driver has WGL_NV_DX_interop2; SKIPs otherwise.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "renderer/DxInterop.hpp"
#include "renderer/Texture.hpp"
#include "renderer/YuvConverter.hpp"
#include "source/SourceManager.hpp"
#include "source/VideoFileSource.hpp"
#include "video/VideoDecoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;

namespace {

struct GLContext {
    GLFWwindow* win = nullptr;
    bool ok = false;
    GLContext() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "zero-copy-test", nullptr, nullptr);
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~GLContext() {
        if (win) { glfwDestroyWindow(win); glfwTerminate(); }
    }
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;
};

const std::filesystem::path HEVC8_CLIP = "D:/testing Videos/qa_video/hevc_1080p.mp4";
const std::filesystem::path HEVC10_CLIP = "D:/testing Videos/qa_video/hevc10_1080p.mp4";
const std::filesystem::path H264_CLIP = "D:/testing Videos/greenCrystals_15Sec.mp4";

std::vector<uint8_t> readBack(const Texture& texture) {
    std::vector<uint8_t> rgba(static_cast<size_t>(texture.getWidth()) * texture.getHeight() * 4);
    glBindTexture(GL_TEXTURE_2D, texture.getId());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return rgba;
}

struct Difference {
    int worst = 0;
    double mean = 0.0;
};

// RGB channels only; alpha is 255 on every path.
Difference compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Difference d;
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (i % 4 == 3) continue;
        const int diff = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        d.worst = std::max(d.worst, diff);
        sum += diff;
        ++n;
    }
    d.mean = n ? sum / static_cast<double>(n) : 0.0;
    return d;
}

// Convert a picture in memory into `texture` with the ADR-055 pass.
void convertInMemory(YuvConverter& converter, const YuvPicture& picture, Texture& texture) {
    YuvUploadState state;
    converter.begin();
    ASSERT_TRUE(converter.convert(picture, texture, state));
    converter.end();
}

class ZeroCopyFirstFrame : public testing::TestWithParam<std::filesystem::path> {};

// Checks what a source's texture shows after each update against the frame it was
// handed: that frame converted the way the readback path converts it (downloaded first
// when it is on the GPU). A source that has fallen back skips frames still on the GPU and
// keeps its last frame (not black, not read back on the main thread): there the texture
// may instead be exactly what it showed before.
class FrameCheck {
public:
    explicit FrameCheck(std::string label) : label_(std::move(label)) {}

    void afterUpdate(SourceManager& sm, const UUID& id, VideoFileSource& video) {
        const FrameData* frame = video.getCurrentFrame();
        if (!frame || !frame->isYuv()) return;
        if (seen_ && frame->frameNumber == lastNumber_ && frame->timestamp == lastTimestamp_) return;
        seen_ = true;
        lastNumber_ = frame->frameNumber;
        lastTimestamp_ = frame->timestamp;
        const Texture& texture = *sm.getTexture(id);
        const std::vector<uint8_t> got = readBack(texture);
        if (frame->yuv.onGpu() && !video.isZeroCopy() && !shown_.empty() && compare(got, shown_).worst == 0) {
            ++held;
            return;
        }
        const YuvPicture memory = VideoDecoder::downloadPicture(frame->yuv);
        ASSERT_TRUE(memory.isValid()) << label_ << ": frame " << frame->frameNumber << " could not be downloaded";
        Texture reference;
        ASSERT_TRUE(reference.createEmpty(texture.getWidth(), texture.getHeight(), 4));
        convertInMemory(converter_, memory, reference);
        const Difference d = compare(got, readBack(reference));
        EXPECT_LE(d.worst, 2) << label_ << ": frame " << frame->frameNumber
                              << (frame->yuv.onGpu() ? " (on the GPU)" : " (in memory)")
                              << " is not the frame shown (mean difference " << d.mean << ")";
        ++delivered;
        if (frame->yuv.onGpu()) ++deliveredOnGpu;
        shown_ = got;
    }

    int delivered = 0;       // frames shown that were checked against their own frame
    int deliveredOnGpu = 0;  // of which zero-copy
    int held = 0;            // frames skipped after the fallback, the last frame kept

private:
    std::string label_;
    YuvConverter converter_;
    bool seen_ = false;
    uint64_t lastNumber_ = 0;
    uint64_t lastTimestamp_ = 0;
    std::vector<uint8_t> shown_;
};

// updateAll(), counting the GPU readbacks it makes on this (the main) thread.
void update(SourceManager& sm, long& mainThreadReadbacks) {
    const long before = VideoDecoder::gpuReadbacksOnThisThread();
    sm.updateAll(1.0 / 60.0);
    mainThreadReadbacks += VideoDecoder::gpuReadbacksOnThisThread() - before;
}

// Skip reasons shared by the playback tests; empty when they can run.
std::string playbackSkip(const GLContext& ctx) {
    if (!std::filesystem::exists(H264_CLIP)) return "test clip not present";
    if (!ctx.ok) return "no GL 3.3 context";
    DxInterop probe;
    if (!probe.available()) return probe.unavailableReason();
    return {};
}

}  // namespace

TEST_P(ZeroCopyFirstFrame, MatchesTheReadbackAndSoftwarePaths) {
    const std::filesystem::path clip = GetParam();
    if (!std::filesystem::exists(clip)) GTEST_SKIP() << clip.string() << " not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";
    DxInterop interop;
    if (!interop.available()) GTEST_SKIP() << interop.unavailableReason();

    DecodeOptions options;
    options.hardware = true;
    options.yuvOutput = true;
    options.zeroCopy = true;
    VideoDecoder hardware;
    ASSERT_TRUE(hardware.open(clip, options));
    if (!hardware.isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << hardware.getDecodeNote();
    EXPECT_TRUE(hardware.isZeroCopy());
    const VideoFrame gpu = hardware.decodeNextFrame();
    ASSERT_TRUE(gpu.isValid());
    ASSERT_TRUE(gpu.yuv.onGpu()) << "the frame was copied to memory";

    DecodeOptions softwareOptions;
    softwareOptions.yuvOutput = true;
    VideoDecoder software;
    ASSERT_TRUE(software.open(clip, softwareOptions));
    const VideoFrame sw = software.decodeNextFrame();
    ASSERT_TRUE(sw.isValid() && !sw.yuv.onGpu());
    ASSERT_EQ(sw.yuv.width, gpu.yuv.width);
    ASSERT_EQ(sw.yuv.height, gpu.yuv.height);

    const int w = gpu.yuv.width;
    const int h = gpu.yuv.height;
    Texture zeroCopy, readbackPath, softwarePath;
    ASSERT_TRUE(zeroCopy.createEmpty(w, h, 4));
    ASSERT_TRUE(readbackPath.createEmpty(w, h, 4));
    ASSERT_TRUE(softwarePath.createEmpty(w, h, 4));
    YuvConverter converter;
    {
        DxInteropTarget target;
        std::string why;
        converter.begin();
        ASSERT_TRUE(interop.stage(gpu.yuv, target, zeroCopy, nullptr, why)) << why;
        interop.finish(converter, [&](void*, const YuvPicture&, const std::string& reason) { why = reason; });
        converter.end();
        ASSERT_TRUE(why.empty()) << why;
    }
    const YuvPicture downloaded = VideoDecoder::downloadPicture(gpu.yuv);
    ASSERT_TRUE(downloaded.isValid() && !downloaded.onGpu());
    convertInMemory(converter, downloaded, readbackPath);
    convertInMemory(converter, sw.yuv, softwarePath);

    const std::vector<uint8_t> a = readBack(zeroCopy);
    const Difference vsReadback = compare(a, readBack(readbackPath));
    const Difference vsSoftware = compare(a, readBack(softwarePath));
    std::printf("[ zero-copy ] %s (%s): vs readback worst %d mean %.3f; vs software worst %d mean %.3f\n",
                clip.filename().string().c_str(), gpu.yuv.layout == YuvLayout::P010 ? "P010" : "NV12",
                vsReadback.worst, vsReadback.mean, vsSoftware.worst, vsSoftware.mean);
    EXPECT_LE(vsReadback.worst, 2);
    EXPECT_LE(vsSoftware.worst, 2);
    RecordProperty("worst_vs_readback", vsReadback.worst);
    RecordProperty("worst_vs_software", vsSoftware.worst);
}

INSTANTIATE_TEST_SUITE_P(Clips, ZeroCopyFirstFrame, testing::Values(HEVC8_CLIP, HEVC10_CLIP, H264_CLIP),
                         [](const testing::TestParamInfo<std::filesystem::path>& info) {
                             return info.index == 0 ? std::string("Hevc8") : info.index == 1 ? "Hevc10" : "H264";
                         });

// Every frame shown through zero-copy is the frame handed over, across more than two
// wraps of the decoder's 12-texture pool: each is compared with the same frame downloaded
// and converted in memory. A renderer that skipped the copy for a pool texture it had
// already opened would show a frozen or looping picture here.
TEST(ZeroCopyPlayback, EveryFrameMatchesItsOwnDownloadAcrossPoolWraps) {
    GLContext ctx;
    const std::string skip = playbackSkip(ctx);
    if (!skip.empty()) GTEST_SKIP() << skip;
    DxInterop::testHooks() = DxInterop::TestHooks{};

    SourceManager sm;
    const UUID id = sm.createVideoSource(H264_CLIP);
    ASSERT_FALSE(id.isNull());
    sm.finishBackgroundWork();  // opened off the main thread (TD-35)
    auto* video = sm.getSourceAs<VideoFileSource>(id);
    ASSERT_NE(video, nullptr);
    if (!video->isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here: " << video->getDecodeNote();
    video->play();
    FrameCheck check("zero-copy");
    long readbacks = 0;
    for (int i = 0; i < 5000 && check.deliveredOnGpu < 30 && !HasFatalFailure(); ++i) {
        update(sm, readbacks);
        check.afterUpdate(sm, id, *video);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(video->isZeroCopy()) << video->getZeroCopyNote();
    EXPECT_GE(check.deliveredOnGpu, 30);
    EXPECT_EQ(check.held, 0);
    EXPECT_EQ(readbacks, 0);
}

// A run-time interop failure (the lock, forced after a few frames) moves that source to
// the readback path: every frame shown is its own frame or, while frames already decoded
// on the GPU drain, the last frame kept (never black, never read back on the main
// thread). The Video Player's note says why, and the stream lets go of its GPU frames
// while it keeps playing.
TEST(ZeroCopyPlayback, LockFailureMidStreamFallsBackWithoutABlackFrame) {
    GLContext ctx;
    const std::string skip = playbackSkip(ctx);
    if (!skip.empty()) GTEST_SKIP() << skip;
    DxInterop::testHooks() = DxInterop::TestHooks{};
    DxInterop::testHooks().failLockAfter = 5;

    FrameCheck check("lock failure");
    long readbacks = 0;
    int onGpu = 0;
    long gpuFramesAfter = -1;
    {
        SourceManager sm;
        const UUID id = sm.createVideoSource(H264_CLIP);
        ASSERT_FALSE(id.isNull());
        sm.finishBackgroundWork();  // opened off the main thread (TD-35)
        auto* video = sm.getSourceAs<VideoFileSource>(id);
        ASSERT_NE(video, nullptr);
        if (!video->isHardwareDecoding()) {
            DxInterop::testHooks() = DxInterop::TestHooks{};
            GTEST_SKIP() << "no GPU decoding here: " << video->getDecodeNote();
        }
        video->play();
        for (int i = 0; i < 5000 && check.delivered - check.deliveredOnGpu < 12 && !HasFatalFailure(); ++i) {
            update(sm, readbacks);
            check.afterUpdate(sm, id, *video);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        onGpu = check.deliveredOnGpu;
        EXPECT_FALSE(video->isZeroCopy());
        EXPECT_NE(video->getZeroCopyNote().find("lock"), std::string::npos) << video->getZeroCopyNote();
        gpuFramesAfter = VideoDecoder::liveGpuFrames();
        sm.removeSource(id);
        EXPECT_EQ(DxInterop::liveObjects(), 0);
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
    EXPECT_EQ(onGpu, 5) << "zero-copy frames before the forced failure";
    EXPECT_GE(check.delivered - onGpu, 12) << "frames kept coming after the fallback";
    EXPECT_LE(check.held, 12) << "frames skipped while the queue drained";
    EXPECT_EQ(readbacks, 0) << "the main thread read frames back from the GPU";
    EXPECT_EQ(gpuFramesAfter, 0) << "the playing stream still held GPU frames after the fallback";
    EXPECT_EQ(VideoDecoder::liveGpuFrames(), 0) << "every frame left on the GPU was released";
}

// Several streams share one interop lock per frame. A lock failure on one of them moves
// only that one to the readback path; the others stay on zero-copy with the right
// frames, and no frame is read back on the main thread (a readback waits on the GPU:
// 16 streams falling back together stalled a frame for 700 ms).
TEST(ZeroCopyPlayback, OneSourcesLockFailureLeavesTheOthersOnZeroCopy) {
    GLContext ctx;
    const std::string skip = playbackSkip(ctx);
    if (!skip.empty()) GTEST_SKIP() << skip;
    DxInterop::testHooks() = DxInterop::TestHooks{};

    constexpr int STREAMS = 4;
    constexpr int BAD = 1;
    long readbacks = 0;
    {
        SourceManager sm;
        std::vector<UUID> ids;
        std::vector<VideoFileSource*> videos;
        std::deque<FrameCheck> checks;  // not moved: FrameCheck holds GL objects
        for (int s = 0; s < STREAMS; ++s) {
            ids.push_back(sm.createVideoSource(H264_CLIP));  // not deduplicated, unlike getOrCreate
            ASSERT_FALSE(ids.back().isNull());
            sm.finishBackgroundWork();  // opened off the main thread (TD-35)
            videos.push_back(sm.getSourceAs<VideoFileSource>(ids.back()));
            ASSERT_NE(videos.back(), nullptr);
            if (!videos.back()->isHardwareDecoding()) GTEST_SKIP() << "no GPU decoding here";
            checks.emplace_back("stream " + std::to_string(s));
            videos.back()->play();
        }
        auto run = [&](auto done) {
            for (int i = 0; i < 8000 && !done() && !HasFatalFailure(); ++i) {
                update(sm, readbacks);
                for (int s = 0; s < STREAMS; ++s) checks[s].afterUpdate(sm, ids[s], *videos[s]);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        run([&] {
            return std::all_of(checks.begin(), checks.end(), [](const FrameCheck& c) { return c.deliveredOnGpu >= 3; });
        });
        for (const FrameCheck& c : checks) ASSERT_GE(c.deliveredOnGpu, 3);
        DxInterop::testHooks().failLockDest = sm.getTexture(ids[BAD]);
        const int badBefore = checks[BAD].delivered;
        std::vector<int> othersBefore;
        for (const FrameCheck& c : checks) othersBefore.push_back(c.deliveredOnGpu);
        run([&] { return checks[BAD].delivered - badBefore >= 10; });

        EXPECT_FALSE(videos[BAD]->isZeroCopy());
        EXPECT_NE(videos[BAD]->getZeroCopyNote().find("lock"), std::string::npos) << videos[BAD]->getZeroCopyNote();
        EXPECT_GE(checks[BAD].delivered - badBefore, 10) << "the stream that fell back kept playing";
        for (int s = 0; s < STREAMS; ++s) {
            if (s == BAD) continue;
            EXPECT_TRUE(videos[s]->isZeroCopy()) << "stream " << s << ": " << videos[s]->getZeroCopyNote();
            EXPECT_GT(checks[s].deliveredOnGpu, othersBefore[static_cast<size_t>(s)]) << "stream " << s;
            EXPECT_EQ(checks[s].held, 0) << "stream " << s;
        }
        for (const UUID& id : ids) sm.removeSource(id);
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
    EXPECT_EQ(readbacks, 0) << "the main thread read frames back from the GPU";
    EXPECT_EQ(VideoDecoder::liveGpuFrames(), 0);
}

namespace {

// A decoder asked to leave frames on the GPU, or empty with the reason to skip.
std::string openZeroCopy(VideoDecoder& decoder, DecodeOptions options) {
    if (!std::filesystem::exists(H264_CLIP)) return "test clip not present";
    options.hardware = true;
    options.yuvOutput = true;
    options.zeroCopy = true;
    if (!decoder.open(H264_CLIP, options)) return "the clip did not open";
    if (!decoder.isHardwareDecoding()) return "no GPU decoding here: " + decoder.getDecodeNote();
    return {};
}

}  // namespace

// Video memory runs out below the pool's cap: zero-copy stops once, with the reason, and
// the stream's frames go through memory. isZeroCopy() (the Video Player's
// "hardware (zero-copy)") must not claim otherwise.
TEST(ZeroCopyDecoder, TextureAllocationFailureStopsZeroCopy) {
    VideoDecoder decoder;
    DecodeOptions options;
    options.failGpuTextureAfter = 3;
    const std::string skip = openZeroCopy(decoder, options);
    if (!skip.empty()) GTEST_SKIP() << skip;
    ASSERT_TRUE(decoder.isZeroCopy());
    std::vector<VideoFrame> held;  // holding the frames keeps their pool textures in use
    int onGpu = 0;
    for (int i = 0; i < 8; ++i) {
        held.push_back(decoder.decodeNextFrame());
        ASSERT_TRUE(held.back().isValid());
        onGpu += held.back().yuv.onGpu() ? 1 : 0;
    }
    EXPECT_EQ(onGpu, 3);
    EXPECT_FALSE(held.back().yuv.onGpu());
    EXPECT_FALSE(decoder.isZeroCopy());
    EXPECT_NE(decoder.getZeroCopyNote().find("video memory"), std::string::npos) << decoder.getZeroCopyNote();
}

// A device that cannot share NV12/P010 textures never claims zero-copy.
TEST(ZeroCopyDecoder, UnshareableFormatNeverClaimsZeroCopy) {
    VideoDecoder decoder;
    DecodeOptions options;
    options.denySharedFormat = true;
    const std::string skip = openZeroCopy(decoder, options);
    if (!skip.empty()) GTEST_SKIP() << skip;
    EXPECT_FALSE(decoder.isZeroCopy()) << "checked when the first frame was decoded at open";
    EXPECT_NE(decoder.getZeroCopyNote().find("cannot share"), std::string::npos) << decoder.getZeroCopyNote();
    const VideoFrame frame = decoder.decodeNextFrame();
    ASSERT_TRUE(frame.isValid());
    EXPECT_FALSE(frame.yuv.onGpu());
    EXPECT_EQ(VideoDecoder::liveGpuFrames(), 0);
}
