#include <gtest/gtest.h>
#include "scene/Scene.hpp"
#include "scene/Quad.hpp"
#include "scene/MediaReferences.hpp"
#include "core/CommandHistory.hpp"

#include <unordered_map>

namespace quadmapper {
namespace test {

TEST(SceneTest, DefaultSceneIsEmpty) {
    Scene scene;

    EXPECT_TRUE(scene.getQuads().empty());
    EXPECT_EQ(scene.getName(), "New Scene");
    EXPECT_FALSE(scene.getId().isNull());
}

// TD-15: the renderer keys its GPU copy of a mesh on this revision.
TEST(MeshObjectTest, MeshRevisionChangesOnEveryMeshMutationOnly) {
    MeshObject a;
    MeshObject b;
    EXPECT_NE(a.getMeshRevision(), b.getMeshRevision());  // unique across objects

    uint64_t last = a.getMeshRevision();
    MeshData data;
    data.positions = {Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)};
    data.indices = {0, 1, 2};
    a.setMesh(data);
    EXPECT_NE(a.getMeshRevision(), last);
    last = a.getMeshRevision();

    a.editMesh().positions[1].x = 2.0f;  // in place: same storage, same sizes
    EXPECT_NE(a.getMeshRevision(), last);
    last = a.getMeshRevision();

    // Nothing else touches it.
    a.setName("Renamed");
    a.setVisible(false);
    a.setOpacity(0.5f);
    a.setTransform(glm::mat4(2.0f));
    a.getTexture().mediaId = UUID::generate();
    (void)a.getMesh();
    EXPECT_EQ(a.getMeshRevision(), last);

    // A copy holds the same data, so it keeps the revision; a new mesh on it does not
    // collide with anything the original ever had.
    MeshObject copy(a);
    EXPECT_EQ(copy.getMeshRevision(), last);
    EXPECT_EQ(a.cloneWithId(UUID::generate()).getMeshRevision(), last);
    copy.setMesh(data);
    EXPECT_NE(copy.getMeshRevision(), last);
    EXPECT_NE(copy.getMeshRevision(), a.getMeshRevision());
}

TEST(SceneTest, CreateQuadAddsToScene) {
    Scene scene;

    Quad* quad = scene.createQuad();

    EXPECT_NE(quad, nullptr);
    EXPECT_EQ(scene.getQuads().size(), 1);
}

TEST(SceneTest, DeleteQuadRemovesFromScene) {
    Scene scene;

    Quad* quad = scene.createQuad();
    UUID id = quad->getId();
    scene.deleteQuad(id);

    EXPECT_TRUE(scene.getQuads().empty());
}

TEST(SceneTest, GetQuadByIdWorks) {
    Scene scene;

    Quad* quad = scene.createQuad();
    quad->setName("TestQuad");
    UUID id = quad->getId();

    Quad* found = scene.getQuad(id);
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->getName(), "TestQuad");
}

TEST(SceneTest, GetQuadByIdReturnsNullForMissing) {
    Scene scene;

    UUID fakeId = UUID::generate();
    Quad* found = scene.getQuad(fakeId);

    EXPECT_EQ(found, nullptr);
}

TEST(SceneTest, QuadsReturnedInZOrder) {
    Scene scene;

    Quad* quad1 = scene.createQuad();
    quad1->setZOrder(2);
    Quad* quad2 = scene.createQuad();
    quad2->setZOrder(1);
    Quad* quad3 = scene.createQuad();
    quad3->setZOrder(3);

    auto quads = scene.getQuadsByZOrder();
    EXPECT_EQ(quads[0]->getZOrder(), 1);
    EXPECT_EQ(quads[1]->getZOrder(), 2);
    EXPECT_EQ(quads[2]->getZOrder(), 3);
}

TEST(SceneTest, CreateMultipleQuads) {
    Scene scene;

    scene.createQuad();
    scene.createQuad();
    scene.createQuad();

    EXPECT_EQ(scene.getQuads().size(), 3);
}

TEST(SceneTest, SetNameUpdatesName) {
    Scene scene;
    scene.setName("My Scene");

    EXPECT_EQ(scene.getName(), "My Scene");
}

