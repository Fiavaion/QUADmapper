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
