// Deterministic pipeline test for ADR-040 P3: a video file becomes a SourceManager
// source whose frames decode and upload to a GPU texture (what the renderer now
// reads via mediaId). Avoids the brittle GUI smoke. Needs a real GL 3.3 context
// AND the test video; skips otherwise (CI-safe).

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "source/SourceManager.hpp"
#include "source/VideoFileSource.hpp"
#include "source/FrameData.hpp"
#include "renderer/Texture.hpp"
#include "core/UUID.hpp"

#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdint>
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
        win = glfwCreateWindow(64, 64, "video-test", nullptr, nullptr);
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
}  // namespace

TEST(VideoSourcePipeline, LoadsAdvancesAndUploads) {
    namespace fs = std::filesystem;
    const fs::path video = "D:/testing Videos/greenCrystals_15Sec_4x3.mp4";
    if (!fs::exists(video)) GTEST_SKIP() << "test video not present";

    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    SourceManager sm;

    // Load + register: a non-null id proves initialize() (file open) + the id fix.
    UUID id = sm.createVideoSource(video);
    ASSERT_FALSE(id.isNull());

    auto* v = sm.getSourceAs<VideoFileSource>(id);
    ASSERT_NE(v, nullptr);
    EXPECT_GT(v->getVideoInfo().width, 0);    // file actually opened
    EXPECT_GT(v->getDuration(), 0.0);

    v->play();

    // Pump updateAll with real time passing so the background decoder fills and
    // playback advances; the GPU texture should become valid.
    bool textureValid = false;
    for (int i = 0; i < 200; ++i) {
        sm.updateAll(0.016);
        Texture* t = sm.getTexture(id);
        if (t && t->isValid()) textureValid = true;
        if (textureValid && v->getCurrentTime() > 0.1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    EXPECT_TRUE(textureValid);             // decode -> upload path is wired
    EXPECT_GT(v->getCurrentTime(), 0.0);   // playback advanced -> decoding works
}

// QM-088 / D4: a project load restores a video's transport through the PlayableSource
// interface (Application::createSourceFrom: setLoopMode, setSpeed, play). The source
// must then actually play, with its clock on the wall clock (ADR-050) and frames never
// ahead of it; a saved ping-pong is real Ping-Pong again.
TEST(VideoSourcePipeline, RestoredTransportPlaysAtClockPace) {
    namespace fs = std::filesystem;
    const fs::path video = "D:/testing Videos/greenCrystals_15Sec_4x3.mp4";
    if (!fs::exists(video)) GTEST_SKIP() << "test video not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    SourceManager sm;
    const UUID id = sm.getOrCreateVideoSource(video);
    auto* playable = sm.getSourceAs<PlayableSource>(id);
    ASSERT_NE(playable, nullptr);

    playable->setLoopMode(LoopMode::PING_PONG);  // as saved by an old file
    playable->setSpeed(1.0f);
    const auto started = std::chrono::steady_clock::now();
    playable->play();
    EXPECT_EQ(playable->getLoopMode(), LoopMode::PING_PONG);
    EXPECT_TRUE(playable->getLoopModeNotice().empty()) << playable->getLoopModeNotice();
    EXPECT_EQ(playable->getPlaybackState(), PlaybackState::PLAYING);

    // About a second of updates; the frame on screen must never be ahead of the clock.
    for (int i = 0; i < 60; ++i) {
        sm.updateAll(1.0 / 60.0);
        if (const FrameData* frame = playable->getCurrentFrame()) {
            const double framePts = static_cast<double>(frame->timestamp) / 1e6;
            EXPECT_LE(framePts, playable->getCurrentTime() + 1e-3) << "update " << i;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    sm.updateAll(0.0);
    // The clock is the wall clock since play(), whatever deltas updateAll was given
    // (sampled at one instant, so a loaded machine cannot skew the comparison).
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - started).count();
    EXPECT_NEAR(playable->getClock().position(now), elapsed, 0.02);
    EXPECT_LE(playable->getCurrentTime(), elapsed);  // the last update's position
    ASSERT_NE(playable->getCurrentFrame(), nullptr);
    EXPECT_GT(playable->getCurrentFrame()->timestamp, 500000u);  // it advanced
    EXPECT_TRUE(playable->getErrorMessage().empty());
}

// ADR-050 §3: sources locked to the SourceManager's master timeline follow it through
// updateAll(), and the Reset Playheads action restarts it.
TEST(VideoSourcePipeline, LockedSourceFollowsTheMasterTimeline) {
    namespace fs = std::filesystem;
    const fs::path video = "D:/testing Videos/greenCrystals_15Sec_4x3.mp4";
    if (!fs::exists(video)) GTEST_SKIP() << "test video not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    SourceManager sm;
    const UUID id = sm.getOrCreateVideoSource(video);
    auto* playable = sm.getSourceAs<PlayableSource>(id);
    ASSERT_NE(playable, nullptr);

    sm.pauseMasterTimeline();
    sm.restartMasterTimeline();
    sm.setTimelineLocked(id, true);
    EXPECT_TRUE(playable->isTimelineLocked());
    EXPECT_EQ(playable->getPlaybackState(), PlaybackState::PLAYING);
    sm.updateAll(0.0);
    EXPECT_NEAR(playable->getCurrentTime(), 0.0, 1e-9);  // master paused at 0

    sm.playMasterTimeline();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sm.updateAll(0.0);
    auto now = std::chrono::steady_clock::now();
    const double masterTime = sm.getMasterTimeline().position(now);
    EXPECT_GT(masterTime, 0.25);
    EXPECT_DOUBLE_EQ(playable->getClock().position(now), masterTime);  // master time on the clip

    playable->seek(10.0);  // ignored while locked
    sm.updateAll(0.0);
    now = std::chrono::steady_clock::now();
    EXPECT_DOUBLE_EQ(playable->getClock().position(now), sm.getMasterTimeline().position(now));

    sm.seekAllToStart();  // restarts the master too
    sm.updateAll(0.0);
    now = std::chrono::steady_clock::now();
    EXPECT_LT(sm.getMasterTimeline().position(now), masterTime);
    EXPECT_DOUBLE_EQ(playable->getClock().position(now), sm.getMasterTimeline().position(now));

    sm.setTimelineLocked(id, false);
    EXPECT_FALSE(playable->isTimelineLocked());
    playable->seek(10.0);  // its own transport works again
    now = std::chrono::steady_clock::now();
    const double position = playable->getClock().position(now);
    EXPECT_GE(position, 10.0);
    EXPECT_LT(position, 10.5);
}

// A video restored paused must show its first frame, not a black quad.
TEST(VideoSourcePipeline, PausedSourceShowsFirstFrame) {
    namespace fs = std::filesystem;
    const fs::path video = "D:/testing Videos/greenCrystals_15Sec_4x3.mp4";
    if (!fs::exists(video)) GTEST_SKIP() << "test video not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    SourceManager sm;
    const UUID id = sm.getOrCreateVideoSource(video);
    auto* playable = sm.getSourceAs<PlayableSource>(id);
    ASSERT_NE(playable, nullptr);

    // The GPU texture starts as an empty RGBA placeholder; the first video frame arrives
    // as YUV planes and the GPU pass converts it into that RGBA texture (ADR-055).
    bool uploaded = false;
    for (int i = 0; i < 100 && !uploaded; ++i) {
        sm.updateAll(1.0 / 60.0);
        const FrameData* frame = playable->getCurrentFrame();
        uploaded = frame && frame->isYuv();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(uploaded);
    Texture* tex = sm.getTexture(id);
    ASSERT_TRUE(tex && tex->isValid());
    EXPECT_EQ(tex->getChannels(), 4);
    EXPECT_EQ(tex->getWidth(), playable->getWidth());
    // Not the black placeholder: the frame's picture is in the texture.
    std::vector<uint8_t> rgba(static_cast<size_t>(tex->getWidth()) * tex->getHeight() * 4);
    glBindTexture(GL_TEXTURE_2D, tex->getId());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    uint64_t sum = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
    EXPECT_GT(sum, 0u);
    EXPECT_NE(playable->getPlaybackState(), PlaybackState::PLAYING);
    EXPECT_DOUBLE_EQ(playable->getCurrentTime(), 0.0);  // the clock did not move
}

// dedupe-by-path: two image binds of the same file share one source (the basis
// for one decode feeding many quads). GL-free, so always runs.
TEST(VideoSourcePipeline, ImageSourceDedupedByPath) {
    namespace fs = std::filesystem;
    const fs::path img = "D:/testing Videos/knowledge-graph.png";
    if (!fs::exists(img)) GTEST_SKIP() << "test image not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    SourceManager sm;
    UUID a = sm.getOrCreateImageSource(img);
    UUID b = sm.getOrCreateImageSource(img);
    ASSERT_FALSE(a.isNull());
    EXPECT_EQ(a, b);                       // same path -> same source, one decode
    EXPECT_EQ(sm.getSourceCount(), 1u);
}

// P5 relies on video dedupe so a saved box-wrap (same video on N quads) reloads
// as one shared decode.
TEST(VideoSourcePipeline, VideoSourceDedupedByPath) {
    namespace fs = std::filesystem;
    const fs::path video = "D:/testing Videos/greenCrystals_15Sec_4x3.mp4";
    if (!fs::exists(video)) GTEST_SKIP() << "test video not present";
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    SourceManager sm;
    UUID a = sm.getOrCreateVideoSource(video);
    UUID b = sm.getOrCreateVideoSource(video);
    ASSERT_FALSE(a.isNull());
    EXPECT_EQ(a, b);
    EXPECT_EQ(sm.getSourceCount(), 1u);
}
