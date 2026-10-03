// AI assist status paths (app/AiAssist.hpp, ADR-051 E1) on a real EditorState: with no
// sidecar, or one that was never started, every action reports why it did nothing and
// leaves the quads alone. No GL context, camera or Python needed.

#include "app/AiAssist.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "ai/SidecarClient.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace quadmapper {
namespace {

std::unique_ptr<ai::SidecarClient> stoppedSidecar() {
    return std::make_unique<ai::SidecarClient>(ai::SidecarConfig{});
}

TEST(AiAssistTest, NoSidecarIsInactiveAndEnableDoesNothing) {
    EditorState es;
    AiAssist ai(es, nullptr);
    EXPECT_FALSE(ai.isActive());
    ai.enable();
    EXPECT_STREQ(es.statusMessage, "");
    EXPECT_EQ(es.statusTimer, 0.0f);
}

TEST(AiAssistTest, DetectSurfaceWithoutSidecarAsksToEnableIt) {
    EditorState es;
    AiAssist ai(es, nullptr);
    ai.detectSurface();
    EXPECT_STREQ(es.statusMessage, "AI: enable the sidecar first (AI > Enable AI Assist)");
    EXPECT_EQ(es.statusTimer, 5.0f);
}

TEST(AiAssistTest, StoppedSidecarIsInactiveAndDetectAsksToEnableIt) {
    EditorState es;
    AiAssist ai(es, stoppedSidecar());
    EXPECT_FALSE(ai.isActive());
    ai.detectSurface();
    EXPECT_STREQ(es.statusMessage, "AI: enable the sidecar first (AI > Enable AI Assist)");
}

TEST(AiAssistTest, OneFlashSolvesWithoutSidecarAskToEnableItAndLeaveTheQuad) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad();
    ASSERT_NE(quad, nullptr);
    es.quadManager().selectQuad(quad->getId());
    const auto before = quad->getGeometry().getCorners();

    AiAssist ai(es, stoppedSidecar());
    ai.solveAndApply(nullptr);
    EXPECT_STREQ(es.statusMessage,
                 "One-Flash: enable the sidecar first (AI > Enable AI Assist)");
    EXPECT_EQ(es.statusTimer, 6.0f);

    es.statusMessage[0] = '\0';
    ai.anamorphic(nullptr);
    EXPECT_STREQ(es.statusMessage,
                 "One-Flash: enable the sidecar first (AI > Enable AI Assist)");
    EXPECT_EQ(quad->getGeometry().getCorners(), before);
}

TEST(AiAssistTest, ClearPatternReportsIt) {
    EditorState es;
    AiAssist ai(es, nullptr);
    ai.clearPattern();
    EXPECT_STREQ(es.statusMessage, "One-Flash: pattern cleared");
    EXPECT_EQ(es.statusTimer, 4.0f);
}

// The perform lock (ADR-051 §4) refuses every AI menu item but Clear Pattern, which
// gives the show back.
TEST(AiAssistTest, ThePerformLockRefusesAllButClearPattern) {
    using AiAction = EditorServices::AiAction;
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    es.quadManager().selectQuad(quad->getId());
    const auto before = quad->getGeometry().getCorners();
    AiAssist ai(es, stoppedSidecar());
    lockPerform(es);
    for (AiAction action : {AiAction::Enable, AiAction::DetectSurface, AiAction::EmitPattern,
                            AiAction::CapturePhoto, AiAction::SolveApply, AiAction::Anamorphic}) {
        es.statusMessage[0] = '\0';
        ai.run(action, nullptr);
        EXPECT_STREQ(es.statusMessage,
                     "AI assist is blocked: the show is locked (Output > Perform Lock)");
    }
    EXPECT_EQ(quad->getGeometry().getCorners(), before);

    ai.run(AiAction::ClearPattern, nullptr);
    EXPECT_STREQ(es.statusMessage, "One-Flash: pattern cleared");
}

// ---- Requests run off the main thread; tick() applies their outcome ------------------
// The actions hand startRequest a worker that captures and asks the sidecar. These tests
// hand it a worker that waits for the test instead, so no camera or sidecar is needed.

using Corners = std::array<std::array<float, 2>, 4>;
constexpr Corners kWarp{{{0.1f, 0.2f}, {0.9f, 0.1f}, {0.8f, 0.9f}, {0.2f, 0.8f}}};

// A worker blocked until release(): what the camera and the HTTP call look like.
struct HeldWork {
    std::shared_ptr<std::promise<void>> gate = std::make_shared<std::promise<void>>();
    std::shared_future<void> opened = gate->get_future().share();

    std::function<AiAssist::Outcome()> returning(AiAssist::Outcome outcome) const {
        return [opened = opened, outcome] {
            opened.wait();
            return outcome;
        };
    }
    void release() { gate->set_value(); }
};

