#include "scene/MediaReferences.hpp"
#include <gtest/gtest.h>
#include "scene/Commands.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGroup.hpp"
#include "scene/Scene.hpp"
#include "core/CommandHistory.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <map>

namespace quadmapper {
namespace test {

// Commands resolve their target by (scene id, quad id) through a resolver over a
// scene list, exactly as the editor does with EditorState::scenes.
class SceneCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        scenes_.push_back(std::make_unique<Scene>());
        history_ = std::make_unique<CommandHistory>();
    }

    Scene& scene() { return *scenes_.front(); }
    QuadManager& manager() { return scene().getQuadManager(); }
    QuadRef ref(const Quad* quad) { return QuadRef{&resolver_, scene().getId(), quad->getId()}; }

    std::vector<std::unique_ptr<Scene>> scenes_;
    SceneListResolver resolver_{scenes_};
    std::unique_ptr<CommandHistory> history_;
};

// A quad with non-default state in every part a delete/duplicate must preserve.
static void decorate(Quad& quad) {
    auto& geo = quad.getGeometry();
    geo.setMeshMode(MeshMode::MESH_EDIT);
    geo.setMeshVertexByIndex(5, Vec2(0.03f, -0.02f));
    geo.setBezierEnabled(true);
    geo.setBezierControl(0, 1, Vec2(0.5f, 0.1f));
    TextureBinding& tex = quad.getTexture();
    tex.mediaId = UUID::generate();
    tex.cropRect = NormalizedRect{0.1f, 0.2f, 0.5f, 0.6f};
    tex.flipH = true;
    tex.uvRotation = 0.25f;
    ColorCorrection cc;
    cc.brightness = 0.2f;
    cc.gamma = 1.8f;
    quad.setColorCorrection(cc);
    SoftEdge se;
    se.left = 0.15f;
    se.gamma = 2.0f;
    quad.setSoftEdge(se);
    quad.setOpacity(0.6f);
}

static void expectSameContent(const Quad& a, const Quad& b) {
    EXPECT_EQ(a.getGeometry().getMeshVertices(), b.getGeometry().getMeshVertices());
    EXPECT_EQ(a.getGeometry().getMeshMode(), b.getGeometry().getMeshMode());
    EXPECT_EQ(a.getGeometry().isBezierEnabled(), b.getGeometry().isBezierEnabled());
    EXPECT_EQ(a.getGeometry().getBezierOffsets(), b.getGeometry().getBezierOffsets());
    EXPECT_EQ(a.getTexture().mediaId, b.getTexture().mediaId);
    EXPECT_FLOAT_EQ(a.getTexture().cropRect.x, b.getTexture().cropRect.x);
    EXPECT_FLOAT_EQ(a.getTexture().cropRect.width, b.getTexture().cropRect.width);
    EXPECT_EQ(a.getTexture().flipH, b.getTexture().flipH);
    EXPECT_FLOAT_EQ(a.getTexture().uvRotation, b.getTexture().uvRotation);
    EXPECT_FLOAT_EQ(a.getColorCorrection().brightness, b.getColorCorrection().brightness);
    EXPECT_FLOAT_EQ(a.getColorCorrection().gamma, b.getColorCorrection().gamma);
    EXPECT_FLOAT_EQ(a.getSoftEdge().left, b.getSoftEdge().left);
    EXPECT_FLOAT_EQ(a.getSoftEdge().gamma, b.getSoftEdge().gamma);
    EXPECT_FLOAT_EQ(a.getOpacity(), b.getOpacity());
}

// MoveCornerCommand tests
TEST_F(SceneCommandsTest, MoveCornerCommandExecutes) {
    Quad* quad = manager().createQuad();
    Vec2 oldPos = quad->getGeometry().getCorners()[0];
    Vec2 newPos(0.5f, 0.5f);

    history_->execute(std::make_unique<MoveCornerCommand>(ref(quad), 0, oldPos, newPos));

    EXPECT_EQ(quad->getGeometry().getCorners()[0], newPos);
}

TEST_F(SceneCommandsTest, MoveCornerCommandUndoes) {
    Quad* quad = manager().createQuad();
    Vec2 oldPos = quad->getGeometry().getCorners()[0];
    Vec2 newPos(0.5f, 0.5f);

    history_->execute(std::make_unique<MoveCornerCommand>(ref(quad), 0, oldPos, newPos));
    history_->undo();

    EXPECT_EQ(quad->getGeometry().getCorners()[0], oldPos);
}

TEST_F(SceneCommandsTest, MoveCornerCommandDescription) {
    Quad* quad = manager().createQuad();
    auto cmd = std::make_unique<MoveCornerCommand>(ref(quad), 0, Vec2(0, 0), Vec2(1, 1));

    EXPECT_NE(cmd->getDescription().find("top-left"), std::string::npos);
}

TEST_F(SceneCommandsTest, MoveCornerCommandMerges) {
    Quad* quad = manager().createQuad();
    Vec2 pos0 = quad->getGeometry().getCorners()[0];
    Vec2 pos1(0.25f, 0.25f);
    Vec2 pos2(0.5f, 0.5f);

    history_->execute(std::make_unique<MoveCornerCommand>(ref(quad), 0, pos0, pos1));
    history_->execute(std::make_unique<MoveCornerCommand>(ref(quad), 0, pos1, pos2));

    // Should merge into single command
    EXPECT_EQ(history_->getUndoCount(), 1);

    history_->undo();
    EXPECT_EQ(quad->getGeometry().getCorners()[0], pos0);
}

TEST_F(SceneCommandsTest, MoveCornerCommandsOnDifferentQuadsDoNotMerge) {
    Quad* a = manager().createQuad();
    Quad* b = manager().createQuad();

    history_->execute(std::make_unique<MoveCornerCommand>(ref(a), 0, Vec2(0.3f), Vec2(0.1f)));
    history_->execute(std::make_unique<MoveCornerCommand>(ref(b), 0, Vec2(0.3f), Vec2(0.2f)));

    EXPECT_EQ(history_->getUndoCount(), 2);
}

