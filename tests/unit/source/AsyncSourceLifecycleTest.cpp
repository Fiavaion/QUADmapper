// TD-35: SourceManager opens video files and frees removed ones off the main thread.
// A source is registered at once and becomes ready when its decoder is open; removing
// it, replacing it or destroying the manager at any point in that lifecycle must not
// leak, wait on the main thread for a decode thread, or touch a freed object. Clips are
// encoded at test time; the zero-copy case needs local media and a GPU decoder.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "core/GlObjectStats.hpp"
#include "core/MainThreadTimings.hpp"
#include "renderer/DxInterop.hpp"
#include "renderer/Texture.hpp"
#include "source/FrameData.hpp"
#include "source/HapFileSource.hpp"
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

    // Pump updateAll() (real time, bounded) until `id` shows a frame on its texture.
    static bool showsFrame(SourceManager& sm, const UUID& id) {
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (std::chrono::steady_clock::now() < deadline) {
            sm.updateAll(1.0 / 60.0);
            const TextureSource* source = sm.getSource(id);
            if (source && source->getCurrentFrame() && sm.showsCurrentFrame(id)) return true;
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

// The GL state of removed sources is freed by the frames that follow (TD-35: never all in
// one call, never waiting on the GPU or a copy), until nothing of theirs is left, on the
// path through memory (upload states, pixel buffers, copies in flight) and, where the
// driver shares D3D11 textures, on the zero-copy path, where up to DxInterop::MAX_SPARES
// sources' shared planes are kept for later sources (released by finishBackgroundWork).
TEST_F(AsyncSourceLifecycle, RemovedSourcesGlStateIsFreedByTheFramesThatFollow) {
    for (const bool throughMemory : {true, false}) {
        SCOPED_TRACE(throughMemory ? "through memory" : "zero-copy where available");
        DxInterop::testHooks().extensionMissing = throughMemory;
        {
            SourceManager sm;
            const UUID first = sm.createVideoSource(clip_);
            sm.getSourceAs<PlayableSource>(first)->play();
            ASSERT_TRUE(showsFrame(sm, first));  // the converter's own GL objects now exist
            for (int i = 0; i < 5; ++i) sm.updateAll(1.0 / 60.0);
            const auto baseline = GlObjectStats::Snapshot::capture();

            std::vector<UUID> ids;
            for (size_t i = 0; i < DxInterop::MAX_SPARES + 2; ++i) ids.push_back(sm.createVideoSource(clip_));
            for (const UUID& id : ids) {
                sm.getSourceAs<PlayableSource>(id)->play();
                ASSERT_TRUE(showsFrame(sm, id));
            }
            for (int i = 0; i < 10; ++i) sm.updateAll(1.0 / 60.0);  // copies and prefetches in flight
            for (const UUID& id : ids) sm.removeSource(id);
            EXPECT_FALSE(GlObjectStats::Snapshot::capture().isFlat(baseline)) << "freed inside removeSource()";
            EXPECT_EQ(sm.getRetiredGlCount(), ids.size());

            const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
            while (sm.getRetiredGlCount() > 0 && std::chrono::steady_clock::now() < deadline) {
                sm.updateAll(1.0 / 60.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_EQ(sm.getRetiredGlCount(), 0u);
            if (throughMemory) {
                const auto after = GlObjectStats::Snapshot::capture();
                EXPECT_TRUE(after.isFlat(baseline)) << after.diff(baseline);
            }
            sm.finishBackgroundWork();  // also the shared planes kept for later sources
            const auto after = GlObjectStats::Snapshot::capture();
            EXPECT_TRUE(after.isFlat(baseline)) << after.diff(baseline);
            EXPECT_EQ(DxInterop::liveObjects(), 0);
            expectNothingHeld();
        }
    }
    DxInterop::testHooks().extensionMissing = false;
}

// TD-35: a zero-copy clip removed and opened again (undo, a preview, Convert to HAP's
// stand-in) takes over the shared planes the removed one kept: no plane is unregistered
// or registered again. (ZeroCopyPlayback.AReopenedClipShowsItsOwnFramesThroughTheKeptPlanes
// checks the frames it then shows.)
TEST_F(AsyncSourceLifecycle, AReopenedZeroCopyClipTakesOverTheKeptSharedPlanes) {
    const fs::path h264 = "D:/testing Videos/greenCrystals_15Sec.mp4";
    if (!fs::exists(h264)) GTEST_SKIP() << "test media not present";
    MainThreadTimings::markMainThread();
    MainThreadTimings::reset();
    auto registrations = [] {
        for (const auto& s : MainThreadTimings::summarize()) {
            if (s.op == "zeroCopyPlaneRegister") return s.count;
        }
        return size_t(0);
    };
    {
        SourceManager sm;
        const UUID first = sm.createVideoSource(h264);
        sm.getSourceAs<PlayableSource>(first)->play();
        ASSERT_TRUE(showsFrame(sm, first));
        if (!sm.getSourceAs<VideoFileSource>(first)->isZeroCopy()) {
            MainThreadTimings::reset();
            GTEST_SKIP() << "no zero-copy here";
        }
        EXPECT_EQ(registrations(), 2u);
        const long held = DxInterop::liveObjects();
        sm.removeSource(first);
        for (int i = 0; i < 5; ++i) sm.updateAll(1.0 / 60.0);
        EXPECT_GT(DxInterop::liveObjects(), 0) << "the shared planes were not kept";

        const UUID again = sm.createVideoSource(h264);
        auto* video = sm.getSourceAs<VideoFileSource>(again);
        video->seek(5.0);
        video->play();
        ASSERT_TRUE(showsFrame(sm, again));
        EXPECT_EQ(registrations(), 2u) << "the planes were registered again";
        EXPECT_LE(DxInterop::liveObjects(), held);
        EXPECT_TRUE(video->isZeroCopy()) << video->getZeroCopyNote();
        sm.removeSource(again);
        sm.finishBackgroundWork();
        EXPECT_EQ(DxInterop::liveObjects(), 0);
    }
    MainThreadTimings::reset();
}

// TD-35: kept shared planes hold video memory (about 100 MB a target at 8K P010), so they
// go once no source has taken them for DxInterop::SPARE_LIFETIME (shortened here), and at
// once when the project is replaced (clear()), freed by the updates that follow.
TEST_F(AsyncSourceLifecycle, KeptSharedPlanesAreReleasedWhenUnusedAndOnClear) {
    const fs::path h264 = "D:/testing Videos/greenCrystals_15Sec.mp4";
    if (!fs::exists(h264)) GTEST_SKIP() << "test media not present";
    constexpr auto LIFETIME = std::chrono::milliseconds(300);
    DxInterop::testHooks().spareLifetimeMs = static_cast<int>(LIFETIME.count());
    {
        SourceManager sm;
        const UUID first = sm.createVideoSource(h264);
        sm.getSourceAs<PlayableSource>(first)->play();
        ASSERT_TRUE(showsFrame(sm, first));
        if (!sm.getSourceAs<VideoFileSource>(first)->isZeroCopy()) {
            DxInterop::testHooks() = DxInterop::TestHooks{};
            GTEST_SKIP() << "no zero-copy here";
        }
        sm.removeSource(first);
        const auto removed = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) sm.updateAll(1.0 / 60.0);
        EXPECT_GT(DxInterop::liveObjects(), 0) << "the shared planes were not kept";
        auto deadline = removed + WAIT_LIMIT;
        while (DxInterop::liveObjects() > 0 && std::chrono::steady_clock::now() < deadline) {
            sm.updateAll(1.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_EQ(DxInterop::liveObjects(), 0) << "kept planes no source took were never released";
        EXPECT_GE(std::chrono::steady_clock::now() - removed, LIFETIME);

        DxInterop::testHooks().spareLifetimeMs = -1;  // SPARE_LIFETIME: clear() must release them itself
        const UUID second = sm.createVideoSource(h264);
        sm.getSourceAs<PlayableSource>(second)->play();
        ASSERT_TRUE(showsFrame(sm, second));
        sm.clear();
        deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (sm.getRetiredGlCount() > 0 && std::chrono::steady_clock::now() < deadline) {
            sm.updateAll(1.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(sm.getRetiredGlCount(), 0u);
        EXPECT_EQ(DxInterop::liveObjects(), 0) << "clear() kept shared planes for later sources";
        sm.finishBackgroundWork();
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
}

// TD-35: while a zero-copy source's first frame waits for its set-up (DxInterop::setUp:
// at least two updates, one plane registered in each), the source holds a frame its
// texture does not show yet. A replacement shows the clip it replaces (setStandIn); a
// source without a stand-in keeps a texture with no storage, which buildQuadTextureMap
// draws black, never as the renderer's white default.
TEST_F(AsyncSourceLifecycle, AZeroCopySourceWaitingForItsSetUpShowsItsStandInOrBlack) {
    const fs::path h264 = "D:/testing Videos/greenCrystals_15Sec.mp4";
    if (!fs::exists(h264)) GTEST_SKIP() << "test media not present";
    SourceManager sm;
    const UUID original = sm.createVideoSource(clip_);
    ASSERT_TRUE(showsFrame(sm, original));
    Texture* originalTexture = sm.getTexture(original);
    ASSERT_NE(originalTexture, nullptr);

    const UUID replacement = sm.createVideoSource(h264);
    sm.setStandIn(replacement, original);
    const UUID fresh = sm.createVideoSource(h264);
    sm.getSourceAs<PlayableSource>(replacement)->play();
    sm.getSourceAs<PlayableSource>(fresh)->play();
    auto waiting = [&](const UUID& id) {
        const TextureSource* source = sm.getSource(id);
        return source && source->getCurrentFrame() && !sm.showsCurrentFrame(id);
    };
    int replacementWaits = 0;
    int freshWaits = 0;
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (!(sm.showsCurrentFrame(replacement) && sm.showsCurrentFrame(fresh)) &&
           std::chrono::steady_clock::now() < deadline) {
        sm.updateAll(1.0 / 60.0);
        if (waiting(replacement)) {
            ++replacementWaits;
            EXPECT_EQ(sm.getTexture(replacement), originalTexture) << "the stand-in was not shown";
        }
        if (waiting(fresh)) {
            ++freshWaits;
            const Texture* texture = sm.getTexture(fresh);
            ASSERT_NE(texture, nullptr);
            EXPECT_FALSE(texture->isValid()) << "a texture with storage but no frame in it was shown";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(sm.showsCurrentFrame(replacement) && sm.showsCurrentFrame(fresh));
    if (!sm.getSourceAs<VideoFileSource>(replacement)->isZeroCopy()) GTEST_SKIP() << "no zero-copy here";
    EXPECT_GE(replacementWaits, 2);
    EXPECT_GE(freshWaits, 2);
    EXPECT_NE(sm.getTexture(replacement), originalTexture);
    sm.clear();
    sm.finishBackgroundWork();
}

namespace {

// A GPU read of `texture` (blitted into the window's framebuffer), as a draw sampling it is.
void readOnGpu(const Texture& texture) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.getId(), 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, texture.getWidth(), texture.getHeight(), 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
}

}  // namespace

// TD-35: a removed source's texture, read by the GPU just before, is freed only once its
// fence reports the GPU done: held while the fence reads as unsignalled (a test hook),
// freed once it signals.
TEST_F(AsyncSourceLifecycle, RemovedGlStateWaitsForItsFence) {
    DxInterop::testHooks().extensionMissing = true;  // through memory: nothing kept for later sources
    {
        SourceManager sm;
        const UUID id = sm.createVideoSource(clip_);
        ASSERT_TRUE(showsFrame(sm, id));
        const Texture* texture = sm.getTexture(id);
        const GLuint name = texture->getId();
        readOnGpu(*texture);
        sm.setFenceHooksForTesting(true);
        sm.removeSource(id);
        for (int i = 0; i < 30; ++i) {
            sm.updateAll(1.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(sm.getRetiredGlCount(), 1u);
        EXPECT_TRUE(glIsTexture(name)) << "freed before its fence signalled";

        sm.setFenceHooksForTesting(false);
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (sm.getRetiredGlCount() > 0 && std::chrono::steady_clock::now() < deadline) {
            sm.updateAll(1.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(sm.getRetiredGlCount(), 0u);
        EXPECT_FALSE(glIsTexture(name));
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
}

// TD-35: a fence that never signals (a GPU reset or lost device) would hold everything
// removed after it for the rest of the session: past SourceManager::FENCE_LIMIT
// (shortened here) it is taken as signalled and the state freed.
TEST_F(AsyncSourceLifecycle, AFenceThatNeverSignalsIsTakenAsSignalledAfterItsLimit) {
    DxInterop::testHooks().extensionMissing = true;
    constexpr auto LIMIT = std::chrono::milliseconds(200);
    {
        SourceManager sm;
        const UUID id = sm.createVideoSource(clip_);
        ASSERT_TRUE(showsFrame(sm, id));
        sm.setFenceHooksForTesting(true, LIMIT);
        sm.removeSource(id);
        const auto removed = std::chrono::steady_clock::now();
        const auto deadline = removed + WAIT_LIMIT;
        while (sm.getRetiredGlCount() > 0 && std::chrono::steady_clock::now() < deadline) {
            sm.updateAll(1.0 / 60.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(sm.getRetiredGlCount(), 0u) << "held for good behind a fence that never signals";
        EXPECT_GE(std::chrono::steady_clock::now() - removed, LIMIT);
    }
    DxInterop::testHooks() = DxInterop::TestHooks{};
}

// A HAP clip opens on the opener thread too (TD-35: its container took up to 18 ms on the
// main thread). Registered at once, not ready; transport given meanwhile carries over;
// ready once the container is open, with frames demuxed from where the clock is.
TEST_F(AsyncSourceLifecycle, AHapClipOpensOffTheMainThreadAndKeepsItsTransport) {
    const fs::path hap = fs::temp_directory_path() / "qm_async_hap_opens.mov";
    if (!testclips::writeHapClip(hap, FRAMES, FPS)) GTEST_SKIP() << "FFmpeg cannot write HAP";
    SourceManager sm;
    std::atomic<bool> release{false};
    std::atomic<std::thread::id> openedOn{};
    sm.setOpenHookForTesting([&](const fs::path&) {
        openedOn = std::this_thread::get_id();
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    const UUID id = sm.createVideoSource(hap);
    ASSERT_FALSE(id.isNull());
    auto* source = sm.getSourceAs<HapFileSource>(id);
    ASSERT_NE(source, nullptr);
    EXPECT_TRUE(source->isOpening());
    EXPECT_FALSE(source->isReady());
    EXPECT_EQ(sm.getOpeningCount(), 1u);
    EXPECT_EQ(sm.findVideoSourceByPath(hap), id);  // deduplicated while it opens
    source->setLoopMode(LoopMode::LOOP);
    source->play();
    source->seek(1.0);
    source->pause();  // the clock stops at 1 s: the position is exact after the open
    for (int i = 0; i < 3; ++i) sm.updateAll(1.0 / 60.0);  // the open waits; frames go on
    EXPECT_FALSE(source->isReady());
    EXPECT_EQ(sm.getSource(id)->getCurrentFrame(), nullptr);

    release = true;
    ASSERT_TRUE(showsFrame(sm, id));
    EXPECT_NE(openedOn.load(), std::this_thread::get_id());
    EXPECT_TRUE(source->isReady());
    EXPECT_FALSE(source->isOpening());
    EXPECT_EQ(sm.getOpeningCount(), 0u);
    EXPECT_EQ(source->getPlaybackState(), PlaybackState::PAUSED);
    EXPECT_EQ(source->getLoopMode(), LoopMode::LOOP);
    EXPECT_NEAR(source->getCurrentTime(), 1.0, 0.05) << "the seek given while it opened was lost";
    const FrameData* frame = sm.getSource(id)->getCurrentFrame();
    ASSERT_NE(frame, nullptr);
    EXPECT_NEAR(static_cast<double>(frame->timestamp) / 1e6, 1.0, 1.0 / FPS + 1e-3)
        << "the first frame shown is not the one at the clock's position";
    EXPECT_GT(source->getWidth(), 0);
    sm.removeSource(id);
    sm.finishBackgroundWork();
    std::error_code ec;
    fs::remove(hap, ec);
}

// A HAP clip removed while its container opens: nothing comes back once the open ends,
// and the container it opened is closed off the main thread.
TEST_F(AsyncSourceLifecycle, AHapClipRemovedDuringItsOpen) {
    const fs::path hap = fs::temp_directory_path() / "qm_async_hap_removed.mov";
    if (!testclips::writeHapClip(hap, FRAMES, FPS)) GTEST_SKIP() << "FFmpeg cannot write HAP";
    SourceManager sm;
    std::atomic<bool> inOpen{false};
    std::atomic<bool> release{false};
    sm.setOpenHookForTesting([&](const fs::path&) {
        inOpen = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    const UUID id = sm.createVideoSource(hap);
    ASSERT_FALSE(id.isNull());
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (!inOpen && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(inOpen);
    sm.removeSource(id);  // mid-open
    EXPECT_FALSE(sm.hasSource(id));
    release = true;
    sm.finishBackgroundWork();
    EXPECT_EQ(sm.getOpeningCount(), 0u);
    for (int i = 0; i < 5; ++i) sm.updateAll(1.0 / 60.0);
    EXPECT_EQ(sm.getSourceCount(), 0u);
    EXPECT_TRUE(sm.takeFailedOpens().empty());
    std::error_code ec;
    fs::remove(hap, ec);
}

// A HAP clip whose open fails off the main thread is left empty, says why, keeps its
// transport and is listed once by takeFailedOpens(), as a video's is.
TEST_F(AsyncSourceLifecycle, AHapOpenThatFailsLeavesAnEmptySourceThatKeepsItsTransport) {
    const fs::path hap = fs::temp_directory_path() / "qm_async_hap_fails.mov";
    if (!testclips::writeHapClip(hap, FRAMES, FPS)) GTEST_SKIP() << "FFmpeg cannot write HAP";
    SourceManager sm;
    sm.setOpenHookForTesting([](const fs::path&) { throw std::runtime_error("cannot open this stream"); });
    const UUID id = sm.createVideoSource(hap);
    auto* source = sm.getSourceAs<HapFileSource>(id);
    ASSERT_NE(source, nullptr);
    source->setLoopMode(LoopMode::PING_PONG);
    source->play();
    sm.finishBackgroundWork();
    EXPECT_EQ(sm.getOpeningCount(), 0u);
    EXPECT_FALSE(source->isReady());
    EXPECT_FALSE(source->isOpening());
    EXPECT_FALSE(source->getErrorMessage().empty());
    EXPECT_EQ(source->getPlaybackState(), PlaybackState::PLAYING);
    EXPECT_EQ(source->getLoopMode(), LoopMode::PING_PONG);
    EXPECT_EQ(sm.takeFailedOpens(), std::vector<UUID>{id});
    sm.removeSource(id);
    sm.finishBackgroundWork();
    std::error_code ec;
    fs::remove(hap, ec);
}

// TD-35: a zero-copy source's first frame costs the main thread at most one costly GL
// call an update: the interop device the opener prepared is opened for OpenGL in one
// update, each shared plane registered in another (their D3D11 textures made on a
// worker); never the whole set-up in the frame that first shows the source.
TEST_F(AsyncSourceLifecycle, AZeroCopySourcesFirstFrameIsSetUpOneGlCallAnUpdate) {
    const fs::path h264 = "D:/testing Videos/greenCrystals_15Sec.mp4";
    if (!fs::exists(h264)) GTEST_SKIP() << "test media not present";
    MainThreadTimings::markMainThread();
    MainThreadTimings::reset();
    auto count = [](const char* op) {
        for (const auto& s : MainThreadTimings::summarize()) {
            if (s.op == op) return s.count;
        }
        return size_t(0);
    };
    auto glCalls = [&] { return count("zeroCopyDeviceOpen") + count("zeroCopyPlaneRegister") + count("zeroCopyTargetBuild"); };
    {
        SourceManager sm;
        const UUID id = sm.createVideoSource(h264);
        auto* video = sm.getSourceAs<VideoFileSource>(id);
        ASSERT_NE(video, nullptr);
        video->play();
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        size_t worst = 0;
        while (!sm.showsCurrentFrame(id) && std::chrono::steady_clock::now() < deadline) {
            const size_t before = glCalls();
            sm.updateAll(1.0 / 60.0);
            worst = std::max(worst, glCalls() - before);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_TRUE(sm.showsCurrentFrame(id));
        if (!video->isZeroCopy()) {
            MainThreadTimings::reset();
            GTEST_SKIP() << "no zero-copy here: " << video->getZeroCopyNote();
        }
        EXPECT_LE(worst, 1u) << "an update made more than one costly zero-copy GL call";
        EXPECT_EQ(count("zeroCopyDeviceOpen"), 1u);
        EXPECT_EQ(count("zeroCopyPlaneRegister"), 2u);
        EXPECT_EQ(count("zeroCopyTargetBuild"), 0u) << "the planes were built at once";
        sm.removeSource(id);
        sm.finishBackgroundWork();
    }
    MainThreadTimings::reset();
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