TEST(SceneTest, SetActiveUpdatesState) {
    Scene scene;

    EXPECT_FALSE(scene.isActive());
    scene.setActive(true);
    EXPECT_TRUE(scene.isActive());
}

TEST(SceneTest, CreateGroupAddsToScene) {
    Scene scene;

    QuadGroup* group = scene.createGroup();

    EXPECT_NE(group, nullptr);
}

TEST(SceneTest, DeleteGroupRemovesFromScene) {
    Scene scene;

    QuadGroup* group = scene.createGroup();
    UUID id = group->getId();
    scene.deleteGroup(id);

    EXPECT_EQ(scene.getGroup(id), nullptr);
}

// ---- Duplicate scene ----------------------------------------------------------

namespace {

// A scene exercising everything a duplicate must carry: two quads (one bound to media,
// warped, selected), a nested group holding both, a 3D object, a view, an output.
std::unique_ptr<Scene> richScene(const UUID& mediaId, const UUID& outputId) {
    auto scene = std::make_unique<Scene>();
    scene->setName("Stage");
    scene->setActive(true);
    scene->setAssignedOutputId(outputId);
    scene->getViewState().panOffset = Vec2(0.25f, -0.5f);
    scene->getViewState().zoomLevel = 2.0f;
    scene->getViewState().gridVisible = false;

    QuadManager& qm = scene->getQuadManager();
    Quad* a = qm.createQuad("A");
    Quad* b = qm.createQuad("B");
    a->getTexture().mediaId = mediaId;
    a->getTexture().uvScale = Vec2(2.0f, 1.0f);
    a->setOpacity(0.5f);
    auto corners = b->getGeometry().getCorners();
    corners[2] = Vec2(0.9f, 0.8f);
    b->getGeometry().setCorners(corners);

    QuadGroup* outer = scene->createGroup("Outer");
    QuadGroup* inner = scene->createGroup("Inner");
    inner->setParentGroup(outer->getId());
    inner->setLocked(true);
    scene->addQuadToGroup(outer->getId(), a->getId());
    scene->addQuadToGroup(inner->getId(), b->getId());
    b->setParentGroup(inner->getId());

    MeshObject* object = scene->createObject("Box");
    object->editMesh().positions = {Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)};
    object->editMesh().indices = {0, 1, 2};
    object->getTexture().mediaId = mediaId;
    object->setOpacity(0.25f);

    qm.addToSelection(a->getId());
    qm.addToSelection(b->getId());  // primary
    return scene;
}

bool sceneHasQuad(const Scene& scene, const UUID& id) { return scene.getQuadManager().getQuad(id) != nullptr; }

}  // namespace

TEST(SceneDuplicateTest, CopyHasFreshIdsAndTheSameContent) {
    const UUID media = UUID::generate();
    auto source = richScene(media, UUID::generate());
    std::unordered_map<UUID, UUID> quadIds;

    auto copy = source->duplicate(&quadIds);

    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy->getId(), source->getId());
    EXPECT_EQ(copy->getName(), "Stage");
    EXPECT_FALSE(copy->isActive());
    EXPECT_FALSE(copy->hasAssignedOutput());
    EXPECT_EQ(copy->getViewState().panOffset, Vec2(0.25f, -0.5f));
    EXPECT_FLOAT_EQ(copy->getViewState().zoomLevel, 2.0f);
    EXPECT_FALSE(copy->getViewState().gridVisible);

    const auto& srcQuads = source->getQuads();
    const auto& dupQuads = copy->getQuads();
    ASSERT_EQ(dupQuads.size(), srcQuads.size());
    ASSERT_EQ(quadIds.size(), srcQuads.size());
    for (size_t i = 0; i < srcQuads.size(); ++i) {
        const Quad& s = *srcQuads[i];
        const Quad& d = *dupQuads[i];
        EXPECT_NE(d.getId(), s.getId());
        EXPECT_FALSE(sceneHasQuad(*source, d.getId()));
        EXPECT_EQ(quadIds.at(s.getId()), d.getId());
        EXPECT_EQ(d.getName(), s.getName());
        EXPECT_EQ(d.getZOrder(), s.getZOrder());
        EXPECT_EQ(d.getGeometry().getCorners(), s.getGeometry().getCorners());
        EXPECT_EQ(d.getTexture().mediaId, s.getTexture().mediaId);  // shared source
        EXPECT_EQ(d.getTexture().uvScale, s.getTexture().uvScale);
        EXPECT_FLOAT_EQ(d.getOpacity(), s.getOpacity());
    }
    EXPECT_EQ(dupQuads[0]->getTexture().mediaId, media);
}