TEST_F(SceneCommandsTest, MoveMeshVertexCommandUndoes) {
    Quad* quad = manager().createQuad();
    quad->getGeometry().setMeshMode(MeshMode::MESH_EDIT);

    history_->execute(std::make_unique<MoveMeshVertexCommand>(
        ref(quad), 3, Vec2(0.0f), Vec2(0.05f, 0.0f)));
    EXPECT_EQ(quad->getGeometry().getMeshVertexByIndex(3), Vec2(0.05f, 0.0f));

    history_->undo();
    EXPECT_EQ(quad->getGeometry().getMeshVertexByIndex(3), Vec2(0.0f));
}

// ============ Stale-target safety (the use-after-free this design removes) ============

TEST_F(SceneCommandsTest, UndoAfterQuadDestroyedIsSafeNoOp) {
    Quad* quad = manager().createQuad();
    const UUID id = quad->getId();
    history_->execute(std::make_unique<MoveCornerCommand>(
        ref(quad), 0, quad->getGeometry().getCorners()[0], Vec2(0.9f, 0.9f)));

    manager().deleteQuad(id);  // bypasses the history, as New/Load or a bug would

    EXPECT_FALSE(history_->undo());        // target gone: skipped, never dereferenced
    EXPECT_EQ(history_->getUndoCount(), 0);  // step consumed, stack not wedged
    EXPECT_TRUE(history_->canRedo());
}

TEST_F(SceneCommandsTest, UndoAfterWholeSceneDestroyedIsSafeNoOp) {
    Quad* quad = manager().createQuad();
    history_->execute(std::make_unique<MoveCornerCommand>(
        ref(quad), 0, quad->getGeometry().getCorners()[0], Vec2(0.9f, 0.9f)));
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(manager().createQuad())));

    scenes_.clear();  // the QuadManager dies with its Scene (project load)

    EXPECT_FALSE(history_->undo());
    EXPECT_FALSE(history_->undo());
    EXPECT_FALSE(history_->redo());
}

TEST_F(SceneCommandsTest, RedoAfterTargetRecreatedWithSameIdWorks) {
    Quad* quad = manager().createQuad();
    const UUID id = quad->getId();
    const Vec2 oldPos = quad->getGeometry().getCorners()[0];
    const Vec2 newPos(0.9f, 0.1f);
    history_->execute(std::make_unique<MoveCornerCommand>(ref(quad), 0, oldPos, newPos));

    manager().deleteQuad(id);
    EXPECT_FALSE(history_->undo());

    Quad* recreated = manager().insertQuad(std::make_unique<Quad>(id), 0);
    ASSERT_NE(recreated, nullptr);
    EXPECT_TRUE(history_->redo());
    EXPECT_EQ(recreated->getGeometry().getCorners()[0], newPos);
}

TEST_F(SceneCommandsTest, CommandResolvesIntoReplacementSceneWithSameId) {
    Quad* quad = manager().createQuad();
    const UUID sceneId = scene().getId();
    const UUID quadId = quad->getId();
    const Vec2 oldPos = quad->getGeometry().getCorners()[0];
    history_->execute(std::make_unique<MoveCornerCommand>(ref(quad), 0, oldPos, Vec2(0.8f)));

    // Replace the scene object (as a reload does); the old QuadManager is freed.
    scenes_.clear();
    scenes_.push_back(std::make_unique<Scene>(sceneId));
    Quad* reloaded = scenes_.front()->getQuadManager().insertQuad(std::make_unique<Quad>(quadId), 0);

    EXPECT_TRUE(history_->undo());
    EXPECT_EQ(reloaded->getGeometry().getCorners()[0], oldPos);
}

TEST_F(SceneCommandsTest, CommandTargetsItsOwnSceneAmongSeveral) {
    // The resolver must find the command's scene by id, not assume the first one.
    scenes_.push_back(std::make_unique<Scene>());
    Scene& second = *scenes_.back();
    Quad* quad = second.getQuadManager().createQuad();
    const Vec2 oldPos = quad->getGeometry().getCorners()[0];

    history_->execute(std::make_unique<MoveCornerCommand>(
        QuadRef{&resolver_, second.getId(), quad->getId()}, 0, oldPos, Vec2(0.8f)));

    EXPECT_EQ(quad->getGeometry().getCorners()[0], Vec2(0.8f));
    ASSERT_TRUE(history_->undo());
    EXPECT_EQ(quad->getGeometry().getCorners()[0], oldPos);
}

// --- Which media sources are still referenced (source release, QM-088) -------------

TEST_F(SceneCommandsTest, ReferencedMediaCoversQuadsInEveryScene) {
    scenes_.push_back(std::make_unique<Scene>());
    Quad* a = manager().createQuad();
    Quad* b = scenes_.back()->getQuadManager().createQuad();
    a->getTexture().mediaId = UUID::generate();
    b->getTexture().mediaId = UUID::generate();

    const auto ids = referencedMediaIds(scenes_, *history_);

    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(a->getTexture().mediaId));
    EXPECT_TRUE(ids.count(b->getTexture().mediaId));  // not only the active scene
}

TEST_F(SceneCommandsTest, ReferencedMediaCoversObjectsInEveryScene) {
    scenes_.push_back(std::make_unique<Scene>());
    MeshObject* object = scenes_.back()->createObject("Prop");
    const UUID media = UUID::generate();
    object->getTexture().mediaId = media;

    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(media));

    scenes_.back()->deleteObject(object->getId());
    EXPECT_FALSE(referencedMediaIds(scenes_, *history_).count(media));
}

TEST_F(SceneCommandsTest, ReferencedMediaKeepsWhatUndoCanRestore) {
    // A deleted quad's source must survive while Ctrl+Z can bring the quad back,
    // or undo restores the quad without its media (procedurals cannot be rebuilt).
    Quad* quad = manager().createQuad();
    const UUID media = UUID::generate();
    quad->getTexture().mediaId = media;
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(quad)));
    ASSERT_EQ(manager().getQuadCount(), 0u);

    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(media));

    history_->clear();  // New/Load: nothing can bring it back any more
    EXPECT_FALSE(referencedMediaIds(scenes_, *history_).count(media));
}

