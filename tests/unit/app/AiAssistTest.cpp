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
#include <filesystem>
#include <fstream>
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
                            AiAction::CapturePhoto, AiAction::SolveApply, AiAction::Anamorphic,
                            AiAction::TakePhoto, AiAction::ApplyProposal, AiAction::ClearProposal}) {
        es.statusMessage[0] = '\0';
        ai.run(action, nullptr);
        EXPECT_STREQ(es.statusMessage,
                     "AI assist is blocked: the show is locked (Output > Perform Lock)");
    }
    es.statusMessage[0] = '\0';
    ai.segmentAt(0.5f, 0.5f);
    EXPECT_STREQ(es.statusMessage, "AI assist is blocked: the show is locked (Output > Perform Lock)");
    EXPECT_FALSE(ai.isBusy());
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

TEST(AiAssistTest, AnAppliedWarpIsOneUndoStep) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    const auto before = quad->getGeometry().getCorners();
    AiAssist ai(es, nullptr);

    HeldWork work;
    ASSERT_TRUE(ai.startRequest("AI: detecting surface...", "AI", quad->getId(),
                                work.returning({"AI: surface applied", 5.0f, kWarp})));
    work.release();
    ASSERT_TRUE(tickUntilIdle(ai));
    ASSERT_NE(quad->getGeometry().getCorners(), before);

    ASSERT_TRUE(es.commandHistory.undo());
    EXPECT_EQ(quad->getGeometry().getCorners(), before);
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

// ---- Pick Surface (ADR-053 §2) --------------------------------------------------------

// Hands `outcome` back through the async path, as a finished request would.
void land(AiAssist& ai, const AiAssist::Outcome& outcome) {
    ASSERT_TRUE(ai.startRequest("working...", "Pick Surface", UUID::null(),
                                [outcome] { return outcome; }));
    ASSERT_TRUE(tickUntilIdle(ai));
}

AiSurfaceProposal proposalWith(float confidence, bool stub = false) {
    AiSurfaceProposal p;
    p.corners = kWarp;
    p.outline = {{0.1f, 0.2f}, {0.9f, 0.1f}, {0.8f, 0.9f}};
    p.confidence = confidence;
    p.stub = stub;
    return p;
}

void landProposal(AiAssist& ai, const AiSurfaceProposal& proposal) {
    AiAssist::Outcome outcome{"Pick Surface: surface found", 6.0f, std::nullopt};
    outcome.proposal = proposal;
    land(ai, outcome);
}

void expectWarped(const Quad& quad) {
    const auto& c = quad.getGeometry().getCorners();
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(c[i].x, kWarp[i][0]);
        EXPECT_FLOAT_EQ(c[i].y, kWarp[i][1]);
    }
}

TEST(AiAssistPickSurfaceTest, TheVenvInterpreterIsPreferredWhenItExists) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("qm_locate_" + UUID::generate().toString());
    const fs::path sidecarDir = root / "tools" / "ai-sidecar";
    fs::create_directories(sidecarDir);
    std::ofstream(sidecarDir / "sidecar.py") << "# test\n";

    ai::SidecarConfig config = AiAssist::locateSidecarFrom(root);
    EXPECT_FALSE(config.scriptPath.empty());
    EXPECT_EQ(config.pythonExe, "python");

    fs::create_directories(sidecarDir / ".venv" / "Scripts");
    std::ofstream(sidecarDir / ".venv" / "Scripts" / "python.exe") << "";
    config = AiAssist::locateSidecarFrom(root);
    EXPECT_EQ(fs::path(config.pythonExe).filename(), "python.exe");
    EXPECT_EQ(fs::path(config.pythonExe).parent_path().parent_path().filename(), ".venv");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(AiAssistPickSurfaceTest, TheMappingIsOneFlashOnlyWithAOneFlashPhoto) {
    EXPECT_EQ(AiAssist::pickMapping(""), AiAssist::PickMapping::CameraAtProjector);
    EXPECT_EQ(AiAssist::pickMapping("C:/tmp/quadmapper_oneflash_capture.png"),
              AiAssist::PickMapping::OneFlash);
}