TEST(SceneDuplicateTest, GroupsGetFreshIdsWithRemappedMembersAndParents) {
    auto source = richScene(UUID::generate(), UUID());
    std::unordered_map<UUID, UUID> quadIds;
    auto copy = source->duplicate(&quadIds);

    ASSERT_EQ(copy->getGroupCount(), 2u);
    const QuadGroup& srcOuter = *source->getGroups()[0];
    const QuadGroup& srcInner = *source->getGroups()[1];
    const QuadGroup& outer = *copy->getGroups()[0];
    const QuadGroup& inner = *copy->getGroups()[1];

    EXPECT_NE(outer.getId(), srcOuter.getId());
    EXPECT_NE(inner.getId(), srcInner.getId());
    EXPECT_EQ(outer.getName(), "Outer");
    EXPECT_EQ(inner.getName(), "Inner");
    EXPECT_TRUE(inner.isLocked());
    ASSERT_TRUE(inner.getParentGroup().has_value());
    EXPECT_EQ(*inner.getParentGroup(), outer.getId());  // parent follows the copy

    const UUID a = source->getQuads()[0]->getId();
    const UUID b = source->getQuads()[1]->getId();
    ASSERT_EQ(outer.getMemberCount(), 1u);
    EXPECT_TRUE(outer.hasMember(quadIds.at(a)));
    EXPECT_FALSE(outer.hasMember(a));
    ASSERT_EQ(inner.getMemberCount(), 1u);
    EXPECT_TRUE(inner.hasMember(quadIds.at(b)));

    const Quad* dupB = copy->getQuad(quadIds.at(b));
    ASSERT_TRUE(dupB->getParentGroup().has_value());
    EXPECT_EQ(*dupB->getParentGroup(), inner.getId());
}

TEST(SceneDuplicateTest, ObjectsAreCopiedWithFreshIds) {
    const UUID media = UUID::generate();
    auto source = richScene(media, UUID());
    auto copy = source->duplicate();

    ASSERT_EQ(copy->getObjects().size(), 1u);
    const MeshObject& s = *source->getObjects()[0];
    const MeshObject& d = *copy->getObjects()[0];
    EXPECT_NE(d.getId(), s.getId());
    EXPECT_EQ(d.getName(), "Box");
    EXPECT_EQ(d.getMesh().positions, s.getMesh().positions);
    EXPECT_EQ(d.getMesh().indices, s.getMesh().indices);
    EXPECT_EQ(d.getTexture().mediaId, media);
    EXPECT_FLOAT_EQ(d.getOpacity(), 0.25f);
}

TEST(SceneDuplicateTest, SelectionFollowsTheCopy) {
    auto source = richScene(UUID(), UUID());
    std::unordered_map<UUID, UUID> quadIds;
    auto copy = source->duplicate(&quadIds);

    const QuadManager& qm = copy->getQuadManager();
    EXPECT_EQ(qm.getSelectionCount(), 2u);
    ASSERT_TRUE(qm.getPrimarySelectedId().has_value());
    EXPECT_EQ(*qm.getPrimarySelectedId(), quadIds.at(*source->getQuadManager().getPrimarySelectedId()));
}

TEST(SceneDuplicateTest, CopyIsIndependentOfTheSource) {
    auto source = richScene(UUID(), UUID());
    auto copy = source->duplicate();
    const auto before = source->getQuads()[0]->getGeometry().getCorners();

    auto corners = copy->getQuads()[0]->getGeometry().getCorners();
    corners[0] = Vec2(0.3f, 0.3f);
    copy->getQuads()[0]->getGeometry().setCorners(corners);
    copy->getGroups()[0]->setName("Renamed");
    copy->getObjects()[0]->editMesh().positions.clear();
    copy->deleteQuad(copy->getQuads()[1]->getId());

    EXPECT_EQ(source->getQuads()[0]->getGeometry().getCorners(), before);
    EXPECT_EQ(source->getGroups()[0]->getName(), "Outer");
    EXPECT_EQ(source->getObjects()[0]->getMesh().positions.size(), 3u);
    EXPECT_EQ(source->getQuads().size(), 2u);
    EXPECT_EQ(source->getGroups()[1]->getMemberCount(), 1u);
}

