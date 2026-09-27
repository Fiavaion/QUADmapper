#include <gtest/gtest.h>
#include "scene/Scene.hpp"
#include "scene/Quad.hpp"
#include "scene/MediaReferences.hpp"
#include "scene/SceneFade.hpp"
#include "core/CommandHistory.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

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

// The world bounds are cached; every mesh change (in place too) and every transform
// change is seen, and a copy starts from correct bounds.
TEST(MeshObjectTest, WorldBoundsFollowMeshRevisionAndTransform) {
    MeshObject object;
    Vec3 lo, hi;
    object.worldBounds(lo, hi);
    EXPECT_GT(lo.x, hi.x);  // empty

    MeshData data;
    data.positions = {Vec3(0.0f), Vec3(1.0f, 2.0f, 3.0f)};
    object.setMesh(data);
    object.worldBounds(lo, hi);
    EXPECT_EQ(lo, Vec3(0.0f));
    EXPECT_EQ(hi, Vec3(1.0f, 2.0f, 3.0f));

    object.editMesh().positions[1] = Vec3(-4.0f, 2.0f, 3.0f);  // in place
    object.worldBounds(lo, hi);
    EXPECT_EQ(lo, Vec3(-4.0f, 0.0f, 0.0f));
    EXPECT_EQ(hi, Vec3(0.0f, 2.0f, 3.0f));

    object.setTransform(glm::translate(glm::mat4(1.0f), Vec3(10.0f, 0.0f, 0.0f)));
    object.worldBounds(lo, hi);
    EXPECT_EQ(lo, Vec3(6.0f, 0.0f, 0.0f));
    EXPECT_EQ(hi, Vec3(10.0f, 2.0f, 3.0f));

    MeshObject copy = object.cloneWithId(UUID::generate());
    copy.setTransform(glm::mat4(1.0f));
    copy.worldBounds(lo, hi);
    EXPECT_EQ(lo, Vec3(-4.0f, 0.0f, 0.0f));
    object.worldBounds(lo, hi);  // the original's cache is its own
    EXPECT_EQ(lo, Vec3(6.0f, 0.0f, 0.0f));

    object.setMesh(MeshData{});
    object.worldBounds(lo, hi);
    EXPECT_GT(lo.x, hi.x);
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
// warped, selected), a nested group holding both, a 3D object, a view, a canvas size.
std::unique_ptr<Scene> richScene(const UUID& mediaId) {
    auto scene = std::make_unique<Scene>();
    scene->setName("Stage");
    scene->setActive(true);
    scene->setCanvasSize(IVec2(3840, 1200));
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
    auto source = richScene(media);
    std::unordered_map<UUID, UUID> quadIds;

    auto copy = source->duplicate(&quadIds);

    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy->getId(), source->getId());
    EXPECT_EQ(copy->getName(), "Stage");
    EXPECT_FALSE(copy->isActive());
    EXPECT_EQ(copy->getCanvasSize(), IVec2(3840, 1200));
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
    auto source = richScene(UUID::generate());
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
    auto source = richScene(media);
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
    auto source = richScene(UUID());
    std::unordered_map<UUID, UUID> quadIds;
    auto copy = source->duplicate(&quadIds);

    const QuadManager& qm = copy->getQuadManager();
    EXPECT_EQ(qm.getSelectionCount(), 2u);
    ASSERT_TRUE(qm.getPrimarySelectedId().has_value());
    EXPECT_EQ(*qm.getPrimarySelectedId(), quadIds.at(*source->getQuadManager().getPrimarySelectedId()));
}

