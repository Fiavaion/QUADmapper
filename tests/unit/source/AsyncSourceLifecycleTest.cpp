// TD-35: SourceManager opens video files and frees removed ones off the main thread.
// A source is registered at once and becomes ready when its decoder is open; removing
// it, replacing it or destroying the manager at any point in that lifecycle must not
// leak, wait on the main thread for a decode thread, or touch a freed object. Clips are
// encoded at test time; the zero-copy case needs local media and a GPU decoder.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "renderer/DxInterop.hpp"
#include "renderer/Texture.hpp"
#include "source/FrameData.hpp"
#include "source/SourceManager.hpp"
#include "source/VideoFileSource.hpp"
#include "video/VideoDecoder.hpp"
#include "../video/TestClips.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int FPS = 25;
constexpr int FRAMES = 75;  // 3 s
const auto WAIT_LIMIT = std::chrono::seconds(10);

struct GLContext {
    GLFWwindow* win = nullptr;
    bool ok = false;
    GLContext() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "async-source-test", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~GLContext() { if (win) { glfwDestroyWindow(win); glfwTerminate(); } }
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;
};

class AsyncSourceLifecycle : public testing::Test {
protected:
    void SetUp() override {
        if (!gl_.ok) GTEST_SKIP() << "no GL 3.3 context";
        std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        clip_ = fs::temp_directory_path() / ("qm_async_" + name + ".mp4");
        if (!testclips::writeMpeg2Clip(clip_, 12, FRAMES, FPS, 0)) GTEST_SKIP() << "no encoder";
        gpuFrames_ = VideoDecoder::liveGpuFrames();
        d3dObjects_ = VideoDecoder::liveD3dObjects();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove(clip_, ec);
    }