TEST_F(SceneCommandsTest, ReferencedMediaIncludesCompoundDeletes) {
    Quad* a = manager().createQuad();
    Quad* b = manager().createQuad();
    const UUID mediaA = UUID::generate();
    const UUID mediaB = UUID::generate();
    a->getTexture().mediaId = mediaA;
    b->getTexture().mediaId = mediaB;

    history_->beginCompoundCommand("Delete Quads");
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(a)));
    // Still inside the open group: its commands count too.
    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(mediaA));
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(b)));
    history_->endCompoundCommand();

    const auto ids = referencedMediaIds(scenes_, *history_);
    EXPECT_TRUE(ids.count(mediaA));
    EXPECT_TRUE(ids.count(mediaB));
}

TEST_F(SceneCommandsTest, CommandOnMissingTargetIsNotRecorded) {
    Quad* quad = manager().createQuad();
    QuadRef stale = ref(quad);
    manager().deleteQuad(quad->getId());

    history_->execute(std::make_unique<RenameQuadCommand>(stale, "x"));

    EXPECT_FALSE(history_->canUndo());
}

// CreateQuadCommand tests
TEST_F(SceneCommandsTest, CreateQuadCommandExecutes) {
    EXPECT_EQ(manager().getQuadCount(), 0);

    history_->execute(std::make_unique<CreateQuadCommand>(resolver_, scene().getId()));

    EXPECT_EQ(manager().getQuadCount(), 1);
}

TEST_F(SceneCommandsTest, CreateQuadCommandWithName) {
    history_->execute(std::make_unique<CreateQuadCommand>(resolver_, scene().getId(), "Custom Name"));

    Quad* quad = manager().getQuads()[0].get();
    EXPECT_EQ(quad->getName(), "Custom Name");
}

TEST_F(SceneCommandsTest, CreateQuadCommandUndoes) {
    history_->execute(std::make_unique<CreateQuadCommand>(resolver_, scene().getId()));
    EXPECT_EQ(manager().getQuadCount(), 1);

    history_->undo();

    EXPECT_EQ(manager().getQuadCount(), 0);
}

TEST_F(SceneCommandsTest, CreateQuadCommandRedoes) {
    auto cmd = std::make_unique<CreateQuadCommand>(resolver_, scene().getId());
    CreateQuadCommand* create = cmd.get();
    history_->execute(std::move(cmd));
    const UUID id = create->getCreatedQuadId();
    history_->undo();
    history_->redo();

    EXPECT_EQ(manager().getQuadCount(), 1);
    EXPECT_NE(manager().getQuad(id), nullptr);  // same id, so later commands stay valid
}

// DeleteQuadCommand tests
TEST_F(SceneCommandsTest, DeleteQuadCommandExecutes) {
    Quad* quad = manager().createQuad();

    history_->execute(std::make_unique<DeleteQuadCommand>(ref(quad)));

    EXPECT_EQ(manager().getQuadCount(), 0);
}

TEST_F(SceneCommandsTest, DeleteQuadCommandUndoRestoresQuad) {
    Quad* quad = manager().createQuad("Test Quad");
    quad->setOpacity(0.75f);
    quad->setVisible(false);

    history_->execute(std::make_unique<DeleteQuadCommand>(ref(quad)));
    history_->undo();

    EXPECT_EQ(manager().getQuadCount(), 1);
    Quad* restored = manager().getQuads()[0].get();
    EXPECT_EQ(restored->getName(), "Test Quad");
    EXPECT_FLOAT_EQ(restored->getOpacity(), 0.75f);
    EXPECT_FALSE(restored->isVisible());
}

TEST_F(SceneCommandsTest, DeleteQuadCommandUsesSceneGroupCleanup) {
    Quad* quad = manager().createQuad();
    const UUID id = quad->getId();
    QuadGroup* group = scene().createGroup("G");
    scene().addQuadToGroup(group->getId(), id);

    history_->execute(std::make_unique<DeleteQuadCommand>(ref(quad)));

    EXPECT_FALSE(group->hasMember(id));  // no stale member id left behind
}

TEST_F(SceneCommandsTest, DeleteQuadUndoRestoresFullStateZOrderAndGroups) {
    manager().createQuad("Back");
    Quad* quad = manager().createQuad("Middle");
    manager().createQuad("Front");
    decorate(*quad);
    const UUID id = quad->getId();
    const Quad before = *quad;
    QuadGroup* group = scene().createGroup("G");
    scene().addQuadToGroup(group->getId(), id);

    history_->execute(std::make_unique<DeleteQuadCommand>(ref(quad)));
    ASSERT_EQ(manager().getQuad(id), nullptr);
    ASSERT_TRUE(history_->undo());

    Quad* restored = manager().getQuad(id);
    ASSERT_NE(restored, nullptr);
    expectSameContent(*restored, before);
    EXPECT_EQ(restored->getName(), "Middle");
    EXPECT_EQ(restored->getZOrder(), 1);
    EXPECT_EQ(manager().getQuads()[1]->getId(), id);
    EXPECT_TRUE(group->hasMember(id));

    // Redo deletes the same quad again, with group cleanup.
    ASSERT_TRUE(history_->redo());
    EXPECT_EQ(manager().getQuad(id), nullptr);
    EXPECT_FALSE(group->hasMember(id));
}

TEST_F(SceneCommandsTest, CompoundDeleteUndoRestoresOriginalOrder) {
    std::vector<UUID> ids;
    for (int i = 0; i < 4; ++i) ids.push_back(manager().createQuad()->getId());

    history_->beginCompoundCommand("Delete quads");
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(manager().getQuad(ids[1]))));
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(manager().getQuad(ids[3]))));
    history_->execute(std::make_unique<DeleteQuadCommand>(ref(manager().getQuad(ids[0]))));
    history_->endCompoundCommand();
    ASSERT_EQ(manager().getQuadCount(), 1);
    EXPECT_EQ(history_->getUndoCount(), 1);

    history_->undo();

    ASSERT_EQ(manager().getQuadCount(), 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(manager().getQuads()[i]->getId(), ids[i]);
    }
}

