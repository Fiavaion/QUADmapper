#include <gtest/gtest.h>
#include "scene/SceneManager.hpp"
#include "scene/Scene.hpp"

namespace quadmapper {
namespace test {

// ============ getSceneByIndex Tests ============

TEST(SceneManagerTest, GetSceneByIndexReturnsValidScene) {
    SceneManager manager;

    // Manager starts with one default scene
    Scene* scene = manager.getSceneByIndex(0);
    EXPECT_NE(scene, nullptr);
    EXPECT_EQ(scene->getName(), "Scene 1");
}

TEST(SceneManagerTest, GetSceneByIndexReturnsNullForInvalidIndex) {
    SceneManager manager;

    Scene* scene = manager.getSceneByIndex(100);
    EXPECT_EQ(scene, nullptr);
}

TEST(SceneManagerTest, GetSceneByIndexWorksWithMultipleScenes) {
    SceneManager manager;

    manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    EXPECT_EQ(manager.getSceneCount(), 3);
    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 1");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 2");
    EXPECT_EQ(manager.getSceneByIndex(2)->getName(), "Scene 3");
    EXPECT_EQ(manager.getSceneByIndex(3), nullptr);
}

TEST(SceneManagerTest, ConstGetSceneByIndexWorks) {
    SceneManager manager;
    manager.createScene("Scene 2");

    const SceneManager& constManager = manager;
    const Scene* scene = constManager.getSceneByIndex(0);

    EXPECT_NE(scene, nullptr);
    EXPECT_EQ(scene->getName(), "Scene 1");
}

// ============ duplicateScene Tests ============

TEST(SceneManagerTest, DuplicateSceneCreatesNewScene) {
    SceneManager manager;
    Scene* original = manager.getSceneByIndex(0);
    original->setName("Original Scene");

    Scene* duplicate = manager.duplicateScene(original->getId());

    EXPECT_NE(duplicate, nullptr);
    EXPECT_EQ(manager.getSceneCount(), 2);
    EXPECT_EQ(duplicate->getName(), "Original Scene Copy");
}

TEST(SceneManagerTest, DuplicateSceneHasNewId) {
    SceneManager manager;
    Scene* original = manager.getSceneByIndex(0);

    Scene* duplicate = manager.duplicateScene(original->getId());

    EXPECT_NE(duplicate->getId(), original->getId());
}

TEST(SceneManagerTest, DuplicateSceneCopiesViewState) {
    SceneManager manager;
    Scene* original = manager.getSceneByIndex(0);

    // Modify view state
    original->getViewState().panOffset = Vec2(10.0f, 20.0f);
    original->getViewState().zoomLevel = 2.5f;
    original->getViewState().gridVisible = false;
    original->getViewState().snapToGrid = true;

    Scene* duplicate = manager.duplicateScene(original->getId());

    EXPECT_FLOAT_EQ(duplicate->getViewState().panOffset.x, 10.0f);
    EXPECT_FLOAT_EQ(duplicate->getViewState().panOffset.y, 20.0f);
    EXPECT_FLOAT_EQ(duplicate->getViewState().zoomLevel, 2.5f);
    EXPECT_FALSE(duplicate->getViewState().gridVisible);
    EXPECT_TRUE(duplicate->getViewState().snapToGrid);
}

TEST(SceneManagerTest, DuplicateSceneCopiesQuads) {
    SceneManager manager;
    Scene* original = manager.getSceneByIndex(0);

    // Create quads in original
    Quad* quad1 = original->createQuad();
    quad1->setName("Quad A");
    Quad* quad2 = original->createQuad();
    quad2->setName("Quad B");

    Scene* duplicate = manager.duplicateScene(original->getId());

    EXPECT_EQ(duplicate->getQuads().size(), 2);
    // Quads should have the same names but different IDs
    EXPECT_EQ(duplicate->getQuads()[0]->getName(), "Quad A");
    EXPECT_EQ(duplicate->getQuads()[1]->getName(), "Quad B");
    EXPECT_NE(duplicate->getQuads()[0]->getId(), quad1->getId());
    EXPECT_NE(duplicate->getQuads()[1]->getId(), quad2->getId());
}

TEST(SceneManagerTest, DuplicateSceneReturnsNullForInvalidId) {
    SceneManager manager;

    UUID fakeId = UUID::generate();
    Scene* duplicate = manager.duplicateScene(fakeId);

    EXPECT_EQ(duplicate, nullptr);
    EXPECT_EQ(manager.getSceneCount(), 1);
}

// ============ reset Tests ============

TEST(SceneManagerTest, ResetClearsAllScenes) {
    SceneManager manager;

    manager.createScene("Scene 2");
    manager.createScene("Scene 3");
    EXPECT_EQ(manager.getSceneCount(), 3);

    manager.reset();

    EXPECT_EQ(manager.getSceneCount(), 1);
    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 1");
}

TEST(SceneManagerTest, ResetCreatesActiveScene) {
    SceneManager manager;

    manager.reset();

    Scene* activeScene = manager.getActiveScene();
    EXPECT_NE(activeScene, nullptr);
    EXPECT_TRUE(activeScene->isActive());
}

TEST(SceneManagerTest, ResetClearsQuadsFromScenes) {
    SceneManager manager;
    Scene* scene = manager.getSceneByIndex(0);
    scene->createQuad();
    scene->createQuad();
    EXPECT_EQ(scene->getQuads().size(), 2);

    manager.reset();

    Scene* newScene = manager.getSceneByIndex(0);
    EXPECT_TRUE(newScene->getQuads().empty());
}

// ============ generateUniqueSceneName Tests ============

TEST(SceneManagerTest, GenerateUniqueSceneNameReturnsNextNumber) {
    SceneManager manager;

    std::string name = manager.generateUniqueSceneName();
    EXPECT_EQ(name, "Scene 2");
}

TEST(SceneManagerTest, GenerateUniqueSceneNameAvoidsConflicts) {
    SceneManager manager;
    manager.createScene("Scene 2");

    std::string name = manager.generateUniqueSceneName();
    EXPECT_EQ(name, "Scene 3");
}

TEST(SceneManagerTest, GenerateUniqueSceneNameHandlesGaps) {
    SceneManager manager;
    manager.createScene("Scene 2");
    manager.createScene("Scene 4");  // Skip Scene 3

    // Should find Scene 3 as available
    std::string name = manager.generateUniqueSceneName();
    // Actually, the algorithm starts from count+1, so it will be Scene 4
    // If Scene 4 exists, it goes to Scene 5
    EXPECT_EQ(name, "Scene 5");
}

TEST(SceneManagerTest, GenerateUniqueSceneNameWithCustomNames) {
    SceneManager manager;
    manager.getSceneByIndex(0)->setName("My Custom Scene");

    // Since "Scene 1" is now free, but algorithm uses count+1
    std::string name = manager.generateUniqueSceneName();
    EXPECT_EQ(name, "Scene 2");
}

// ============ getSceneCount Tests ============

TEST(SceneManagerTest, GetSceneCountReturnsCorrectValue) {
    SceneManager manager;

    EXPECT_EQ(manager.getSceneCount(), 1);

    manager.createScene();
    EXPECT_EQ(manager.getSceneCount(), 2);

    manager.createScene();
    EXPECT_EQ(manager.getSceneCount(), 3);
}

// ============ SceneViewState Tests ============

TEST(SceneTest, ViewStateHasDefaultValues) {
    Scene scene;

    EXPECT_FLOAT_EQ(scene.getViewState().panOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(scene.getViewState().panOffset.y, 0.0f);
    EXPECT_FLOAT_EQ(scene.getViewState().zoomLevel, 1.0f);
    EXPECT_TRUE(scene.getViewState().gridVisible);
    EXPECT_FALSE(scene.getViewState().snapToGrid);
}

TEST(SceneTest, ViewStateCanBeModified) {
    Scene scene;

    scene.getViewState().panOffset = Vec2(100.0f, -50.0f);
    scene.getViewState().zoomLevel = 0.5f;
    scene.getViewState().gridVisible = false;
    scene.getViewState().snapToGrid = true;

    EXPECT_FLOAT_EQ(scene.getViewState().panOffset.x, 100.0f);
    EXPECT_FLOAT_EQ(scene.getViewState().panOffset.y, -50.0f);
    EXPECT_FLOAT_EQ(scene.getViewState().zoomLevel, 0.5f);
    EXPECT_FALSE(scene.getViewState().gridVisible);
    EXPECT_TRUE(scene.getViewState().snapToGrid);
}

TEST(SceneTest, ResetViewResetsToDefaults) {
    Scene scene;

    // Modify view state
    scene.getViewState().panOffset = Vec2(100.0f, -50.0f);
    scene.getViewState().zoomLevel = 3.0f;
    scene.getViewState().gridVisible = false;
    scene.getViewState().snapToGrid = true;

    // Reset
    scene.resetView();

    // Should be back to defaults
    EXPECT_FLOAT_EQ(scene.getViewState().panOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(scene.getViewState().panOffset.y, 0.0f);
    EXPECT_FLOAT_EQ(scene.getViewState().zoomLevel, 1.0f);
    EXPECT_TRUE(scene.getViewState().gridVisible);
    EXPECT_FALSE(scene.getViewState().snapToGrid);
}

TEST(SceneTest, ConstViewStateAccessWorks) {
    Scene scene;
    scene.getViewState().zoomLevel = 2.0f;

    const Scene& constScene = scene;
    const SceneViewState& viewState = constScene.getViewState();

    EXPECT_FLOAT_EQ(viewState.zoomLevel, 2.0f);
}

// ============ canDeleteScene Tests ============

TEST(SceneManagerTest, CanDeleteSceneReturnsFalseWithOneScene) {
    SceneManager manager;

    EXPECT_EQ(manager.getSceneCount(), 1);
    EXPECT_FALSE(manager.canDeleteScene());
}

TEST(SceneManagerTest, CanDeleteSceneReturnsTrueWithMultipleScenes) {
    SceneManager manager;
    manager.createScene("Scene 2");

    EXPECT_EQ(manager.getSceneCount(), 2);
    EXPECT_TRUE(manager.canDeleteScene());
}

TEST(SceneManagerTest, CanDeleteSceneAfterDeletion) {
    SceneManager manager;
    Scene* scene2 = manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    EXPECT_TRUE(manager.canDeleteScene());

    manager.deleteScene(scene2->getId());

    EXPECT_EQ(manager.getSceneCount(), 2);
    EXPECT_TRUE(manager.canDeleteScene());

    // Delete one more
    manager.deleteScene(manager.getSceneByIndex(0)->getId());

    EXPECT_EQ(manager.getSceneCount(), 1);
    EXPECT_FALSE(manager.canDeleteScene());
}

// ============ getSceneIndex Tests ============

TEST(SceneManagerTest, GetSceneIndexReturnsCorrectIndex) {
    SceneManager manager;
    Scene* scene1 = manager.getSceneByIndex(0);
    Scene* scene2 = manager.createScene("Scene 2");
    Scene* scene3 = manager.createScene("Scene 3");

    auto idx1 = manager.getSceneIndex(scene1->getId());
    auto idx2 = manager.getSceneIndex(scene2->getId());
    auto idx3 = manager.getSceneIndex(scene3->getId());

    EXPECT_TRUE(idx1.has_value());
    EXPECT_TRUE(idx2.has_value());
    EXPECT_TRUE(idx3.has_value());

    EXPECT_EQ(*idx1, 0);
    EXPECT_EQ(*idx2, 1);
    EXPECT_EQ(*idx3, 2);
}

TEST(SceneManagerTest, GetSceneIndexReturnsNulloptForInvalidId) {
    SceneManager manager;

    UUID fakeId = UUID::generate();
    auto idx = manager.getSceneIndex(fakeId);

    EXPECT_FALSE(idx.has_value());
}

// ============ setActiveSceneByIndex Tests ============

TEST(SceneManagerTest, SetActiveSceneByIndexWorks) {
    SceneManager manager;
    manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    // Initially scene 1 is active
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 1");

    manager.setActiveSceneByIndex(1);
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 2");

    manager.setActiveSceneByIndex(2);
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 3");

    manager.setActiveSceneByIndex(0);
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 1");
}

TEST(SceneManagerTest, SetActiveSceneByIndexIgnoresInvalidIndex) {
    SceneManager manager;
    manager.createScene("Scene 2");

    manager.setActiveSceneByIndex(0);
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 1");

    // Try invalid index
    manager.setActiveSceneByIndex(100);

    // Should still be scene 1
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 1");
}

// ============ moveScene Tests ============

TEST(SceneManagerTest, MoveSceneReordersCorrectly) {
    SceneManager manager;
    manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 1");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 2");
    EXPECT_EQ(manager.getSceneByIndex(2)->getName(), "Scene 3");

    // Move scene 0 to position 2
    manager.moveScene(0, 2);

    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 2");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 3");
    EXPECT_EQ(manager.getSceneByIndex(2)->getName(), "Scene 1");
}

TEST(SceneManagerTest, MoveSceneFromEndToStart) {
    SceneManager manager;
    manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    // Move scene 2 to position 0
    manager.moveScene(2, 0);

    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 3");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 1");
    EXPECT_EQ(manager.getSceneByIndex(2)->getName(), "Scene 2");
}