    // Pump updateAll() (real time, bounded) until `id` shows a frame.
    static bool showsFrame(SourceManager& sm, const UUID& id) {
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (std::chrono::steady_clock::now() < deadline) {
            sm.updateAll(1.0 / 60.0);
            const TextureSource* source = sm.getSource(id);
            if (source && source->getCurrentFrame()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    void expectNothingHeld() const {
        EXPECT_EQ(VideoDecoder::liveGpuFrames(), gpuFrames_);
        EXPECT_EQ(VideoDecoder::liveD3dObjects(), d3dObjects_);
    }

    GLContext gl_;
    fs::path clip_;
    long gpuFrames_ = 0;
    long d3dObjects_ = 0;
};

}  // namespace

// The source exists at once, not ready, and becomes ready without the caller waiting.
TEST_F(AsyncSourceLifecycle, ASourceIsRegisteredAtOnceAndReadyOnceOpen) {
    SourceManager sm;
    const UUID id = sm.createVideoSource(clip_);
    ASSERT_FALSE(id.isNull());
    auto* video = sm.getSourceAs<VideoFileSource>(id);
    ASSERT_NE(video, nullptr);
    EXPECT_TRUE(sm.hasSource(id));
    EXPECT_EQ(sm.getSourcePath(id), clip_);
    EXPECT_EQ(sm.findVideoSourceByPath(clip_), id);  // deduplicated while it opens
    EXPECT_EQ(sm.getOrCreateVideoSource(clip_), id);
    EXPECT_EQ(sm.getSourceCount(), 1u);
    ASSERT_TRUE(showsFrame(sm, id));
    EXPECT_TRUE(video->isReady());
    EXPECT_EQ(sm.getOpeningCount(), 0u);
    EXPECT_GT(video->getVideoInfo().width, 0);
}

// Play, loop, speed and a seek given while the file opens are kept, and the clock ran
// meanwhile: the source starts where it would have been had it opened at once.
TEST_F(AsyncSourceLifecycle, TransportGivenWhileOpeningCarriesOver) {
    SourceManager sm;
    const UUID id = sm.createVideoSource(clip_);
    auto* playable = sm.getSourceAs<PlayableSource>(id);
    ASSERT_NE(playable, nullptr);
    playable->setLoopMode(LoopMode::LOOP);
    playable->setSpeed(1.0f);
    playable->seek(1.0);
    playable->play();
    const auto requested = std::chrono::steady_clock::now();
    EXPECT_EQ(playable->getPlaybackState(), PlaybackState::PLAYING);
    EXPECT_EQ(playable->getLoopMode(), LoopMode::LOOP);

    sm.finishBackgroundWork();
    sm.updateAll(0.0);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - requested).count();
    EXPECT_TRUE(sm.getSource(id)->isReady());
    EXPECT_EQ(playable->getPlaybackState(), PlaybackState::PLAYING);
    EXPECT_EQ(playable->getLoopMode(), LoopMode::LOOP);
    const double duration = playable->getDuration();
    ASSERT_GT(duration, 2.0);
    const double expected = std::fmod(1.0 + elapsed, duration);
    EXPECT_NEAR(playable->getCurrentTime(), expected, 0.1);
    ASSERT_TRUE(showsFrame(sm, id));
}

// Removed before its file opened: nothing is left once the open finishes, and the
// decoder it opened is freed off the main thread.
TEST_F(AsyncSourceLifecycle, RemoveDuringOpen) {
    SourceManager sm;
    int removed = 0;
    sm.setSourceRemovedCallback([&removed](const UUID&) { ++removed; });
    const UUID id = sm.createVideoSource(clip_);
    sm.removeSource(id);
    EXPECT_FALSE(sm.hasSource(id));
    EXPECT_EQ(removed, 1);
    sm.finishBackgroundWork();
    EXPECT_EQ(sm.getOpeningCount(), 0u);
    EXPECT_EQ(sm.getSourceCount(), 0u);
    for (int i = 0; i < 5; ++i) sm.updateAll(1.0 / 60.0);  // nothing comes back
    EXPECT_EQ(sm.getSourceCount(), 0u);
    expectNothingHeld();
}

// A clip replaced again and again before any open finished (the media bin's hover
// preview, or a quick undo/redo): only the last one becomes a source.
TEST_F(AsyncSourceLifecycle, ReplaceDuringOpen) {
    SourceManager sm;
    UUID id = sm.createVideoSource(clip_);
    for (int i = 0; i < 5; ++i) {
        sm.removeSource(id);
        id = sm.createVideoSource(clip_);
    }
    ASSERT_TRUE(showsFrame(sm, id));
    EXPECT_EQ(sm.getSourceCount(), 1u);
    sm.finishBackgroundWork();
    EXPECT_EQ(sm.getOpeningCount(), 0u);
}

// Removed while it plays: the decode thread is joined off the main thread.
TEST_F(AsyncSourceLifecycle, RemoveWhilePlaying) {
    SourceManager sm;
    const UUID id = sm.createVideoSource(clip_);
    sm.getSourceAs<PlayableSource>(id)->play();
    ASSERT_TRUE(showsFrame(sm, id));
    sm.removeSource(id);
    EXPECT_FALSE(sm.hasSource(id));
    EXPECT_EQ(sm.getTexture(id), nullptr);
    sm.finishBackgroundWork();
    expectNothingHeld();
}

// The manager destroyed with files still opening and sources still being freed (project
// replace, app exit): every worker is joined before what it uses is gone.
TEST_F(AsyncSourceLifecycle, ShutdownDuringOpenAndTeardown) {
    for (int round = 0; round < 3; ++round) {
        SourceManager sm;
        std::vector<UUID> ids;
        for (int i = 0; i < 4; ++i) ids.push_back(sm.createVideoSource(clip_));
        if (round > 0) {
            ASSERT_TRUE(showsFrame(sm, ids[0]));  // one up and decoding
            sm.getSourceAs<PlayableSource>(ids[0])->play();
        }
        sm.removeSource(ids[1]);  // being freed
        if (round == 2) sm.clear();
    }  // destroyed here
    expectNothingHeld();
}

// A replacement shows the clip it replaces until its own first frame: no blank frame.
TEST_F(AsyncSourceLifecycle, AStandInShowsUntilTheFirstFrame) {
    SourceManager sm;
    const UUID original = sm.createVideoSource(clip_);
    ASSERT_TRUE(showsFrame(sm, original));
    Texture* originalTexture = sm.getTexture(original);
    ASSERT_NE(originalTexture, nullptr);

    const UUID replacement = sm.createVideoSource(clip_);
    sm.setStandIn(replacement, original);
    EXPECT_EQ(sm.getTexture(replacement), originalTexture);
    EXPECT_EQ(sm.getTextureMap({replacement}).at(replacement), originalTexture);
    ASSERT_TRUE(showsFrame(sm, replacement));
    EXPECT_NE(sm.getTexture(replacement), originalTexture);
    EXPECT_GT(sm.getTexture(replacement)->getWidth(), 0);
}

// A file that exists but cannot be a video is refused outright, as a missing one is, so
// the caller keeps it as missing media (TD-35 review: it became a source that never
// showed anything): bytes no container reads, and a container with sound and no video.
TEST_F(AsyncSourceLifecycle, AFileThatIsNotAVideoIsRefusedOutright) {
    const fs::path junk = fs::temp_directory_path() / "qm_async_not_a_video.mp4";
    {
        std::ofstream out(junk, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 4096; ++i) out.put(static_cast<char>((i * 131) & 0xff));
    }
    const fs::path sound = fs::temp_directory_path() / "qm_async_sound_only.mp4";
    {
        std::ofstream out(sound, std::ios::binary | std::ios::trunc);
        const auto u32 = [&out](uint32_t v) { for (int i = 0; i < 4; ++i) out.put(static_cast<char>((v >> (8 * i)) & 0xff)); };
        const auto u16 = [&out](uint16_t v) { out.put(static_cast<char>(v & 0xff)); out.put(static_cast<char>(v >> 8)); };
        const uint32_t samples = 8000;
        out.write("RIFF", 4);
        u32(36 + samples * 2);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);  // PCM, mono, 8 kHz, 16-bit
        u16(1);
        u32(8000);
        u32(16000);
        u16(2);
        u16(16);
        out.write("data", 4);
        u32(samples * 2);
        for (uint32_t i = 0; i < samples; ++i) u16(static_cast<uint16_t>((i * 97) & 0x3fff));
    }
    SourceManager sm;
    EXPECT_TRUE(sm.createVideoSource(fs::temp_directory_path() / "qm_async_no_such_file.mp4").isNull());
    EXPECT_TRUE(sm.createVideoSource(junk).isNull());
    EXPECT_TRUE(sm.getOrCreateVideoSource(sound).isNull());
    EXPECT_EQ(sm.getSourceCount(), 0u);
    EXPECT_EQ(sm.getOpeningCount(), 0u);
    std::error_code ec;
    fs::remove(junk, ec);
    fs::remove(sound, ec);
}