TEST(SceneDuplicateTest, EmptySceneDuplicates) {
    Scene source;
    std::unordered_map<UUID, UUID> quadIds;
    auto copy = source.duplicate(&quadIds);
    EXPECT_TRUE(copy->getQuads().empty());
    EXPECT_TRUE(quadIds.empty());
}

TEST(SceneDuplicateTest, MediaReferencesCountTheCopy) {
    const UUID media = UUID::generate();
    SceneList scenes;
    scenes.push_back(richScene(media, UUID()));
    scenes.push_back(scenes[0]->duplicate());
    scenes.erase(scenes.begin());  // only the copy still uses the media
    CommandHistory history;

    EXPECT_EQ(referencedMediaIds(scenes, history).count(media), 1u);
}

TEST(SceneListTest, CopyNamesAreUnique) {
    SceneList scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->setName("Intro");
    EXPECT_EQ(uniqueSceneCopyName(scenes, "Intro"), "Intro copy");

    scenes.push_back(std::make_unique<Scene>());
    scenes[1]->setName("Intro copy");
    EXPECT_EQ(uniqueSceneCopyName(scenes, "Intro"), "Intro copy 2");

    scenes.push_back(std::make_unique<Scene>());
    scenes[2]->setName("Intro copy 2");
    EXPECT_EQ(uniqueSceneCopyName(scenes, "Intro"), "Intro copy 3");
    EXPECT_EQ(uniqueSceneCopyName(scenes, "Intro copy"), "Intro copy copy");
}

// ---- Reorder ------------------------------------------------------------------

namespace {

SceneList namedScenes(std::initializer_list<const char*> names) {
    SceneList scenes;
    for (const char* name : names) {
        scenes.push_back(std::make_unique<Scene>());
        scenes.back()->setName(name);
    }
    return scenes;
}

std::string order(const SceneList& scenes) {
    std::string out;
    for (const auto& s : scenes) out += s->getName();
    return out;
}

}  // namespace

TEST(SceneListTest, MoveSceneDownAndUp) {
    SceneList scenes = namedScenes({"A", "B", "C", "D"});
    EXPECT_TRUE(moveScene(scenes, 0, 2));
    EXPECT_EQ(order(scenes), "BCAD");
    EXPECT_TRUE(moveScene(scenes, 3, 0));
    EXPECT_EQ(order(scenes), "DBCA");
    EXPECT_TRUE(moveScene(scenes, 1, 2));  // Move down
    EXPECT_EQ(order(scenes), "DCBA");
    EXPECT_TRUE(moveScene(scenes, 2, 1));  // Move up
    EXPECT_EQ(order(scenes), "DBCA");
}

TEST(SceneListTest, MoveSceneRejectsBadIndices) {
    SceneList scenes = namedScenes({"A", "B"});
    EXPECT_FALSE(moveScene(scenes, 1, 1));
    EXPECT_FALSE(moveScene(scenes, 2, 0));
    EXPECT_FALSE(moveScene(scenes, 0, 2));
    EXPECT_EQ(order(scenes), "AB");
    SceneList empty;
    EXPECT_FALSE(moveScene(empty, 0, 0));
}

TEST(SceneListTest, FindSceneIndex) {
    SceneList scenes = namedScenes({"A", "B"});
    EXPECT_EQ(findSceneIndex(scenes, scenes[1]->getId()), 1);
    EXPECT_EQ(findSceneIndex(scenes, UUID::generate()), -1);
    EXPECT_EQ(findSceneIndex(scenes, UUID()), -1);
}