// DuplicateQuadCommand tests
TEST_F(SceneCommandsTest, DuplicateQuadCommandCopiesFullStateAndIsUndoable) {
    Quad* source = manager().createQuad("Src");
    decorate(*source);
    const UUID sourceId = source->getId();

    auto cmd = std::make_unique<DuplicateQuadCommand>(ref(source));
    const UUID dupId = cmd->getDuplicateId();
    history_->execute(std::move(cmd));

    Quad* dup = manager().getQuad(dupId);
    ASSERT_NE(dup, nullptr);
    EXPECT_NE(dupId, sourceId);
    EXPECT_EQ(dup->getName(), "Src Copy");
    expectSameContent(*dup, *manager().getQuad(sourceId));
    EXPECT_EQ(manager().getPrimarySelectedId(), dupId);

    history_->undo();
    EXPECT_EQ(manager().getQuad(dupId), nullptr);
    EXPECT_EQ(manager().getQuadCount(), 1);

    history_->redo();
    Quad* redone = manager().getQuad(dupId);  // same id comes back
    ASSERT_NE(redone, nullptr);
    EXPECT_EQ(redone->getTexture().mediaId, manager().getQuad(sourceId)->getTexture().mediaId);
}

TEST_F(SceneCommandsTest, DuplicateOfMissingSourceIsNotRecorded) {
    Quad* source = manager().createQuad();
    QuadRef stale = ref(source);
    manager().deleteQuad(source->getId());

    history_->execute(std::make_unique<DuplicateQuadCommand>(stale));

    EXPECT_EQ(manager().getQuadCount(), 0);
    EXPECT_FALSE(history_->canUndo());
}

// SetQuadVisibilityCommand tests
TEST_F(SceneCommandsTest, SetQuadVisibilityCommandExecutes) {
    Quad* quad = manager().createQuad();
    EXPECT_TRUE(quad->isVisible());

    history_->execute(std::make_unique<SetQuadVisibilityCommand>(ref(quad), false));

    EXPECT_FALSE(quad->isVisible());
}

TEST_F(SceneCommandsTest, SetQuadVisibilityCommandUndoes) {
    Quad* quad = manager().createQuad();
    history_->execute(std::make_unique<SetQuadVisibilityCommand>(ref(quad), false));
    history_->undo();

    EXPECT_TRUE(quad->isVisible());
}

TEST_F(SceneCommandsTest, SetQuadVisibilityCommandDescription) {
    Quad* quad = manager().createQuad();

    auto showCmd = std::make_unique<SetQuadVisibilityCommand>(ref(quad), true);
    EXPECT_NE(showCmd->getDescription().find("Show"), std::string::npos);

    quad->setVisible(true);
    auto hideCmd = std::make_unique<SetQuadVisibilityCommand>(ref(quad), false);
    EXPECT_NE(hideCmd->getDescription().find("Hide"), std::string::npos);
}

// SetQuadLockedCommand tests
TEST_F(SceneCommandsTest, SetQuadLockedCommandExecutes) {
    Quad* quad = manager().createQuad();
    EXPECT_FALSE(quad->isLocked());

    history_->execute(std::make_unique<SetQuadLockedCommand>(ref(quad), true));

    EXPECT_TRUE(quad->isLocked());
}

// SetQuadOpacityCommand tests
TEST_F(SceneCommandsTest, SetQuadOpacityCommandExecutes) {
    Quad* quad = manager().createQuad();

    history_->execute(std::make_unique<SetQuadOpacityCommand>(ref(quad), 0.5f));

    EXPECT_FLOAT_EQ(quad->getOpacity(), 0.5f);
}

TEST_F(SceneCommandsTest, SetQuadOpacityCommandMerges) {
    Quad* quad = manager().createQuad();

    history_->execute(std::make_unique<SetQuadOpacityCommand>(ref(quad), 0.75f));
    history_->execute(std::make_unique<SetQuadOpacityCommand>(ref(quad), 0.5f));
    history_->execute(std::make_unique<SetQuadOpacityCommand>(ref(quad), 0.25f));

    // Should merge
    EXPECT_EQ(history_->getUndoCount(), 1);

    history_->undo();
    EXPECT_FLOAT_EQ(quad->getOpacity(), 1.0f);  // Original value
}

// RenameQuadCommand tests
TEST_F(SceneCommandsTest, RenameQuadCommandExecutes) {
    Quad* quad = manager().createQuad("Original");

    history_->execute(std::make_unique<RenameQuadCommand>(ref(quad), "New Name"));

    EXPECT_EQ(quad->getName(), "New Name");
}

TEST_F(SceneCommandsTest, RenameQuadCommandUndoes) {
    Quad* quad = manager().createQuad("Original");
    history_->execute(std::make_unique<RenameQuadCommand>(ref(quad), "New Name"));
    history_->undo();

    EXPECT_EQ(quad->getName(), "Original");
}

// MoveQuadCommand tests
TEST_F(SceneCommandsTest, MoveQuadCommandMovesAllCorners) {
    Quad* quad = manager().createQuad();
    auto originalCorners = quad->getGeometry().getCorners();

    Vec2 delta(0.1f, 0.2f);
    history_->execute(std::make_unique<MoveQuadCommand>(ref(quad), delta));

    auto newCorners = quad->getGeometry().getCorners();
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(newCorners[i].x, originalCorners[i].x + delta.x);
        EXPECT_FLOAT_EQ(newCorners[i].y, originalCorners[i].y + delta.y);
    }
}

TEST_F(SceneCommandsTest, MoveQuadCommandMerges) {
    Quad* quad = manager().createQuad();
    auto originalCorners = quad->getGeometry().getCorners();

    history_->execute(std::make_unique<MoveQuadCommand>(ref(quad), Vec2(0.1f, 0.0f)));
    history_->execute(std::make_unique<MoveQuadCommand>(ref(quad), Vec2(0.0f, 0.1f)));

    EXPECT_EQ(history_->getUndoCount(), 1);

    history_->undo();
    auto restoredCorners = quad->getGeometry().getCorners();
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(restoredCorners[i].x, originalCorners[i].x);
        EXPECT_FLOAT_EQ(restoredCorners[i].y, originalCorners[i].y);
    }
}

// QuadPropertyCommand tests (template; bezier undo uses it)
TEST_F(SceneCommandsTest, QuadPropertyCommandWorks) {
    Quad* quad = manager().createQuad();
    std::array<Vec2, 8> before = quad->getGeometry().getBezierOffsets();
    std::array<Vec2, 8> after = before;
    after[2] = Vec2(0.1f, 0.2f);

    auto cmd = std::make_unique<QuadPropertyCommand<std::array<Vec2, 8>>>(
        "Bezier Edge", ref(quad), before, after,
        [](Quad& q, const std::array<Vec2, 8>& o) { q.getGeometry().setBezierOffsets(o); });

    EXPECT_TRUE(cmd->execute());
    EXPECT_EQ(quad->getGeometry().getBezierOffsets(), after);

    EXPECT_TRUE(cmd->undo());
    EXPECT_EQ(quad->getGeometry().getBezierOffsets(), before);

    manager().deleteQuad(quad->getId());
    EXPECT_FALSE(cmd->execute());
}