TEST(SceneDuplicateTest, CopyIsIndependentOfTheSource) {
    auto source = richScene(UUID());
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
    scenes.push_back(richScene(media));
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

TEST(SceneCanvasTest, DefaultIsFullHdAndSizeIsClamped) {
    Scene scene;
    EXPECT_EQ(scene.getCanvasSize(), IVec2(DEFAULT_CANVAS_WIDTH, DEFAULT_CANVAS_HEIGHT));
    scene.setCanvasSize(IVec2(3840, 1080));
    EXPECT_EQ(scene.getCanvasSize(), IVec2(3840, 1080));
    scene.setCanvasSize(IVec2(0, 100000));
    EXPECT_EQ(scene.getCanvasSize(), IVec2(1, MAX_CANVAS_DIMENSION));
}

// ---- Scene take, keys 1-9 (US-008, ADR-047 §5) ----------------------------------

class SceneTakeTest : public ::testing::Test {
protected:
    void SetUp() override {
        scenes = namedScenes({"A", "B", "C"});
        outputs = {{out1, true, id(0)}, {out2, true, UUID()}};  // A on air on output 1
    }

    // What EditorState::takeScene does with the plan.
    SceneTake take(int number) {
        const SceneTake t = planSceneTake(scenes, active, number, outputs);
        if (t.kind == SceneTake::Kind::Route) {
            for (SceneTakeOutput& o : outputs) {
                if (std::find(t.outputIds.begin(), t.outputIds.end(), o.id) != t.outputIds.end()) {
                    o.sceneId = t.sceneId;
                }
            }
        }
        if (t.kind != SceneTake::Kind::None) active = t.sceneId;
        return t;
    }

    const UUID& id(size_t i) const { return scenes[i]->getId(); }
    UUID shownOn(const UUID& output) const {
        for (const SceneTakeOutput& o : outputs) {
            if (o.id == output) return o.sceneId;
        }
        return UUID();
    }

    const UUID out1 = UUID::generate();
    const UUID out2 = UUID::generate();
    const UUID out3 = UUID::generate();
    std::vector<SceneTakeOutput> outputs;
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

TEST_F(SceneTakeTest, UnshownSceneTakesTheActiveScenesOutput) {
    active = id(0);
    const SceneTake t = take(2);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.sceneId, id(1));
    EXPECT_EQ(t.outputIds, std::vector<UUID>{out1});
    EXPECT_EQ(shownOn(out1), id(1));  // the previous scene leaves the output
    EXPECT_TRUE(shownOn(out2).isNull());
    EXPECT_EQ(active, id(1));
}

TEST_F(SceneTakeTest, ShownSceneJustBecomesActive) {
    active = id(0);
    outputs[1].sceneId = id(2);
    const SceneTake t = take(3);

    EXPECT_EQ(t.kind, SceneTake::Kind::Activate);
    EXPECT_EQ(t.sceneId, id(2));
    EXPECT_EQ(shownOn(out1), id(0));  // routing unchanged
    EXPECT_EQ(shownOn(out2), id(2));
    EXPECT_EQ(active, id(2));
}

TEST_F(SceneTakeTest, ActiveLiveSceneIsANoOp) {
    active = id(0);
    EXPECT_EQ(take(1).kind, SceneTake::Kind::None);
    EXPECT_EQ(shownOn(out1), id(0));
}

TEST_F(SceneTakeTest, UnroutedActiveSceneFallsBackToTheFirstEnabledOutput) {
    outputs = {{out1, false, UUID()}, {out2, true, UUID()}};
    active = id(0);
    const SceneTake t = take(2);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.outputIds, std::vector<UUID>{out2});
    EXPECT_EQ(shownOn(out2), id(1));
}

TEST_F(SceneTakeTest, FallbackTakesTheFirstEnabledOutputsWholeGroup) {
    // Active A is on no output; C is blended over outputs 1 and 3, B is on output 2.
    outputs = {{out1, true, id(2)}, {out2, true, id(1)}, {out3, true, id(2)}};
    active = id(0);
    const SceneTake t = take(1);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.outputIds, (std::vector<UUID>{out1, out3}));
    EXPECT_EQ(shownOn(out1), id(0));
    EXPECT_EQ(shownOn(out3), id(0));
    EXPECT_EQ(shownOn(out2), id(1));  // another scene's output: untouched
}

