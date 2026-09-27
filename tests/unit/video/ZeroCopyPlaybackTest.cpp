// Zero-copy hardware video (ADR-056), end to end on real clips: the first frame of an
// 8-bit HEVC, a 10-bit HEVC and an H.264 clip, decoded on the GPU and left there, is
// delivered through WGL_NV_DX_interop2 and YuvConverter; every pixel matches the same
// frame copied to memory and uploaded (the ADR-055 readback path) and the frame decoded
// in software, within +-2 per channel (the planes carry the same samples). A lock failure mid-stream
// falls back to the readback path for that source only, without a black frame, and says
// why once.
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

// A run-time interop failure (the lock, forced after a few frames) moves that source to
// the readback path from the same frame: frames keep reaching the texture, none black,
// and the Video Player's note says why.
TEST(ZeroCopyPlayback, LockFailureMidStreamFallsBackWithoutABlackFrame) {
    if (!std::filesystem::exists(H264_CLIP)) GTEST_SKIP() << "test clip not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";
    {
        DxInterop probe;
        if (!probe.available()) GTEST_SKIP() << probe.unavailableReason();
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
    DxInterop::testHooks().failLockAfter = 5;

    int uploadsBefore = 0, uploadsAfter = 0;
    bool wasZeroCopy = false;
    {
        SourceManager sm;
        const UUID id = sm.createVideoSource(H264_CLIP);
        ASSERT_FALSE(id.isNull());
        auto* video = sm.getSourceAs<VideoFileSource>(id);
        ASSERT_NE(video, nullptr);
        video->play();

        uint64_t uploads = 0;
        for (int i = 0; i < 2000 && uploads < 20; ++i) {
            const bool zeroCopyNow = video->isZeroCopy();
            if (!video->isHardwareDecoding()) break;
            sm.updateAll(1.0 / 60.0);
            if (sm.getUploadStats().uploads == uploads) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            uploads = sm.getUploadStats().uploads;
            wasZeroCopy = wasZeroCopy || zeroCopyNow;
            (zeroCopyNow ? uploadsBefore : uploadsAfter)++;
            const std::vector<uint8_t> rgba = readBack(*sm.getTexture(id));
            double sum = 0.0;
            for (size_t p = 0; p < rgba.size(); p += 4) sum += rgba[p] + rgba[p + 1] + rgba[p + 2];
            EXPECT_GT(sum / static_cast<double>(rgba.size() / 4 * 3), 4.0) << "upload " << uploads << " is black";
        }
        if (!video->isHardwareDecoding()) {
            DxInterop::testHooks() = DxInterop::TestHooks{};
            GTEST_SKIP() << "no GPU decoding here: " << video->getDecodeNote();
        }
        EXPECT_FALSE(video->isZeroCopy());
        EXPECT_NE(video->getZeroCopyNote().find("lock"), std::string::npos) << video->getZeroCopyNote();
        sm.removeSource(id);
        EXPECT_EQ(DxInterop::liveObjects(), 0);
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
    EXPECT_TRUE(wasZeroCopy);
    EXPECT_GE(uploadsBefore, 5);
    EXPECT_GE(uploadsAfter, 10) << "frames kept coming after the fallback";
    EXPECT_EQ(VideoDecoder::liveGpuFrames(), 0) << "every frame left on the GPU was released";
}