TEST(SceneListTest, AssignSceneToOutputIsOneScenePerOutput) {
    SceneList scenes = namedScenes({"A", "B"});
    const UUID out = UUID::generate();
    assignSceneToOutput(scenes, scenes[0]->getId(), out);
    EXPECT_EQ(scenes[0]->getAssignedOutputId(), out);

    assignSceneToOutput(scenes, scenes[1]->getId(), out);
    EXPECT_EQ(scenes[1]->getAssignedOutputId(), out);
    EXPECT_FALSE(scenes[0]->hasAssignedOutput());

    assignSceneToOutput(scenes, scenes[1]->getId(), UUID());
    EXPECT_FALSE(scenes[1]->hasAssignedOutput());
}

TEST(SceneListTest, RemovedOutputIsClearedFromEveryScene) {
    SceneList scenes = namedScenes({"A", "B", "C"});
    const UUID removed = UUID::generate();
    const UUID kept = UUID::generate();
    scenes[0]->setAssignedOutputId(removed);
    scenes[1]->setAssignedOutputId(kept);

    EXPECT_TRUE(clearOutputAssignments(scenes, removed));
    EXPECT_FALSE(scenes[0]->hasAssignedOutput());
    EXPECT_EQ(scenes[1]->getAssignedOutputId(), kept);
    EXPECT_FALSE(scenes[2]->hasAssignedOutput());

    EXPECT_FALSE(clearOutputAssignments(scenes, removed));  // nothing left to clear
    EXPECT_FALSE(clearOutputAssignments(scenes, UUID()));   // null never matches unassigned
    EXPECT_FALSE(scenes[2]->hasAssignedOutput());
}

// ---- Scene take, keys 1-9 (US-008) ----------------------------------------------

class SceneTakeTest : public ::testing::Test {
protected:
    void SetUp() override {
        scenes = namedScenes({"A", "B", "C"});
        scenes[0]->setAssignedOutputId(out1);  // A on air on output 1
    }

    // What EditorState::takeScene does with the plan.
    SceneTake take(int number) {
        const SceneTake t = planSceneTake(scenes, active, number, outputs);
        if (t.kind == SceneTake::Kind::Route) assignSceneToOutput(scenes, t.sceneId, t.outputId);
        if (t.kind != SceneTake::Kind::None) active = t.sceneId;
        return t;
    }

    const UUID& id(size_t i) const { return scenes[i]->getId(); }

    const UUID out1 = UUID::generate();
    const UUID out2 = UUID::generate();
    std::vector<SceneTakeOutput> outputs{{out1, true}, {out2, true}};
    SceneList scenes;
    UUID active;  // set in each test
};

TEST_F(SceneTakeTest, OutOfRangeDoesNothing) {
    active = id(0);
    for (int n : {0, -1, 4, 9, 10}) {
        EXPECT_EQ(planSceneTake(scenes, active, n, outputs).kind, SceneTake::Kind::None) << n;
    }
    SceneList none;
    EXPECT_EQ(planSceneTake(none, UUID(), 1, outputs).kind, SceneTake::Kind::None);
}

TEST_F(SceneTakeTest, UnassignedSceneTakesTheActiveScenesOutput) {
    active = id(0);
    const SceneTake t = take(2);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.sceneId, id(1));
    EXPECT_EQ(t.outputId, out1);
    EXPECT_EQ(scenes[1]->getAssignedOutputId(), out1);
    EXPECT_FALSE(scenes[0]->hasAssignedOutput());  // previous scene leaves the output
    EXPECT_EQ(active, id(1));
}

TEST_F(SceneTakeTest, SceneWithItsOwnOutputJustBecomesActive) {
    active = id(0);
    scenes[2]->setAssignedOutputId(out2);
    const SceneTake t = take(3);

    EXPECT_EQ(t.kind, SceneTake::Kind::Activate);
    EXPECT_EQ(t.sceneId, id(2));
    EXPECT_EQ(scenes[0]->getAssignedOutputId(), out1);  // routing unchanged
    EXPECT_EQ(scenes[2]->getAssignedOutputId(), out2);
    EXPECT_EQ(active, id(2));
}

TEST_F(SceneTakeTest, ActiveLiveSceneIsANoOp) {
    active = id(0);
    EXPECT_EQ(take(1).kind, SceneTake::Kind::None);
    EXPECT_EQ(scenes[0]->getAssignedOutputId(), out1);
}