TEST_F(SceneTakeTest, FallbackOutputIsTakenFromTheSceneOnIt) {
    // Active A is on no output; C is on the first enabled output. B takes it from C.
    outputs = {{out1, true, id(2)}, {out2, true, UUID()}};
    active = id(0);
    take(2);
    EXPECT_EQ(shownOn(out1), id(1));
}

TEST_F(SceneTakeTest, UnroutedActiveSceneTakesTheFallbackOutputItself) {
    outputs[0].sceneId = UUID();
    active = id(0);
    const SceneTake t = take(1);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(shownOn(out1), id(0));
}

TEST_F(SceneTakeTest, NoOutputAtAllJustActivates) {
    outputs = {{out1, false, UUID()}};
    active = id(0);

    EXPECT_EQ(take(1).kind, SceneTake::Kind::None);
    const SceneTake t = take(3);
    EXPECT_EQ(t.kind, SceneTake::Kind::Activate);
    EXPECT_TRUE(shownOn(out1).isNull());
    EXPECT_EQ(active, id(2));

    outputs.clear();
    EXPECT_EQ(take(2).kind, SceneTake::Kind::Activate);
}

TEST_F(SceneTakeTest, ActiveSceneOnADisabledOutputStillHandsItOver) {
    outputs = {{out1, false, id(0)}, {out2, true, UUID()}};
    active = id(0);
    const SceneTake t = take(2);
    EXPECT_EQ(t.outputIds, std::vector<UUID>{out1});  // the active scene's output, enabled or not
}

TEST_F(SceneTakeTest, UnknownActiveSceneFallsBack) {
    active = UUID();
    outputs[0].sceneId = UUID();
    const SceneTake t = take(3);
    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.outputIds, std::vector<UUID>{out1});
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
    EXPECT_EQ(shownOn(out1), id(0));
    EXPECT_TRUE(shownOn(out2).isNull());
    EXPECT_EQ(active, id(0));
}

TEST_F(SceneTakeTest, SecondOutputRoutingIsLeftAlone) {
    // Two projectors: A on 1, B on 2. From A, taking C swaps output 1 only.
    outputs[1].sceneId = id(1);
    active = id(0);
    take(3);
    EXPECT_EQ(shownOn(out1), id(2));
    EXPECT_EQ(shownOn(out2), id(1));
}

// ADR-047 §5: a blended group (every output showing the active scene) changes
// together; an output showing another scene is left alone.
TEST_F(SceneTakeTest, BlendedGroupChangesTogether) {
    outputs = {{out1, true, id(0)}, {out2, true, id(0)}, {out3, true, id(1)}};
    active = id(0);
    const SceneTake t = take(3);

    EXPECT_EQ(t.kind, SceneTake::Kind::Route);
    EXPECT_EQ(t.outputIds, (std::vector<UUID>{out1, out2}));
    EXPECT_EQ(shownOn(out1), id(2));
    EXPECT_EQ(shownOn(out2), id(2));
    EXPECT_EQ(shownOn(out3), id(1));

    // And back: the whole group again.
    take(1);
    EXPECT_EQ(shownOn(out1), id(0));
    EXPECT_EQ(shownOn(out2), id(0));
    EXPECT_EQ(shownOn(out3), id(1));
}



// ---- Scene crossfades (ADR-047 §5, QM-121) ------------------------------------------

// Ids made per test: a namespace-scope UUID::generate() runs before the generator is
// set up (static initialization order), so the ids are members of the fixture.
class SceneFadeTest : public ::testing::Test {
protected:
    const UUID kOut = UUID::generate();
    const UUID kA = UUID::generate();
    const UUID kB = UUID::generate();
    const UUID kC = UUID::generate();
};

TEST_F(SceneFadeTest, MixFollowsTheFrameClockLinearly) {
    SceneFades fades;
    fades.begin(kOut, kA, kB, 100.0, 1.0);
    ASSERT_NE(fades.find(kOut), nullptr);
    EXPECT_EQ(fades.find(kOut)->fromSceneId, kA);
    EXPECT_EQ(fades.find(kOut)->toSceneId, kB);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 99.0), 0.0f);  // a clock before the start: the old scene
    EXPECT_FLOAT_EQ(fades.mix(kOut, 100.0), 0.0f);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 100.25), 0.25f);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 100.5), 0.5f);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 101.0), 1.0f);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 250.0), 1.0f);
    EXPECT_FLOAT_EQ(fades.mix(UUID::generate(), 100.5), 1.0f) << "an output not fading shows its scene";
}