// Ticks until the request's outcome has been applied (the main thread's side).
bool tickUntilIdle(AiAssist& ai) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ai.isBusy() && std::chrono::steady_clock::now() < deadline) {
        ai.tick(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !ai.isBusy();
}

TEST(AiAssistTest, ARequestRunsOffTheMainThreadAndLandsOnTheQuadPickedAtTheClick) {
    EditorState es;
    Quad* a = es.quadManager().createQuad("A");
    Quad* b = es.quadManager().createQuad("B");
    es.quadManager().selectQuad(a->getId());
    const auto bBefore = b->getGeometry().getCorners();
    AiAssist ai(es, nullptr);

    HeldWork work;
    ASSERT_TRUE(ai.startRequest("One-Flash: solving...", "One-Flash", a->getId(),
                                work.returning({"One-Flash: warp applied", 6.0f, kWarp})));
    EXPECT_TRUE(ai.isBusy());
    EXPECT_STREQ(es.statusMessage, "One-Flash: solving...");

    // A second request waits; ticking while the worker runs changes nothing.
    EXPECT_FALSE(ai.startRequest("AI: detecting surface...", "AI", a->getId(),
                                 work.returning({"never", 5.0f, kWarp})));
    EXPECT_STREQ(es.statusMessage, "AI: wait for the running request to finish");
    ai.tick(nullptr);
    EXPECT_TRUE(ai.isBusy());

    es.quadManager().selectQuad(b->getId());  // the selection moves on meanwhile
    work.release();
    ASSERT_TRUE(tickUntilIdle(ai));

    const auto& c = a->getGeometry().getCorners();
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(c[i].x, kWarp[i][0]);
        EXPECT_FLOAT_EQ(c[i].y, kWarp[i][1]);
    }
    EXPECT_EQ(b->getGeometry().getCorners(), bBefore);
    EXPECT_STREQ(es.statusMessage, "One-Flash: warp applied");
    EXPECT_EQ(es.statusTimer, 6.0f);
}

TEST(AiAssistTest, AResultThatLandsAfterTheLockEngagedChangesNothing) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    es.quadManager().selectQuad(quad->getId());
    const auto before = quad->getGeometry().getCorners();
    AiAssist ai(es, nullptr);

    HeldWork work;
    ASSERT_TRUE(ai.startRequest("AI: detecting surface...", "AI", quad->getId(),
                                work.returning({"AI: surface applied", 5.0f, kWarp})));
    lockPerform(es);
    work.release();
    ASSERT_TRUE(tickUntilIdle(ai));

    EXPECT_EQ(quad->getGeometry().getCorners(), before);
    EXPECT_STREQ(es.statusMessage,
                 "AI: result dropped - the show is locked (Output > Perform Lock)");
}

TEST(AiAssistTest, AResultForADeletedQuadChangesNothing) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    Quad* other = es.quadManager().createQuad("B");
    const UUID target = quad->getId();
    const auto otherBefore = other->getGeometry().getCorners();
    AiAssist ai(es, nullptr);

    HeldWork work;
    ASSERT_TRUE(ai.startRequest("One-Flash: solving...", "One-Flash", target,
                                work.returning({"One-Flash: warp applied", 6.0f, kWarp})));
    es.quadManager().deleteQuad(target);
    work.release();
    ASSERT_TRUE(tickUntilIdle(ai));

    EXPECT_EQ(es.quadManager().getQuad(target), nullptr);
    EXPECT_EQ(other->getGeometry().getCorners(), otherBefore);
    EXPECT_STREQ(es.statusMessage, "One-Flash: result dropped - the quad was deleted");
}

// Shutdown never waits out a request stuck in the camera or a 30 s HTTP call.
TEST(AiAssistTest, ShutdownWithARequestStuckInFlightIsBounded) {
    EditorState es;
    HeldWork work;
    const auto start = std::chrono::steady_clock::now();
    {
        AiAssist ai(es, nullptr);
        ASSERT_TRUE(ai.startRequest("One-Flash: capturing photo...", "One-Flash", UUID::null(),
                                    work.returning({"One-Flash: captured", 6.0f, std::nullopt})));
        ai.stopSidecar();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
    work.release();  // the let-go worker finishes on its own shares
}

TEST(AiAssistTest, TickWithAnIdleSidecarPostsNothing) {
    EditorState es;
    AiAssist ai(es, stoppedSidecar());
    ai.tick(nullptr);
    ai.stopSidecar();
    ai.tick(nullptr);
    EXPECT_STREQ(es.statusMessage, "");
    EXPECT_FALSE(ai.isActive());
}

}  // namespace
}  // namespace quadmapper