TEST(SceneManagerTest, MoveSceneSamePositionNoChange) {
    SceneManager manager;
    manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    // Move scene 1 to position 1 (no change)
    manager.moveScene(1, 1);

    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 1");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 2");
    EXPECT_EQ(manager.getSceneByIndex(2)->getName(), "Scene 3");
}

TEST(SceneManagerTest, MoveSceneInvalidIndicesNoChange) {
    SceneManager manager;
    manager.createScene("Scene 2");

    // Invalid from index
    manager.moveScene(100, 0);
    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 1");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 2");

    // Invalid to index
    manager.moveScene(0, 100);
    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 1");
    EXPECT_EQ(manager.getSceneByIndex(1)->getName(), "Scene 2");
}

TEST(SceneManagerTest, MoveScenePreservesActiveScene) {
    SceneManager manager;
    Scene* scene2 = manager.createScene("Scene 2");
    manager.createScene("Scene 3");

    // Set scene 2 as active
    manager.setActiveScene(scene2->getId());
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 2");

    // Move scene 2 to position 0
    manager.moveScene(1, 0);

    // Scene 2 should still be active
    EXPECT_EQ(manager.getActiveScene()->getName(), "Scene 2");

    // And it should now be at index 0
    EXPECT_EQ(manager.getSceneByIndex(0)->getName(), "Scene 2");
}

// ============ getActiveSceneId Tests ============

TEST(SceneManagerTest, GetActiveSceneIdReturnsCorrectId) {
    SceneManager manager;
    Scene* scene = manager.getSceneByIndex(0);

    EXPECT_EQ(manager.getActiveSceneId(), scene->getId());
}

TEST(SceneManagerTest, GetActiveSceneIdUpdatesOnSwitch) {
    SceneManager manager;
    Scene* scene2 = manager.createScene("Scene 2");

    manager.setActiveScene(scene2->getId());

    EXPECT_EQ(manager.getActiveSceneId(), scene2->getId());
}

// ============ Const getActiveScene Tests ============

TEST(SceneManagerTest, ConstGetActiveSceneWorks) {
    SceneManager manager;
    Scene* scene = manager.getSceneByIndex(0);
    scene->setName("Active Scene");

    const SceneManager& constManager = manager;
    const Scene* activeScene = constManager.getActiveScene();

    EXPECT_NE(activeScene, nullptr);
    EXPECT_EQ(activeScene->getName(), "Active Scene");
}

} // namespace test
} // namespace quadmapper