// Gate (ADR-047 §8): a 1 s crossfade is smooth. A fake frame clock steps 60 Hz frames,
// then irregular ones: every frame shows a new mix (no repeated frame), the mix only
// rises, it is the elapsed wall time (not a frame count), and the fade completes on time.
TEST_F(SceneFadeTest, OneSecondFadeIsMonotonicWithNoRepeatedFrame) {
    for (const bool jitter : {false, true}) {
        SCOPED_TRACE(jitter ? "irregular frames" : "60 Hz frames");
        SceneFades fades;
        const double start = 5000.0;
        fades.begin(kOut, kA, kB, start, 1.0);
        std::vector<float> mixes;
        double now = start;
        int frame = 0;
        while (!fades.empty()) {
            mixes.push_back(fades.mix(kOut, now));
            EXPECT_NEAR(mixes.back(), std::min(1.0, now - start), 1e-6);
            // Retiring happens at the next frame's start, as Application::presentFrame does.
            ++frame;
            now = jitter ? now + (frame % 3 == 0 ? 0.021 : 0.012) : start + frame / 60.0;
            fades.retire(now, [](const UUID&, const SceneFade&) { return true; });
            ASSERT_LT(frame, 1000);
        }
        ASSERT_GE(mixes.size(), 2u);
        EXPECT_EQ(mixes.front(), 0.0f) << "the take's own frame still shows the old scene";
        for (size_t i = 1; i < mixes.size(); ++i) {
            EXPECT_GT(mixes[i], mixes[i - 1]) << "frame " << i;
        }
        if (!jitter) EXPECT_EQ(mixes.size(), 60u) << "a 1 s fade spans 60 frames at 60 Hz";
        EXPECT_GE(now - start, 1.0);
        EXPECT_LT(now - start, 1.0 + 0.022);
    }
}

TEST_F(SceneFadeTest, ZeroOrInvalidTimeIsACut) {
    SceneFades fades;
    for (double seconds : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        fades.begin(kOut, kA, kB, 10.0, seconds);
        EXPECT_TRUE(fades.empty()) << seconds;
    }
    fades.begin(kOut, kA, kA, 10.0, 1.0);  // same scene: nothing to fade
    EXPECT_TRUE(fades.empty());
    fades.begin(kOut, UUID::null(), kB, 10.0, 1.0);  // the output showed nothing: from black
    ASSERT_NE(fades.find(kOut), nullptr);
    EXPECT_TRUE(fades.find(kOut)->fromSceneId.isNull());
}

TEST_F(SceneFadeTest, FinishedFadesRetireAndRejectedOnesToo) {
    SceneFades fades;
    const UUID other = UUID::generate();
    fades.begin(kOut, kA, kB, 0.0, 1.0);
    fades.begin(other, kA, kC, 0.0, 2.0);
    auto all = [](const UUID&, const SceneFade&) { return true; };
    EXPECT_FALSE(fades.retire(0.99, all));
    EXPECT_TRUE(fades.retire(1.0, all));
    EXPECT_EQ(fades.find(kOut), nullptr);
    ASSERT_NE(fades.find(other), nullptr);
    EXPECT_TRUE(fades.retire(1.5, [&](const UUID& id, const SceneFade&) { return id != other; }));
    EXPECT_TRUE(fades.empty());
}