// The Properties warp switch (ADR-058): one step that undoes and redoes the mode and
// leaves a meshed quad's control vertices where they are.
TEST_F(SceneCommandsTest, WarpModeChangeUndoesAndRedoesWithoutMovingTheMesh) {
    Quad* quad = manager().createQuad();
    QuadGeometry& geo = quad->getGeometry();
    geo.setCorners({Vec2(0.1f, 0.2f), Vec2(0.8f, 0.1f), Vec2(0.9f, 0.9f), Vec2(0.2f, 0.7f)});
    geo.setUniformGrid(2);
    geo.setMeshMode(MeshMode::MESH_EDIT);
    geo.setMeshVertex(1, 1, Vec2(0.02f, 0.03f));
    const std::vector<Vec2> positions = geo.generateMeshPositions();
    const WarpMode before = geo.getWarpMode();
    ASSERT_EQ(before, WarpMode::LINEAR);

    history_->execute(std::make_unique<QuadPropertyCommand<WarpMode>>(
        "Warp Mode", ref(quad), before, WarpMode::PERSPECTIVE,
        [](Quad& q, const WarpMode& m) { q.getGeometry().setWarpMode(m); }));
    auto expectMeshUnmoved = [&](const char* when) {
        const std::vector<Vec2> now = geo.generateMeshPositions();
        ASSERT_EQ(now.size(), positions.size()) << when;
        for (size_t i = 0; i < now.size(); ++i) {
            EXPECT_NEAR(now[i].x, positions[i].x, 1e-5f) << when << " vertex " << i;
            EXPECT_NEAR(now[i].y, positions[i].y, 1e-5f) << when << " vertex " << i;
        }
    };
    EXPECT_EQ(geo.getWarpMode(), WarpMode::PERSPECTIVE);
    expectMeshUnmoved("after the change");
    history_->undo();
    EXPECT_EQ(geo.getWarpMode(), WarpMode::LINEAR);
    expectMeshUnmoved("after undo");
    history_->redo();
    EXPECT_EQ(geo.getWarpMode(), WarpMode::PERSPECTIVE);
    expectMeshUnmoved("after redo");
}

// Grid-line handle moves (ADR-049 §3): consecutive moves of one handle merge into one
// step, another handle is its own step, and a deleted quad makes them no-ops.
TEST_F(SceneCommandsTest, MoveGridHandleMergesPerHandleAndIsSafeAfterDelete) {
    Quad* quad = manager().createQuad();
    QuadGeometry& geo = quad->getGeometry();
    geo.setUniformGrid(2);
    auto move = [&](int handle, const Vec2& by) {
        const VertexTangents before = geo.getVertexTangent(4);
        geo.setGridHandlePosition(4, handle, geo.gridHandlePosition(4, handle) + by);
        history_->execute(std::make_unique<MoveGridHandleCommand>(ref(quad), 4, handle, before,
                                                                  geo.getVertexTangent(4)));
    };
    move(GRID_HANDLE_U_AFTER, Vec2(0.01f, 0.0f));
    move(GRID_HANDLE_U_AFTER, Vec2(0.01f, 0.0f));
    move(GRID_HANDLE_V_BEFORE, Vec2(0.0f, 0.01f));

    ASSERT_TRUE(history_->undo());  // the V handle
    EXPECT_FALSE(geo.getVertexTangent(4).isExplicit(GRID_HANDLE_V_BEFORE));
    EXPECT_TRUE(geo.getVertexTangent(4).isExplicit(GRID_HANDLE_U_AFTER));
    ASSERT_TRUE(history_->undo());  // both U moves at once
    EXPECT_EQ(geo.getVertexTangent(4), VertexTangents{});

    ASSERT_TRUE(history_->redo());
    manager().deleteQuad(quad->getId());
    EXPECT_FALSE(history_->redo());
    EXPECT_FALSE(history_->undo());
}

// --- 3D object commands (TD-16) ----------------------------------------------------

class ObjectCommandsTest : public SceneCommandsTest {
protected:
    // A textured, placed object with a small mesh.
    MeshObject* addObject(const std::string& name) {
        MeshObject* object = scene().createObject(name);
        MeshData mesh;
        mesh.positions = {Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)};
        mesh.indices = {0, 1, 2};
        object->setMesh(mesh);
        object->setSourcePath("C:/show/" + name + ".obj");
        object->getTexture().mediaId = UUID::generate();
        object->setTransform(glm::mat4(2.0f));
        object->setOpacity(0.4f);
        return object;
    }
    ObjectRef oref(const MeshObject* object) {
        return ObjectRef{&resolver_, scene().getId(), object->getId()};
    }
    size_t indexOf(const UUID& id) {
        const auto& objects = scene().getObjects();
        for (size_t i = 0; i < objects.size(); ++i) {
            if (objects[i]->getId() == id) return i;
        }
        return objects.size();
    }
};

TEST_F(ObjectCommandsTest, DeleteUndoRestoresTheSameObjectAtItsPlace) {
    addObject("A");
    MeshObject* middle = addObject("B");
    addObject("C");
    const UUID id = middle->getId();
    const UUID media = middle->getTexture().mediaId;
    const uint64_t revision = middle->getMeshRevision();

    history_->execute(std::make_unique<DeleteObjectCommand>(oref(middle)));
    ASSERT_EQ(scene().getObjects().size(), 2u);
    EXPECT_EQ(scene().getObject(id), nullptr);

    ASSERT_TRUE(history_->undo());
    MeshObject* back = scene().getObject(id);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(indexOf(id), 1u);  // between A and C again
    EXPECT_EQ(back->getName(), "B");
    EXPECT_EQ(back->getTexture().mediaId, media);
    EXPECT_EQ(back->getTransform(), glm::mat4(2.0f));
    EXPECT_FLOAT_EQ(back->getOpacity(), 0.4f);
    EXPECT_EQ(back->getMesh().triangleCount(), 1u);
    EXPECT_EQ(back->getMeshRevision(), revision);  // the same object: no re-upload needed

    ASSERT_TRUE(history_->redo());
    EXPECT_EQ(scene().getObject(id), nullptr);
    EXPECT_EQ(scene().getObjects().size(), 2u);
}

