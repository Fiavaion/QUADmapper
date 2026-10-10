// The Scan Output state machine (app/GrayCodeScan, ADR-053 §3) with a fake host and a
// fake camera: the frame steps and captures, the settle timing, what ends a scan, and
// that every ending gives back exactly what the scan changed. No GL, outputs or camera.

#include "app/GrayCodeScan.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper {
namespace {

using Clock = GrayCodeScan::Clock;
using Phase = GrayCodeScan::Phase;

struct FakeHost : ScanHost {
    bool isLocked = false;
    bool isBlackout = false;
    bool perform = false;
    bool performEntryWorks = true;
    int monitorAfterPerform = -1;  // >= 0: entering perform mode moves the output there
    int performEntries = 0;
    int performExits = 0;
    std::optional<ScanOutput> out = ScanOutput{IVec2(1920, 1080), 1};
    int missingFrame = -1;  // showFrame fails for this index
    std::vector<int> shown;
    int shownNow = -1;      // -1: the show, not a frame
    int hides = 0;

    bool locked() const override { return isLocked; }
    bool blackout() const override { return isBlackout; }
    bool inPerformMode() const override { return perform; }
    bool enterPerformMode() override {
        ++performEntries;
        perform = performEntryWorks;
        if (perform && monitorAfterPerform >= 0 && out) out->monitor = monitorAfterPerform;
        return perform;
    }
    void exitPerformMode() override {
        ++performExits;
        perform = false;
    }
    std::optional<ScanOutput> output(const UUID&) const override { return out; }
    bool showFrame(int index) override {
        if (index == missingFrame) return false;
        shown.push_back(index);
        shownNow = index;
        return true;
    }
    void hideFrames() override {
        ++hides;
        shownNow = -1;
    }
};

// What the test sees of the camera after the scan owns (and maybe drops) it.
struct CameraLog {
    ScanCamera::State state = ScanCamera::State::Ready;
    std::string error;
    std::vector<std::string> captures;
    bool closed = false;
};

struct FakeCamera : ScanCamera {
    std::shared_ptr<CameraLog> log;
    explicit FakeCamera(std::shared_ptr<CameraLog> l) : log(std::move(l)) {}
    ~FakeCamera() override { log->closed = true; }
    State state() const override { return log->state; }
    void capture(const std::string& path) override {
        log->captures.push_back(path);
        log->state = State::Capturing;
    }
    std::string error() const override { return log->error; }
};

class GrayCodeScanTest : public ::testing::Test {
protected:
    FakeHost host;
    std::shared_ptr<CameraLog> camera = std::make_shared<CameraLog>();
    const UUID outputId = UUID::generate();
    std::unique_ptr<GrayCodeScan> scan = std::make_unique<GrayCodeScan>(
        host, std::make_unique<FakeCamera>(camera), outputId, ScanOutput{IVec2(1920, 1080), 1},
        std::filesystem::path("scan"));
    Clock::time_point now = Clock::now();