TEST_F(SceneTakeTest, UnroutedActiveSceneFallsBackToTheFirstEnabledOutput) {
    scenes[0]->setAssignedOutputId(UUID());
    outputs = {{out1, false}, {out2, true}};
    active = id(0);
    const SceneTake t = take(2);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.outputId, out2);
    EXPECT_EQ(scenes[1]->getAssignedOutputId(), out2);
}

TEST_F(SceneTakeTest, FallbackOutputIsTakenFromTheSceneHoldingIt) {
    // Active A is unrouted; C holds the first enabled output. B takes it from C.
    scenes[0]->setAssignedOutputId(UUID());
    scenes[2]->setAssignedOutputId(out1);
    active = id(0);
    take(2);

    EXPECT_EQ(scenes[1]->getAssignedOutputId(), out1);
    EXPECT_FALSE(scenes[2]->hasAssignedOutput());
}

TEST_F(SceneTakeTest, UnroutedActiveSceneTakesTheFallbackOutputItself) {
    scenes[0]->setAssignedOutputId(UUID());
    active = id(0);
    const SceneTake t = take(1);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(scenes[0]->getAssignedOutputId(), out1);
}

TEST_F(SceneTakeTest, NoOutputAtAllJustActivates) {
    scenes[0]->setAssignedOutputId(UUID());
    outputs = {{out1, false}};
    active = id(0);

    EXPECT_EQ(take(1).kind, SceneTake::Kind::None);
    const SceneTake t = take(3);
    EXPECT_EQ(t.kind, SceneTake::Kind::Activate);
    EXPECT_FALSE(scenes[2]->hasAssignedOutput());
    EXPECT_EQ(active, id(2));

    outputs.clear();
    EXPECT_EQ(take(2).kind, SceneTake::Kind::Activate);
}

TEST_F(SceneTakeTest, ActiveSceneOnADisabledOutputStillHandsItOver) {
    outputs = {{out1, false}, {out2, true}};
    active = id(0);
    const SceneTake t = take(2);
    EXPECT_EQ(t.outputId, out1);  // O is the active scene's output, enabled or not
}

TEST_F(SceneTakeTest, AssignmentToADeletedOutputCountsAsNone) {
    const UUID deleted = UUID::generate();
    scenes[1]->setAssignedOutputId(deleted);
    active = id(0);
    const SceneTake t = take(2);
    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(scenes[1]->getAssignedOutputId(), out1);

    // Active scene on a deleted output: fall back to the first enabled one.
    scenes[1]->setAssignedOutputId(deleted);
    scenes[0]->setAssignedOutputId(UUID());
    const SceneTake u = take(1);
    EXPECT_EQ(u.kind, SceneTake::Kind::Route);
    EXPECT_EQ(u.outputId, out1);
}

TEST_F(SceneTakeTest, UnknownActiveSceneFallsBack) {
    active = UUID();
    scenes[0]->setAssignedOutputId(UUID());
    const SceneTake t = take(3);
    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.outputId, out1);
}

TEST_F(SceneTakeTest, NumberingFollowsListOrder) {
    active = id(0);
    const UUID c = id(2);
    ASSERT_TRUE(moveScene(scenes, 2, 0));  // C is now scene 1
    const SceneTake t = take(1);
    EXPECT_EQ(t.sceneId, c);
    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
}

TEST_F(SceneTakeTest, TakingBackAndForthKeepsOneSceneOnTheOutput) {
    active = id(0);
    take(2);
    take(3);
    take(1);
    EXPECT_EQ(scenes[0]->getAssignedOutputId(), out1);
    EXPECT_FALSE(scenes[1]->hasAssignedOutput());
    EXPECT_FALSE(scenes[2]->hasAssignedOutput());
    EXPECT_EQ(active, id(0));
}

TEST_F(SceneTakeTest, SecondOutputRoutingIsLeftAlone) {
    // Two projectors: A on 1, B on 2. From A, taking C swaps output 1 only.
    scenes[1]->setAssignedOutputId(out2);
    active = id(0);
    take(3);
    EXPECT_EQ(scenes[2]->getAssignedOutputId(), out1);
    EXPECT_FALSE(scenes[0]->hasAssignedOutput());
    EXPECT_EQ(scenes[1]->getAssignedOutputId(), out2);
}

} // namespace test
} // namespace quadmapper
