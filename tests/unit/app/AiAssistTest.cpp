// AI assist status paths (app/AiAssist.hpp, ADR-051 E1) on a real EditorState: with no
// sidecar, or one that was never started, every action reports why it did nothing and
// leaves the quads alone. No GL context, camera or Python needed.

#include "app/AiAssist.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "ai/SidecarClient.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

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
