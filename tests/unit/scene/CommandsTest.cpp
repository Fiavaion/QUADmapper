#include "scene/MediaReferences.hpp"
#include <gtest/gtest.h>
#include "scene/Commands.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGroup.hpp"
#include "scene/Scene.hpp"
#include "app/GridUndo.hpp"
#include "core/CommandHistory.hpp"

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

// SetMeshResolutionCommand tests
TEST_F(SceneCommandsTest, SetMeshResolutionCommandExecutes) {
    Quad* quad = manager().createQuad();

    history_->execute(std::make_unique<SetMeshResolutionCommand>(ref(quad), 32));

    EXPECT_EQ(quad->getGeometry().getMeshResolution(), 32);
}

TEST_F(SceneCommandsTest, SetMeshResolutionCommandUndoRestoresVertices) {
    Quad* quad = manager().createQuad();
    int originalRes = quad->getGeometry().getMeshResolution();
    auto originalVertices = quad->getGeometry().getMeshVertices();

    history_->execute(std::make_unique<SetMeshResolutionCommand>(ref(quad), 32));
    history_->undo();

    EXPECT_EQ(quad->getGeometry().getMeshResolution(), originalRes);
    EXPECT_EQ(quad->getGeometry().getMeshVertices().size(), originalVertices.size());
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

// Grid undo (app/GridUndo.hpp) through the id-based command
TEST_F(SceneCommandsTest, GridChangeUndoRestoresGridAndIsSafeAfterDelete) {
    Quad* quad = manager().createQuad();
    QuadGeometry& geo = quad->getGeometry();
    GridSnapshot before = captureGrid(geo);
    geo.insertUDivision(0.3f);
    pushGridChange(*history_, ref(quad), "Add Column", before, captureGrid(geo));
    const int columnsAfter = geo.getColumns();

    history_->undo();
    EXPECT_EQ(geo.getUCoords(), before.u);
    EXPECT_EQ(geo.getMeshVertices(), before.offsets);

    history_->redo();
    EXPECT_EQ(geo.getColumns(), columnsAfter);

    manager().deleteQuad(quad->getId());
    EXPECT_FALSE(history_->undo());
}

} // namespace test
} // namespace quadmapper