TEST_F(ObjectCommandsTest, DeletedObjectKeepsItsMediaReferencedWhileUndoCanRestoreIt) {
    MeshObject* object = addObject("Prop");
    const UUID media = object->getTexture().mediaId;
    history_->execute(std::make_unique<DeleteObjectCommand>(oref(object)));

    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(media));
    history_->undo();
    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(media));  // live again
    history_->redo();
    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(media));

    history_->clear();  // New/Load
    EXPECT_FALSE(referencedMediaIds(scenes_, *history_).count(media));
}

TEST_F(ObjectCommandsTest, AddObjectInsertsAndUndoHoldsItWhole) {
    addObject("First");
    auto created = std::make_unique<MeshObject>();
    created->setName("Imported");
    const UUID id = created->getId();
    const UUID media = UUID::generate();
    created->getTexture().mediaId = media;
    MeshData mesh;
    mesh.positions = {Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)};
    mesh.indices = {0, 1, 2};
    created->setMesh(mesh);

    history_->execute(std::make_unique<AddObjectCommand>(resolver_, scene().getId(),
                                                         std::move(created), 0));
    ASSERT_NE(scene().getObject(id), nullptr);
    EXPECT_EQ(indexOf(id), 0u);
    EXPECT_EQ(history_->getUndoDescription(), "Import 3D object");

    ASSERT_TRUE(history_->undo());
    EXPECT_EQ(scene().getObject(id), nullptr);
    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(media));  // held by redo

    ASSERT_TRUE(history_->redo());
    ASSERT_NE(scene().getObject(id), nullptr);
    EXPECT_EQ(indexOf(id), 0u);
    EXPECT_EQ(scene().getObject(id)->getMesh().triangleCount(), 1u);
}

TEST_F(ObjectCommandsTest, AddObjectIntoAGoneSceneIsNotRecorded) {
    const UUID sceneId = scene().getId();
    scenes_.clear();
    auto created = std::make_unique<MeshObject>();
    history_->execute(std::make_unique<AddObjectCommand>(resolver_, sceneId, std::move(created), 0));
    EXPECT_FALSE(history_->canUndo());
}

TEST_F(ObjectCommandsTest, TransformDragMergesAndResetIsItsOwnStep) {
    MeshObject* object = addObject("Box");
    const glm::mat4 original = object->getTransform();
    const glm::mat4 step1 = glm::translate(original, Vec3(0.1f, 0.0f, 0.0f));
    const glm::mat4 step2 = glm::translate(original, Vec3(0.2f, 0.0f, 0.0f));

    history_->execute(makeSetObjectTransformCommand(oref(object), step1, true));
    history_->execute(makeSetObjectTransformCommand(oref(object), step2, true));
    EXPECT_EQ(object->getTransform(), step2);
    EXPECT_EQ(history_->getUndoCount(), 1u);  // one drag, one step

    history_->execute(makeSetObjectTransformCommand(oref(object), glm::mat4(1.0f), false));
    EXPECT_EQ(history_->getUndoCount(), 2u);

    history_->undo();
    EXPECT_EQ(object->getTransform(), step2);
    history_->undo();
    EXPECT_EQ(object->getTransform(), original);
    history_->redo();
    EXPECT_EQ(object->getTransform(), step2);
}

TEST_F(ObjectCommandsTest, OpacitySliderMergesPerObject) {
    MeshObject* a = addObject("A");
    MeshObject* b = addObject("B");
    history_->execute(makeSetObjectOpacityCommand(oref(a), 0.5f));
    history_->execute(makeSetObjectOpacityCommand(oref(a), 0.6f));
    history_->execute(makeSetObjectOpacityCommand(oref(b), 0.9f));  // another object: new step
    EXPECT_EQ(history_->getUndoCount(), 2u);

    history_->undo();
    EXPECT_FLOAT_EQ(b->getOpacity(), 0.4f);
    EXPECT_FLOAT_EQ(a->getOpacity(), 0.6f);
    history_->undo();
    EXPECT_FLOAT_EQ(a->getOpacity(), 0.4f);
}

TEST_F(ObjectCommandsTest, VisibilityAndRenameUndoAndRedo) {
    MeshObject* object = addObject("Old");
    history_->execute(makeSetObjectVisibilityCommand(oref(object), false));
    history_->execute(makeRenameObjectCommand(oref(object), "New"));
    history_->execute(makeRenameObjectCommand(oref(object), "Newer"));  // renames do not merge
    EXPECT_FALSE(object->isVisible());
    EXPECT_EQ(object->getName(), "Newer");
    EXPECT_EQ(history_->getUndoCount(), 3u);

    history_->undo();
    EXPECT_EQ(object->getName(), "New");
    history_->undo();
    EXPECT_EQ(object->getName(), "Old");
    history_->undo();
    EXPECT_TRUE(object->isVisible());
    history_->redo();
    EXPECT_FALSE(object->isVisible());
}

TEST_F(ObjectCommandsTest, TextureBindingUndoRestoresTheOldSourceAndKeepsBothReferenced) {
    MeshObject* object = addObject("Screen");
    const UUID oldMedia = object->getTexture().mediaId;
    const UUID newMedia = UUID::generate();
    TextureBinding bound = object->getTexture();
    bound.mediaId = newMedia;
    history_->execute(makeSetObjectTextureCommand(oref(object), bound));
    EXPECT_EQ(object->getTexture().mediaId, newMedia);

    // The old source can come back by undo, so it must not be released.
    const auto ids = referencedMediaIds(scenes_, *history_);
    EXPECT_TRUE(ids.count(oldMedia));
    EXPECT_TRUE(ids.count(newMedia));

    history_->undo();
    EXPECT_EQ(object->getTexture().mediaId, oldMedia);
    EXPECT_TRUE(referencedMediaIds(scenes_, *history_).count(newMedia));  // redo can
    history_->redo();
    EXPECT_EQ(object->getTexture().mediaId, newMedia);

    TextureBinding none = object->getTexture();
    none.mediaId = UUID();
    history_->execute(makeSetObjectTextureCommand(oref(object), none));
    EXPECT_TRUE(object->getTexture().mediaId.isNull());
    history_->undo();
    EXPECT_EQ(object->getTexture().mediaId, newMedia);
}