    void tick(double seconds = 0.0) {
        now += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        scan->tick(now);
    }
    // Ticks (1/60 s apart) until the camera is asked for a frame, for at most `seconds`.
    bool tickUntilCapture(double seconds) {
        const size_t before = camera->captures.size();
        for (double t = 0.0; t <= seconds; t += 1.0 / 60.0) {
            tick(1.0 / 60.0);
            if (camera->captures.size() > before) return true;
        }
        return false;
    }
    void finishCapture() {
        camera->state = ScanCamera::State::Ready;
        tick(1.0 / 60.0);
    }
    // Starts a 4-frame scan and runs it to its first frame.
    void startShowing() {
        scan->patternsReady(4);
        tick();
        ASSERT_EQ(scan->phase(), Phase::Settling);
    }
};

TEST_F(GrayCodeScanTest, WaitsForTheFramesAndTheCameraBeforeShowingAnything) {
    camera->state = ScanCamera::State::Opening;
    tick();
    scan->patternsReady(4);
    tick();
    EXPECT_EQ(scan->phase(), Phase::Preparing);
    EXPECT_EQ(scan->progress(), "Scan: preparing...");
    EXPECT_TRUE(host.shown.empty());
    EXPECT_EQ(host.performEntries, 0);

    camera->state = ScanCamera::State::Ready;
    tick();
    EXPECT_EQ(scan->phase(), Phase::Settling);
    EXPECT_EQ(host.performEntries, 1);
    EXPECT_EQ(host.shown, std::vector<int>{0});
    EXPECT_EQ(scan->progress(), "Scan: 0/4 (Esc cancels)");
}

TEST_F(GrayCodeScanTest, ShowsSettlesAndCapturesEveryFrameThenRestores) {
    startShowing();
    for (int k = 0; k < 4; ++k) {
        ASSERT_TRUE(tickUntilCapture(2.0)) << "frame " << k;
        EXPECT_EQ(host.shownNow, k);
        EXPECT_EQ(std::filesystem::path(camera->captures.back()).filename().string(),
                  GrayCodeScan::captureName(k));
        EXPECT_EQ(scan->phase(), Phase::Capturing);
        finishCapture();
        EXPECT_EQ(scan->capturedFrames(), k + 1);
    }
    EXPECT_EQ(scan->phase(), Phase::Captured);
    EXPECT_TRUE(scan->endStatus().empty());
    EXPECT_EQ(host.shown, (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(host.shownNow, -1) << "the show is back on the outputs";
    EXPECT_EQ(host.performExits, 1) << "perform mode was the scan's, so it is left";
    EXPECT_TRUE(camera->closed);
}

TEST_F(GrayCodeScanTest, EachFrameSettlesForTheSettleTimeAndIsPresentedFirst) {
    startShowing();
    // The first frame waits out the switch to fullscreen as well.
    const double first = GrayCodeScan::DEFAULT_SETTLE_SECONDS + GrayCodeScan::FIRST_FRAME_EXTRA_SECONDS;
    tick(first - 0.05);
    tick(0.01);
    EXPECT_TRUE(camera->captures.empty());
    tick(0.05);
    ASSERT_EQ(camera->captures.size(), 1u);
    finishCapture();  // frame 1 goes up on this tick

    // Long enough, but shown for one tick only: it may not have been presented yet.
    tick(1.0);
    EXPECT_EQ(camera->captures.size(), 1u);
    tick(0.0);
    EXPECT_EQ(camera->captures.size(), 2u);
    finishCapture();

    // Enough ticks, but not yet the settle time.
    tick(0.1);
    tick(0.1);
    EXPECT_EQ(camera->captures.size(), 2u);
    tick(0.06);
    EXPECT_EQ(camera->captures.size(), 3u);
}

TEST_F(GrayCodeScanTest, ACancelRestoresTheShowAndLeavesPerformModeItEntered) {
    startShowing();
    ASSERT_TRUE(host.perform);
    scan->cancel();
    EXPECT_EQ(scan->phase(), Phase::Ended);
    EXPECT_EQ(scan->endStatus(), "Scan cancelled");
    EXPECT_EQ(host.shownNow, -1);
    EXPECT_EQ(host.performExits, 1);
    EXPECT_FALSE(host.perform);
    EXPECT_TRUE(camera->closed);

    scan->cancel();  // over: nothing more
    tick(1.0);
    EXPECT_EQ(host.performExits, 1);
    EXPECT_EQ(scan->endStatus(), "Scan cancelled");
}

TEST_F(GrayCodeScanTest, PerformModeThatWasOnBeforeTheScanStaysOn) {
    host.perform = true;
    startShowing();
    EXPECT_EQ(host.performEntries, 0);
    scan->cancel();
    EXPECT_EQ(host.performExits, 0);
    EXPECT_TRUE(host.perform);
}

TEST_F(GrayCodeScanTest, LeavingPerformModeCancels) {
    startShowing();
    host.perform = false;  // Esc or F11
    tick();
    EXPECT_EQ(scan->phase(), Phase::Ended);
    EXPECT_EQ(scan->endStatus(), "Scan cancelled");
    EXPECT_EQ(host.performExits, 0);
    EXPECT_EQ(host.performEntries, 1) << "never re-entered";
    EXPECT_EQ(host.shownNow, -1);
}

TEST_F(GrayCodeScanTest, TheLockBlackoutAndOutputChangesEndIt) {
    struct Case {
        const char* status;
        void (*apply)(FakeHost&);
    };
    const Case cases[] = {
        {"Scan cancelled: the show was locked", [](FakeHost& h) { h.isLocked = true; }},
        {"Scan cancelled: Blackout is on", [](FakeHost& h) { h.isBlackout = true; }},
        {"Scan cancelled: the output was removed", [](FakeHost& h) { h.out.reset(); }},
        {"Scan cancelled: the output's resolution or display changed",
         [](FakeHost& h) { h.out->resolution = IVec2(1280, 720); }},
        {"Scan cancelled: the output's resolution or display changed",
         [](FakeHost& h) { h.out->monitor = 2; }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.status);
        host = FakeHost{};
        camera = std::make_shared<CameraLog>();
        scan = std::make_unique<GrayCodeScan>(host, std::make_unique<FakeCamera>(camera), outputId,
                                              ScanOutput{IVec2(1920, 1080), 1}, "scan");
        startShowing();
        ASSERT_TRUE(tickUntilCapture(2.0));
        c.apply(host);
        tick();
        EXPECT_EQ(scan->phase(), Phase::Ended);
        EXPECT_EQ(scan->endStatus(), c.status);
        EXPECT_EQ(host.shownNow, -1);
        EXPECT_FALSE(host.perform);
        EXPECT_TRUE(camera->closed);
    }
}

TEST_F(GrayCodeScanTest, TheLockEndsItBeforeAnythingIsShown) {
    scan->patternsReady(4);
    host.isLocked = true;
    tick();
    EXPECT_EQ(scan->endStatus(), "Scan cancelled: the show was locked");
    EXPECT_EQ(host.performEntries, 0);
    EXPECT_TRUE(host.shown.empty());
    EXPECT_EQ(host.hides, 0) << "nothing was shown, so nothing is hidden";
}

TEST_F(GrayCodeScanTest, CameraAndFrameFailuresEndItWithTheReason) {
    startShowing();
    ASSERT_TRUE(tickUntilCapture(2.0));
    camera->state = ScanCamera::State::Failed;
    camera->error = "the camera capture failed";
    tick();
    EXPECT_EQ(scan->endStatus(), "Scan: the camera capture failed");
    EXPECT_EQ(host.shownNow, -1);
    EXPECT_FALSE(host.perform);

    host = FakeHost{};
    host.missingFrame = 1;
    camera = std::make_shared<CameraLog>();
    scan = std::make_unique<GrayCodeScan>(host, std::make_unique<FakeCamera>(camera), outputId,
                                          ScanOutput{IVec2(1920, 1080), 1}, "scan");
    startShowing();
    ASSERT_TRUE(tickUntilCapture(2.0));
    finishCapture();
    EXPECT_EQ(scan->endStatus(), "Scan: could not load pattern_001.png");
    EXPECT_FALSE(host.perform);
}

TEST_F(GrayCodeScanTest, NoFramesOrNoFullscreenEndsIt) {
    scan->patternsReady(0);
    EXPECT_EQ(scan->endStatus(), "Scan: the AI sidecar wrote no patterns");

    host = FakeHost{};
    host.performEntryWorks = false;
    camera = std::make_shared<CameraLog>();
    scan = std::make_unique<GrayCodeScan>(host, std::make_unique<FakeCamera>(camera), outputId,
                                          ScanOutput{IVec2(1920, 1080), 1}, "scan");
    scan->patternsReady(4);
    tick();
    EXPECT_EQ(scan->endStatus(), "Scan: the output could not go fullscreen (is its display connected?)");
    EXPECT_TRUE(host.shown.empty());
}

TEST_F(GrayCodeScanTest, TheDisplayGoingFullscreenSettlesOnIsTheScans) {
    host.monitorAfterPerform = 2;  // perform mode resolved the output to another display
    startShowing();
    EXPECT_EQ(scan->output(), (ScanOutput{IVec2(1920, 1080), 2}));
    ASSERT_TRUE(tickUntilCapture(2.0)) << "not taken for a change";
}

}  // namespace
}  // namespace quadmapper
