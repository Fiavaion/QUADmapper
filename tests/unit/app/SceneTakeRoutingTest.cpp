// Scene takes on the live routing (ADR-047 §5): EditorState::takeScene against real
// outputs. No GL context needed.
#include "app/EditorState.hpp"
#include <gtest/gtest.h>

using namespace quadmapper;

namespace {

struct BlendedRig {
    EditorState es;
    OutputManager om;
    OutputWindow* left = nullptr;
    OutputWindow* right = nullptr;
    OutputWindow* side = nullptr;
    UUID wide, other, next;

    BlendedRig() {
        left = om.createOutput("Left");
        right = om.createOutput("Right");
        side = om.createOutput("Side");
        Scene& first = es.activeScene();
        first.setCanvasSize(IVec2(3200, 1080));
        wide = first.getId();
        other = es.addScene("Other")->getId();
        next = es.addScene("Next")->getId();
        es.scenes[2]->setCanvasSize(IVec2(3200, 1080));
        es.setActiveScene(wide);

        // Left and right blend over one wide canvas; the side projector shows its own scene.
        OutputManager::showScene(*left, es.scenes[0].get(), es.scenes);
        OutputManager::showScene(*right, es.scenes[0].get(), es.scenes);
        OutputManager::showScene(*side, es.scenes[1].get(), es.scenes);
        left->getSlices()[0].input = CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f};
        left->getSlices()[0].blend[SLICE_EDGE_RIGHT].width = 640.0f;
        right->getSlices()[0].input = CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f};
        right->getSlices()[0].blend[SLICE_EDGE_LEFT].width = 640.0f;
    }
};

}  // namespace

TEST(SceneTakeRoutingTest, TakeChangesTheBlendedGroupTogether) {
    BlendedRig rig;
    const SceneTake take = rig.es.takeScene(3, rig.om);

    EXPECT_EQ(take.kind, SceneTake::Kind::Route);
    EXPECT_EQ(take.outputIds, (std::vector<UUID>{rig.left->getId(), rig.right->getId()}));
    EXPECT_EQ(rig.left->getSceneId(), rig.next);
    EXPECT_EQ(rig.right->getSceneId(), rig.next);
    EXPECT_EQ(rig.side->getSceneId(), rig.other);  // not in the group: untouched
    EXPECT_EQ(rig.es.activeSceneId, rig.next);
    // The rig's slices and blends carry over (same canvas size, so unchanged).
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width, 640.0f);

    // Back again: the whole group.
    rig.es.takeScene(1, rig.om);
    EXPECT_EQ(rig.left->getSceneId(), rig.wide);
    EXPECT_EQ(rig.right->getSceneId(), rig.wide);
    EXPECT_EQ(rig.side->getSceneId(), rig.other);
}

TEST(SceneTakeRoutingTest, TakingASceneAnotherOutputShowsOnlyActivatesIt) {
    BlendedRig rig;
    const SceneTake take = rig.es.takeScene(2, rig.om);
    EXPECT_EQ(take.kind, SceneTake::Kind::Activate);
    EXPECT_EQ(rig.es.activeSceneId, rig.other);
    EXPECT_EQ(rig.left->getSceneId(), rig.wide);
    EXPECT_EQ(rig.right->getSceneId(), rig.wide);
}

TEST(SceneTakeRoutingTest, CalibrationActivatesTheSceneTheOutputShows) {
    BlendedRig rig;
    rig.es.beginCalibration(*rig.side);
    EXPECT_EQ(rig.es.activeSceneId, rig.other);
    EXPECT_EQ(rig.es.calibratingOutputId, rig.side->getId());
}

// The active scene is on no output (just added, or clicked in the Scenes panel): the
// take still moves the whole blended group, not only its first output.
TEST(SceneTakeRoutingTest, TakeFromAnUnshownActiveSceneMovesTheWholeGroup) {
    BlendedRig rig;
    rig.es.addScene("Being edited");  // active now, on no output
    const SceneTake take = rig.es.takeScene(3, rig.om);

    EXPECT_EQ(take.kind, SceneTake::Kind::Route);
    EXPECT_EQ(take.outputIds, (std::vector<UUID>{rig.left->getId(), rig.right->getId()}));
    EXPECT_EQ(rig.left->getSceneId(), rig.next);
    EXPECT_EQ(rig.right->getSceneId(), rig.next);
    EXPECT_EQ(rig.side->getSceneId(), rig.other);
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
}

// Deleting a scene takes it off every output showing it; the next scene routed there
// gets the slices rescaled onto its own canvas.
TEST(SceneTakeRoutingTest, DeletingAShownSceneTakesItOffItsOutputs) {
    BlendedRig rig;
    ASSERT_TRUE(rig.es.deleteScene(rig.wide, &rig.om));
    EXPECT_TRUE(rig.left->getSceneId().isNull());
    EXPECT_TRUE(rig.right->getSceneId().isNull());
    EXPECT_EQ(rig.side->getSceneId(), rig.other);
    ASSERT_EQ(rig.right->getSlices().size(), 1u);  // kept for the next scene

    Scene* half = rig.es.addScene("Half");
    half->setCanvasSize(IVec2(1600, 540));
    OutputManager::showScene(*rig.right, half, rig.es.scenes);
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{640.0f, 0.0f, 960.0f, 540.0f}));
}

// A take onto a lone output through its default slice gives the scene that output's
// resolution, even when the scene was made for another projector size.
TEST(SceneTakeRoutingTest, TakeOntoALoneOutputAdoptsItsResolution) {
    EditorState es;
    OutputManager om;
    OutputWindow* out = om.createOutput("P1");  // 1920x1080
    Scene& first = es.activeScene();
    OutputManager::showScene(*out, &first, es.scenes);
    Scene* second = es.addScene("Two");
    second->setCanvasSize(out->getEffectiveResolution());  // as the Scenes panel's New does
    es.setActiveScene(first.getId());
    om.followOutputSizes(es.scenes);
    out->setResolution(IVec2(1280, 800));  // a WXGA projector
    om.followOutputSizes(es.scenes);
    ASSERT_EQ(first.getCanvasSize(), IVec2(1280, 800));

    es.takeScene(2, om);
    EXPECT_EQ(out->getSceneId(), second->getId());
    EXPECT_EQ(second->getCanvasSize(), IVec2(1280, 800));
    EXPECT_EQ(out->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1280.0f, 800.0f}));
}