TEST_F(ObjectCommandsTest, PropertyStepsFollowTheObjectThroughDeleteAndUndo) {
    MeshObject* object = addObject("Box");
    const UUID id = object->getId();
    history_->execute(makeSetObjectOpacityCommand(oref(object), 1.0f));
    history_->execute(std::make_unique<DeleteObjectCommand>(oref(object)));

    history_->undo();  // back, same id
    history_->undo();  // the opacity step resolves the restored object by id
    ASSERT_NE(scene().getObject(id), nullptr);
    EXPECT_FLOAT_EQ(scene().getObject(id)->getOpacity(), 0.4f);
}

TEST_F(ObjectCommandsTest, StepsOnAGoneObjectOrSceneAreSafeNoOps) {
    MeshObject* object = addObject("Box");
    const ObjectRef target = oref(object);
    history_->execute(makeSetObjectOpacityCommand(target, 0.9f));
    history_->execute(makeRenameObjectCommand(target, "Renamed"));

    // The object goes outside the history (e.g. its scene was deleted), and a new one
    // with a new id takes its place: nothing resolves to it.
    scene().deleteObject(target.objectId);
    MeshObject* other = addObject("Box");
    EXPECT_FALSE(history_->undo());
    EXPECT_FALSE(history_->undo());
    EXPECT_EQ(other->getName(), "Box");
    EXPECT_FLOAT_EQ(other->getOpacity(), 0.4f);
    EXPECT_FALSE(history_->redo());

    // Commands are not even made for an object that is gone.
    EXPECT_EQ(makeSetObjectOpacityCommand(target, 0.1f), nullptr);
    EXPECT_EQ(makeSetObjectTransformCommand(target, glm::mat4(1.0f), true), nullptr);
    EXPECT_EQ(makeSetObjectVisibilityCommand(target, false), nullptr);
    EXPECT_EQ(makeRenameObjectCommand(target, "x"), nullptr);
    EXPECT_EQ(makeSetObjectTextureCommand(target, TextureBinding{}), nullptr);

    // A delete of a gone object is not recorded; the undo of a delete whose scene is
    // gone fails without losing anything else.
    history_->execute(std::make_unique<DeleteObjectCommand>(target));
    EXPECT_FALSE(history_->canRedo());
    history_->execute(std::make_unique<DeleteObjectCommand>(oref(other)));
    scenes_.clear();
    EXPECT_FALSE(history_->undo());
    EXPECT_FALSE(history_->redo());
}

TEST_F(ObjectCommandsTest, RemapMediaReachesObjectsInCompoundAndOpenGroups) {
    MeshObject* a = addObject("A");
    MeshObject* b = addObject("B");
    const UUID from = UUID::generate();
    const UUID to = UUID::generate();
    a->getTexture().mediaId = from;
    b->getTexture().mediaId = from;
    const UUID aId = a->getId();
    const UUID bId = b->getId();

    history_->beginCompoundCommand("Delete objects");
    history_->execute(std::make_unique<DeleteObjectCommand>(oref(a)));
    history_->endCompoundCommand();
    history_->beginCompoundCommand("Still open");
    history_->execute(std::make_unique<DeleteObjectCommand>(oref(b)));
    history_->remapMediaId(from, to);
    history_->endCompoundCommand();

    EXPECT_FALSE(referencedMediaIds(scenes_, *history_).count(from));
    history_->undo();
    history_->undo();
    EXPECT_EQ(scene().getObject(aId)->getTexture().mediaId, to);
    EXPECT_EQ(scene().getObject(bId)->getTexture().mediaId, to);
}

TEST(SceneObjectListTest, TakeAndInsertKeepOrderAndRefuseDuplicateIds) {
    Scene scene;
    const UUID a = scene.createObject("A")->getId();
    const UUID b = scene.createObject("B")->getId();
    size_t index = 99;
    std::unique_ptr<MeshObject> taken = scene.takeObject(a, &index);
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(index, 0u);
    EXPECT_EQ(scene.takeObject(a), nullptr);
    EXPECT_NE(scene.insertObject(std::move(taken), 5), nullptr);  // clamped to the end
    EXPECT_EQ(scene.getObjects().back()->getId(), a);
    EXPECT_EQ(scene.insertObject(std::make_unique<MeshObject>(b), 0), nullptr);  // id taken
    EXPECT_EQ(scene.getObjects().size(), 2u);
}

// ---- Output slices and scene canvases (ADR-047 part B) ------------------------------

namespace {

// Outputs' slices by id, as EditorState's resolver reaches OutputManager.
class FakeSliceOutputs : public OutputResolver {
public:
    std::map<UUID, OutputSlices> outputs;

    CalibrationTarget findCalibration(const UUID&) override { return {}; }
    bool readSlices(const UUID& id, OutputSlices& out) override {
        auto it = outputs.find(id);
        if (it == outputs.end()) return false;
        out = it->second;
        return true;
    }
    bool writeSlices(const UUID& id, const OutputSlices& slices) override {
        auto it = outputs.find(id);
        if (it == outputs.end()) return false;
        it->second = slices;
        return true;
    }
};

OutputSlices withCorner(OutputSlices slices, int corner, Vec2 position) {
    slices.slices[0].warp.setCorner(corner, position);
    return slices;
}

}  // namespace

class SliceCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        start_.slices.push_back(makeFullCanvasSlice(IVec2(1920, 1080)));
        start_.canvas = IVec2(1920, 1080);
        outputs_.outputs[id_] = start_;
    }
    OutputRef ref() { return OutputRef{&outputs_, id_}; }
    const OutputSlices& now() { return outputs_.outputs[id_]; }

    FakeSliceOutputs outputs_;
    UUID id_ = UUID::generate();
    OutputSlices start_;
    CommandHistory history_;
};