TEST(AiAssistPickSurfaceTest, ASegmentReplyBecomesAProposalAndAStatus) {
    ai::SegmentReply missing;
    missing.endpointMissing = true;
    AiAssist::Outcome o = AiAssist::segmentOutcome(missing);
    EXPECT_EQ(o.status, "Pick Surface: update the AI sidecar (it cannot segment)");
    EXPECT_FALSE(o.proposal);

    o = AiAssist::segmentOutcome(ai::SegmentReply{});
    EXPECT_EQ(o.status, "Pick Surface: segmentation failed (sidecar error)");
    EXPECT_FALSE(o.proposal);

    ai::SegmentResult result;
    result.corners = kWarp;
    result.outline = {{0.1f, 0.2f}, {0.9f, 0.1f}, {0.8f, 0.9f}};
    result.confidence = 0.8f;
    result.planarity = 0.9f;
    o = AiAssist::segmentOutcome(ai::SegmentReply{result, false});
    ASSERT_TRUE(o.proposal);
    EXPECT_TRUE(o.proposal->canApply());
    EXPECT_EQ(o.proposal->outline.size(), 3u);
    EXPECT_EQ(o.proposal->planarity, 0.9f);
    EXPECT_FALSE(o.corners) << "a proposal is only shown, never applied on arrival";
    EXPECT_EQ(o.status,
              "Pick Surface: surface found (confidence 0.80) - Apply to warp the selected quad");

    result.confidence = 0.2f;
    o = AiAssist::segmentOutcome(ai::SegmentReply{result, false});
    ASSERT_TRUE(o.proposal);
    EXPECT_FALSE(o.proposal->canApply());
    EXPECT_EQ(o.status, "Pick Surface: low confidence (0.20) - click elsewhere on the surface");

    result.confidence = 0.9f;
    result.stub = true;
    o = AiAssist::segmentOutcome(ai::SegmentReply{result, false});
    ASSERT_TRUE(o.proposal);
    EXPECT_FALSE(o.proposal->canApply());
    EXPECT_EQ(o.status, "Pick Surface: placeholder only - the sidecar has no SAM 2 model");
}

TEST(AiAssistPickSurfaceTest, AProposalIsShownAndANewPhotoClearsIt) {
    EditorState es;
    AiAssist ai(es, nullptr);
    landProposal(ai, proposalWith(0.8f));
    ASSERT_TRUE(ai.photo().proposal);
    EXPECT_STREQ(es.statusMessage, "Pick Surface: surface found");

    AiAssist::Outcome photo{"AI Photo: photo taken - click the surface to map", 5.0f, std::nullopt};
    photo.photo = "C:/tmp/quadmapper_ai_photo.png";
    land(ai, photo);
    EXPECT_EQ(ai.photo().path, "C:/tmp/quadmapper_ai_photo.png");
    EXPECT_EQ(ai.photo().serial, 1);
    EXPECT_FALSE(ai.photo().proposal);

    landProposal(ai, proposalWith(0.8f));
    ai.run(EditorServices::AiAction::ClearProposal, nullptr);
    EXPECT_FALSE(ai.photo().proposal);
    EXPECT_STREQ(es.statusMessage, "Pick Surface: proposal cleared");
}

TEST(AiAssistPickSurfaceTest, AProposalThatLandsAfterTheLockEngagedIsDropped) {
    EditorState es;
    AiAssist ai(es, nullptr);
    HeldWork work;
    AiAssist::Outcome outcome{"Pick Surface: surface found", 6.0f, std::nullopt};
    outcome.proposal = proposalWith(0.8f);
    ASSERT_TRUE(ai.startRequest("Pick Surface: finding the surface...", "Pick Surface", UUID::null(),
                                work.returning(outcome)));
    lockPerform(es);
    work.release();
    ASSERT_TRUE(tickUntilIdle(ai));
    EXPECT_FALSE(ai.photo().proposal);
    EXPECT_STREQ(es.statusMessage,
                 "Pick Surface: result dropped - the show is locked (Output > Perform Lock)");
}

TEST(AiAssistPickSurfaceTest, ApplyWithoutAOneFlashPhotoUsesThePhotoCornersOnTheQuadPickedAtTheClick) {
    EditorState es;
    Quad* a = es.quadManager().createQuad("A");
    Quad* b = es.quadManager().createQuad("B");
    const auto bBefore = b->getGeometry().getCorners();
    AiAssist ai(es, nullptr);
    landProposal(ai, proposalWith(0.8f));

    es.quadManager().selectQuad(a->getId());
    ai.run(EditorServices::AiAction::ApplyProposal, nullptr);
    es.quadManager().selectQuad(b->getId());  // the selection moves on before the result lands
    ASSERT_TRUE(tickUntilIdle(ai));

    expectWarped(*a);
    EXPECT_EQ(b->getGeometry().getCorners(), bBefore);
    EXPECT_STREQ(es.statusMessage,
                 "Pick Surface: no One-Flash photo - assuming the camera is at the projector");
}

