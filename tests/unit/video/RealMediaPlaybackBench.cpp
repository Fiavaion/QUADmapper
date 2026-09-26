// Opt-in measurement of real playback (ADR-050): plays files from the local test-media
// folder on the real clock through a simulated 60 Hz render loop and reports how
// continuously frames were presented and what the decode cost in CPU time, forward
// (Loop) against Ping-Pong. Disabled by default (it runs for minutes and needs the
// media); run with
//   test_video_decoder --gtest_also_run_disabled_tests --gtest_filter=*RealMedia*
// Files that are missing are skipped. Media is never committed.

#include <gtest/gtest.h>

#include "video/VideoPlayer.hpp"
#include "video/PlaybackClock.hpp"
#include "source/HapFileSource.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr double RUN_SECONDS = 12.0;
constexpr double TICK = 1.0 / 60.0;

double processCpuSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    auto seconds = [](const FILETIME& t) {
        ULARGE_INTEGER v;
        v.LowPart = t.dwLowDateTime;
        v.HighPart = t.dwHighDateTime;
        return static_cast<double>(v.QuadPart) * 1e-7;
    };
    return seconds(kernel) + seconds(user);
#else
    return 0.0;
#endif
}

struct Report {
    int presented = 0;
    int expected = 0;
    double longestGapMs = 0.0;   // wall time between two presented frames
    int reverseOrderBreaks = 0;  // reverse-leg frames not strictly below the previous one
                                 // (a late forward frame shown after the reflection is not one)
    double cpuPercentOfCore = 0.0;
    size_t cachePeakMb = 0;
    std::string notice;
    std::string gaps;            // where the long gaps happened (clip time, direction)
};

// Drive `update` (returns the pts of a newly presented frame, or NaN) for RUN_SECONDS.
template <typename Update, typename Direction>
Report drive(double fps, Update&& update, Direction&& direction) {
    Report r;
    r.expected = static_cast<int>(RUN_SECONDS * fps);
    const double cpuBefore = processCpuSeconds();
    const auto start = std::chrono::steady_clock::now();
    auto next = start;
    auto lastPresent = start;
    double lastPts = -1.0;
    int lastDirection = 1;
    while (std::chrono::steady_clock::now() - start < std::chrono::duration<double>(RUN_SECONDS)) {
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(TICK));
        std::this_thread::sleep_until(next);
        const double pts = update();
        if (std::isnan(pts)) continue;
        const auto now = std::chrono::steady_clock::now();
        const int dir = direction();
        if (r.presented > 0) {
            const double gap = std::chrono::duration<double, std::milli>(now - lastPresent).count();
            r.longestGapMs = std::max(r.longestGapMs, gap);
            if (gap > 100.0) {
                char at[64];
                std::snprintf(at, sizeof(at), " %.0fms@%.2fs%s", gap, pts, dir < 0 ? "r" : "");
                r.gaps += at;
            }
        }
        if (dir < 0 && lastDirection < 0 && pts >= lastPts - 1e-9) ++r.reverseOrderBreaks;
        lastDirection = dir;
        lastPts = pts;
        lastPresent = now;
        ++r.presented;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    r.cpuPercentOfCore = 100.0 * (processCpuSeconds() - cpuBefore) / wall;
    return r;
}

void print(const std::string& file, const char* mode, const Report& r) {
    std::printf("[ %-40s %-9s ] presented %4d/%4d  longest gap %6.1f ms  reverse order breaks %d  "
                "CPU %5.1f%% of a core  reverse cache peak %zu MB%s%s%s%s\n",
                file.c_str(), mode, r.presented, r.expected, r.longestGapMs, r.reverseOrderBreaks,
                r.cpuPercentOfCore, r.cachePeakMb, r.gaps.empty() ? "" : "  gaps >100ms:",
                r.gaps.c_str(), r.notice.empty() ? "" : "  NOTICE: ", r.notice.c_str());
    std::fflush(stdout);
}