TEST_F(SliceCommandsTest, SliceEditAppliesAndUndoes) {
    OutputSlices after = start_;
    after.slices[0].blend[SLICE_EDGE_RIGHT].width = 128.0f;
    after.slices.push_back(makeFullCanvasSlice(IVec2(1920, 1080)));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Add slice", "", start_, after));
    ASSERT_EQ(now().slices.size(), 2u);
    EXPECT_FLOAT_EQ(now().slices[0].blend[SLICE_EDGE_RIGHT].width, 128.0f);
    EXPECT_EQ(history_.getUndoDescription(), "Add slice");

    ASSERT_TRUE(history_.undo());
    ASSERT_EQ(now().slices.size(), 1u);
    EXPECT_FLOAT_EQ(now().slices[0].blend[SLICE_EDGE_RIGHT].width, 0.0f);
    ASSERT_TRUE(history_.redo());
    EXPECT_EQ(now().slices.size(), 2u);
}

// A take or routing rescales an output's slices to another canvas without an undo step
// (ADR-047 Amendment 5): undo and redo fit their snapshot to the canvas the output's
// slices are made for now, so the projector keeps the part of the picture recorded.
TEST_F(SliceCommandsTest, UndoAndRedoFitTheSnapshotToTheOutputsCanvasNow) {
    OutputSlices after = start_;
    after.slices[0].input = CanvasRect{960.0f, 0.0f, 960.0f, 1080.0f};
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Slice input", "", start_, after));

    // Routed to a canvas twice the size (not an undo step).
    OutputSlices& live = outputs_.outputs[id_];
    rescaleSlices(live.slices, live.canvas, IVec2(3840, 2160));
    live.canvas = IVec2(3840, 2160);

    ASSERT_TRUE(history_.undo());
    EXPECT_EQ(now().canvas, IVec2(3840, 2160));
    EXPECT_EQ(now().slices[0].input, (CanvasRect{0.0f, 0.0f, 3840.0f, 2160.0f}));
    ASSERT_TRUE(history_.redo());
    EXPECT_EQ(now().canvas, IVec2(3840, 2160));
    EXPECT_EQ(now().slices[0].input, (CanvasRect{1920.0f, 0.0f, 1920.0f, 2160.0f}));
}

// Edits of one key merge only while the output's canvas stays the same: a step never
// spans a routing change.
TEST_F(SliceCommandsTest, EditsAcrossACanvasChangeDoNotMerge) {
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner", start_,
                                                        withCorner(start_, 0, Vec2(0.1f, 0.0f))));
    OutputSlices& live = outputs_.outputs[id_];
    rescaleSlices(live.slices, live.canvas, IVec2(960, 540));
    live.canvas = IVec2(960, 540);
    const OutputSlices before = now();
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner", before,
                                                        withCorner(before, 0, Vec2(0.2f, 0.0f))));
    EXPECT_EQ(history_.getUndoCount(), 2u);
}

// Nudges (or a drag) of one warp corner are one undo step; another corner is another.
TEST_F(SliceCommandsTest, EditsOfOneTargetMergeIntoOneStep) {
    OutputSlices a = withCorner(start_, 1, Vec2(0.99f, 0.0f));
    OutputSlices b = withCorner(a, 1, Vec2(0.98f, 0.0f));
    OutputSlices c = withCorner(b, 1, Vec2(0.97f, 0.01f));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner 1", start_, a));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner 1", a, b));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner 1", b, c));
    EXPECT_EQ(history_.getUndoCount(), 1u);
    EXPECT_EQ(now().slices[0].warp.getCorners()[1], Vec2(0.97f, 0.01f));

    OutputSlices d = withCorner(c, 2, Vec2(0.9f, 0.9f));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner 2", c, d));
    EXPECT_EQ(history_.getUndoCount(), 2u);
    // An unkeyed edit never merges.
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Reset", "", d, start_));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Reset", "", start_, start_));
    EXPECT_EQ(history_.getUndoCount(), 4u);

    history_.undo();
    history_.undo();
    history_.undo();  // corner 2
    EXPECT_EQ(now().slices[0].warp.getCorners()[2], Vec2(1.0f, 1.0f));
    EXPECT_EQ(now().slices[0].warp.getCorners()[1], Vec2(0.97f, 0.01f));
    history_.undo();  // the whole corner-1 run
    EXPECT_TRUE(isIdentityWarp(now().slices[0].warp));
}

TEST_F(SliceCommandsTest, EditsOfTwoOutputsDoNotMerge) {
    const UUID other = UUID::generate();
    outputs_.outputs[other] = start_;
    OutputSlices a = withCorner(start_, 0, Vec2(0.01f, 0.0f));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "corner 0", start_, a));
    history_.execute(std::make_unique<SliceEditCommand>(OutputRef{&outputs_, other}, "Move",
                                                        "corner 0", start_, a));
    EXPECT_EQ(history_.getUndoCount(), 2u);
}

TEST_F(SliceCommandsTest, EditOfARemovedOutputIsASafeNoOp) {
    OutputSlices a = withCorner(start_, 0, Vec2(0.01f, 0.0f));
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "", start_, a));
    outputs_.outputs.clear();
    EXPECT_FALSE(history_.undo());
    // Never recorded when the output is already gone.
    history_.execute(std::make_unique<SliceEditCommand>(ref(), "Move", "", start_, a));
    EXPECT_FALSE(history_.canUndo());
}

TEST_F(SceneCommandsTest, SetSceneCanvasCommandSetsSizeAndLockAndUndoes) {
    const Vec2 corner = manager().createQuad()->getGeometry().getCorners()[2];
    history_->execute(std::make_unique<SetSceneCanvasCommand>(
        resolver_, nullptr, scene().getId(), CanvasSetting{IVec2(1920, 1080), false},
        CanvasSetting{IVec2(3840, 1080), true}));
    EXPECT_EQ(scene().getCanvasSize(), IVec2(3840, 1080));
    EXPECT_TRUE(scene().isCanvasLocked());
    // Quads are normalized: they keep their place on the resized canvas.
    EXPECT_EQ(manager().getQuads().front()->getGeometry().getCorners()[2], corner);

    ASSERT_TRUE(history_->undo());
    EXPECT_EQ(scene().getCanvasSize(), IVec2(1920, 1080));
    EXPECT_FALSE(scene().isCanvasLocked());

    scenes_.clear();
    EXPECT_FALSE(history_->redo()) << "the scene is gone: a logged no-op";
}

} // namespace test
} // namespace quadmapper