// A file that passed the probe but whose open fails on the opener thread, here by an
// exception (TD-35 review: one left the source opening forever, getOpeningCount never
// drained): the open finishes as failed, the source stays empty, says why, keeps the
// transport it was given and is listed once by takeFailedOpens().
TEST_F(AsyncSourceLifecycle, AnOpenThatFailsLeavesAnEmptySourceThatKeepsItsTransport) {
    SourceManager sm;
    sm.setOpenHookForTesting([](const fs::path&) { throw std::runtime_error("cannot open this stream"); });
    const UUID id = sm.createVideoSource(clip_);
    ASSERT_FALSE(id.isNull());
    auto* playable = sm.getSourceAs<PlayableSource>(id);
    ASSERT_NE(playable, nullptr);
    playable->setLoopMode(LoopMode::PING_PONG);
    playable->play();

    sm.finishBackgroundWork();
    EXPECT_EQ(sm.getOpeningCount(), 0u) << "the open never finished";
    EXPECT_FALSE(sm.getSource(id)->isReady());
    EXPECT_FALSE(sm.getSourceAs<VideoFileSource>(id)->isOpening());
    EXPECT_FALSE(playable->getErrorMessage().empty());
    EXPECT_EQ(playable->getPlaybackState(), PlaybackState::PLAYING) << "the failed open reset the transport";
    EXPECT_EQ(playable->getLoopMode(), LoopMode::PING_PONG);
    EXPECT_EQ(sm.takeFailedOpens(), std::vector<UUID>{id});
    EXPECT_TRUE(sm.takeFailedOpens().empty());
    for (int i = 0; i < 3; ++i) sm.updateAll(1.0 / 60.0);
    EXPECT_EQ(sm.getSource(id)->getCurrentFrame(), nullptr);
    EXPECT_FALSE(sm.getTexture(id)->isValid());
    sm.removeSource(id);
    sm.finishBackgroundWork();
}

// A source removed while its open waits behind another on the opener thread is never
// opened: retire() cancels it (TD-35 review: no test noticed when the cancel was gone).
TEST_F(AsyncSourceLifecycle, ASourceRemovedBeforeItsOpenStartsIsNeverOpened) {
    SourceManager sm;
    std::atomic<int> opens{0};
    std::atomic<bool> release{false};
    sm.setOpenHookForTesting([&](const fs::path&) {
        if (opens.fetch_add(1) != 0) return;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // the first open waits
    });
    const UUID first = sm.createVideoSource(clip_);
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (opens == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const UUID queued = sm.createVideoSource(clip_);  // waits behind the first
    sm.removeSource(queued);
    release = true;
    sm.finishBackgroundWork();
    EXPECT_EQ(opens, 1) << "the removed source's file was opened anyway";
    EXPECT_FALSE(sm.hasSource(queued));
    EXPECT_TRUE(sm.getSource(first)->isReady());
    EXPECT_EQ(sm.getOpeningCount(), 0u);
}

// Zero-copy (ADR-056): the opener prepares the interop device, so the first frame only
// opens it for OpenGL; removing the source mid-decode frees every D3D11 object.
TEST_F(AsyncSourceLifecycle, ZeroCopySourceRemovedMidDecodeFreesEveryD3dObject) {
    const fs::path h264 = "D:/testing Videos/greenCrystals_15Sec.mp4";
    if (!fs::exists(h264)) GTEST_SKIP() << "test media not present";
    {
        SourceManager sm;
        const UUID id = sm.createVideoSource(h264);
        auto* video = sm.getSourceAs<VideoFileSource>(id);
        ASSERT_NE(video, nullptr);
        video->play();
        ASSERT_TRUE(showsFrame(sm, id));
        if (!video->isZeroCopy()) GTEST_SKIP() << "no zero-copy here: " << video->getZeroCopyNote();
        EXPECT_LE(DxInterop::liveDevices(), 1);
        for (int i = 0; i < 20; ++i) {
            sm.updateAll(1.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sm.removeSource(id);
        sm.finishBackgroundWork();
        EXPECT_EQ(DxInterop::liveObjects(), 0);
        expectNothingHeld();
    }
    EXPECT_EQ(DxInterop::liveDevices(), 0);
}