TEST(AiAssistPickSurfaceTest, AnApplyThatLandsUnderTheLockOrOnADeletedQuadChangesNothing) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    es.quadManager().selectQuad(quad->getId());
    const auto before = quad->getGeometry().getCorners();
    AiAssist ai(es, nullptr);
    landProposal(ai, proposalWith(0.8f));

    // The outcome is applied on tick(): the lock taken in between drops it.
    ai.applyProposal();
    lockPerform(es);
    ASSERT_TRUE(tickUntilIdle(ai));
    EXPECT_EQ(quad->getGeometry().getCorners(), before);
    EXPECT_STREQ(es.statusMessage,
                 "Pick Surface: result dropped - the show is locked (Output > Perform Lock)");

    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));
    const UUID target = quad->getId();
    ai.applyProposal();
    es.quadManager().deleteQuad(target);
    ASSERT_TRUE(tickUntilIdle(ai));
    EXPECT_EQ(es.quadManager().getQuad(target), nullptr);
    EXPECT_STREQ(es.statusMessage, "Pick Surface: result dropped - the quad was deleted");
}

TEST(AiAssistPickSurfaceTest, ApplyRefusesNoProposalAnUnsureOneAndNoQuad) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    const auto before = quad->getGeometry().getCorners();
    AiAssist ai(es, nullptr);

    ai.applyProposal();
    EXPECT_STREQ(es.statusMessage, "Pick Surface: click the surface in the photo first");

    landProposal(ai, proposalWith(0.9f, /*stub=*/true));
    ai.applyProposal();
    EXPECT_STREQ(es.statusMessage, "Pick Surface: the proposal is not confident enough to apply");

    landProposal(ai, proposalWith(0.2f));
    ai.applyProposal();
    EXPECT_STREQ(es.statusMessage, "Pick Surface: the proposal is not confident enough to apply");

    landProposal(ai, proposalWith(0.8f));
    es.quadManager().deselectAll();
    ai.applyProposal();
    EXPECT_STREQ(es.statusMessage, "Pick Surface: select a quad to warp first");
    EXPECT_FALSE(ai.isBusy());
    EXPECT_EQ(quad->getGeometry().getCorners(), before);
}

// With a One-Flash photo from this session, Apply goes through /oneflash: without a
// connected sidecar it asks for one rather than falling back to photo space.
TEST(AiAssistPickSurfaceTest, ApplyWithAOneFlashPhotoMapsThroughTheSidecar) {
    EditorState es;
    Quad* quad = es.quadManager().createQuad("A");
    es.quadManager().selectQuad(quad->getId());
    const auto before = quad->getGeometry().getCorners();
    AiAssist ai(es, stoppedSidecar());

    AiAssist::Outcome captured{"One-Flash: captured", 6.0f, std::nullopt};
    captured.oneFlashPhoto = "C:/tmp/quadmapper_oneflash_capture.png";
    land(ai, captured);
    landProposal(ai, proposalWith(0.8f));

    ai.applyProposal();
    EXPECT_STREQ(es.statusMessage, "Pick Surface: enable the sidecar first (AI > Enable AI Assist)");
    EXPECT_FALSE(ai.isBusy());
    EXPECT_EQ(quad->getGeometry().getCorners(), before);
}

TEST(AiAssistPickSurfaceTest, SegmentingNeedsASidecarThatCanSegment) {
    EditorState es;
    AiAssist ai(es, stoppedSidecar());
    EXPECT_FALSE(ai.canSegment());
    ai.segmentAt(0.5f, 0.5f);
    EXPECT_STREQ(es.statusMessage, "Pick Surface: enable the sidecar first (AI > Enable AI Assist)");
    EXPECT_FALSE(ai.isBusy());

    AiAssist none(es, nullptr);
    EXPECT_FALSE(none.canSegment());
}

}  // namespace
}  // namespace quadmapper