Report playVideo(const fs::path& path, LoopMode mode) {
    VideoPlayer player;
    EXPECT_TRUE(player.load(path));
    player.setLoopMode(mode);
    // Start 3 s before the end so a run spans the loop point or the reflection.
    player.seek(std::max(0.0, player.getState().duration - 3.0));
    player.play();
    Report r = drive(
        player.getVideoInfo().frameRate,
        [&] {
            player.update(TICK);
            if (!player.hasNewFrame()) return std::nan("");
            player.consumeFrame();
            return player.getCurrentFrame().pts;
        },
        // The leg the frame was scheduled for, not the clock's current direction.
        [&] {
            const PlaybackClock& clock = player.getClock();
            return legAt(player.getCurrentFrame().due + 1e-9, clock.duration(), clock.loopMode()).direction;
        });
    r.cachePeakMb = player.getReverseCachePeakBytes() >> 20;
    r.notice = player.getLoopModeNotice();
    EXPECT_TRUE(player.getErrorMessage().empty()) << player.getErrorMessage();
    return r;
}

Report playHap(const fs::path& path, LoopMode mode, double fps) {
    HapFileSource source(path);
    EXPECT_TRUE(source.initialize());
    source.setLoopMode(mode);
    source.seek(std::max(0.0, source.getDuration() - 3.0));
    source.play();
    Report r = drive(
        fps,
        [&] {
            if (!source.update(TICK)) return std::nan("");
            source.markFrameConsumed();
            return static_cast<double>(source.getCurrentFrame()->timestamp) / 1e6;
        },
        [&] { return source.getClock().direction(source.getClockNow()); });
    r.notice = source.getLoopModeNotice();
    EXPECT_TRUE(source.getErrorMessage().empty()) << source.getErrorMessage();
    return r;
}

const fs::path MEDIA = "D:/testing Videos";

}  // namespace

TEST(RealMediaPlayback, DISABLED_FfmpegForwardAgainstPingPong) {
    for (const char* name : {"5sec.mp4", "greenCrystals_15Sec.mp4", "keying woman.mp4",
                             "converted/greenCrystals_15Sec_4x3__submission_h264.mp4"}) {
        const fs::path path = MEDIA / name;
        if (!fs::exists(path)) continue;
        print(name, "Loop", playVideo(path, LoopMode::LOOP));
        const Report pingPong = playVideo(path, LoopMode::PING_PONG);
        print(name, "Ping-Pong", pingPong);
        EXPECT_EQ(pingPong.reverseOrderBreaks, 0) << name;
    }
    // A long-GOP encode (one group of pictures bigger than the reverse cache) must fall
    // back to Loop with a reason; make one with e.g.
    //   ffmpeg -i greenCrystals_15Sec.mp4 -c:v libx264 -g 400 -sc_threshold 0 %TEMP%\qm_bench_longgop.mp4
    const fs::path longGop = fs::temp_directory_path() / "qm_bench_longgop.mp4";
    if (fs::exists(longGop)) {
        const Report r = playVideo(longGop, LoopMode::PING_PONG);
        print(longGop.filename().string(), "Ping-Pong", r);
        EXPECT_FALSE(r.notice.empty());
    }
}

// Raw decode + RGB conversion speed, one thread: the ceiling for any playback mode.
TEST(RealMediaPlayback, DISABLED_DecodeThroughput) {
    for (const char* name : {"5sec.mp4", "greenCrystals_15Sec.mp4", "keying woman.mp4"}) {
        const fs::path path = MEDIA / name;
        if (!fs::exists(path)) continue;
        VideoDecoder decoder;
        ASSERT_TRUE(decoder.open(path));
        const auto start = std::chrono::steady_clock::now();
        int frames = 0;
        while (frames < 120 && decoder.decodeNextFrame().isValid()) ++frames;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("[ %-40s ] %d frames decoded in %.2f s = %.0f fps (content %.2f fps)\n", name, frames,
                    seconds, frames / seconds, decoder.getInfo().frameRate);
        std::fflush(stdout);
    }
}

TEST(RealMediaPlayback, DISABLED_HapForwardAgainstPingPong) {
    for (const auto& [name, fps] : {std::pair<const char*, double>{"Hap/5sec_hap.mov", 30.0},
                                    std::pair<const char*, double>{"keying woman_hap.mov", 30000.0 / 1001.0}}) {
        const fs::path path = MEDIA / name;
        if (!fs::exists(path)) continue;
        print(name, "Loop", playHap(path, LoopMode::LOOP, fps));
        const Report pingPong = playHap(path, LoopMode::PING_PONG, fps);
        print(name, "Ping-Pong", pingPong);
        EXPECT_EQ(pingPong.reverseOrderBreaks, 0) << name;
    }
}