// Taking back the scene that is fading out reverses from where the fade is: no jump.
TEST_F(SceneFadeTest, TakingBackTheOutgoingSceneReversesWithoutAJump) {
    SceneFades fades;
    fades.begin(kOut, kA, kB, 0.0, 1.0);
    const double now = 0.3;  // A at 0.7, B at 0.3
    fades.begin(kOut, kB, kA, now, 2.0);
    const SceneFade* fade = fades.find(kOut);
    ASSERT_NE(fade, nullptr);
    EXPECT_EQ(fade->fromSceneId, kB);
    EXPECT_EQ(fade->toSceneId, kA);
    EXPECT_NEAR(fades.mix(kOut, now), 0.7, 1e-6) << "A keeps its 0.7";
    EXPECT_NEAR(fades.mix(kOut, now + 0.2), 0.8, 1e-6) << "then rises over the new 2 s time";
    EXPECT_FLOAT_EQ(fades.mix(kOut, now + 0.6), 1.0f);
}

// Any other scene: the fade in progress completes at once (the scene fading in is on
// screen) and the new fade starts from it.
TEST_F(SceneFadeTest, TakingAThirdSceneFadesFromTheSceneFadingIn) {
    SceneFades fades;
    fades.begin(kOut, kA, kB, 0.0, 1.0);
    fades.begin(kOut, kB, kC, 0.4, 1.0);
    const SceneFade* fade = fades.find(kOut);
    ASSERT_NE(fade, nullptr);
    EXPECT_EQ(fade->fromSceneId, kB);
    EXPECT_EQ(fade->toSceneId, kC);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 0.4), 0.0f);

    // Taking the scene already fading in changes nothing: the fade carries on as it was.
    fades.begin(kOut, kC, kC, 0.9, 5.0);  // what OutputManager::takeScene would pass
    ASSERT_NE(fades.find(kOut), nullptr);
    EXPECT_EQ(fades.find(kOut)->fromSceneId, kB);
    EXPECT_FLOAT_EQ(fades.mix(kOut, 0.9), 0.5f) << "carries on at the original time";

    // A cut during a fade ends it.
    fades.begin(kOut, kC, kA, 0.7, 0.0);
    EXPECT_TRUE(fades.empty());
}

TEST_F(SceneFadeTest, DeletingASceneEndsTheFadesInvolvingIt) {
    SceneFades fades;
    const UUID o2 = UUID::generate(), o3 = UUID::generate();
    fades.begin(kOut, kA, kB, 0.0, 1.0);
    fades.begin(o2, kB, kC, 0.0, 1.0);
    fades.begin(o3, kC, kA, 0.0, 1.0);
    fades.cancelScene(kB);
    EXPECT_EQ(fades.find(kOut), nullptr);
    EXPECT_EQ(fades.find(o2), nullptr);
    EXPECT_NE(fades.find(o3), nullptr);
    fades.cancelOutput(o3);
    EXPECT_TRUE(fades.empty());
}

// Audio cross-gain (non-realtime side): equal power, monotonic, exact at both ends.
TEST_F(SceneFadeTest, CrossGainIsEqualPowerMonotonicAndExactAtTheEnds) {
    EXPECT_EQ(crossfadeGains(0.0f).out, 1.0f);
    EXPECT_EQ(crossfadeGains(0.0f).in, 0.0f);
    EXPECT_EQ(crossfadeGains(1.0f).out, 0.0f);
    EXPECT_EQ(crossfadeGains(1.0f).in, 1.0f);
    EXPECT_EQ(crossfadeGains(-0.5f).out, 1.0f);
    EXPECT_EQ(crossfadeGains(std::nanf("")).out, 1.0f);
    EXPECT_EQ(crossfadeGains(3.0f).in, 1.0f);
    CrossGain last = crossfadeGains(0.0f);
    for (int i = 1; i <= 1000; ++i) {
        const CrossGain g = crossfadeGains(static_cast<float>(i) / 1000.0f);
        EXPECT_LE(g.out, last.out);
        EXPECT_GE(g.in, last.in);
        EXPECT_NEAR(g.out * g.out + g.in * g.in, 1.0f, 1e-5f) << "constant power";
        last = g;
    }
    EXPECT_NEAR(crossfadeGains(0.5f).out, std::sqrt(0.5f), 1e-6f);
}

} // namespace test
} // namespace quadmapper
