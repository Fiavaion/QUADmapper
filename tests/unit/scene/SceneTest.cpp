#include <gtest/gtest.h>
#include "scene/Scene.hpp"
#include "scene/Quad.hpp"

namespace quadmapper {
namespace test {

TEST(SceneTest, DefaultSceneIsEmpty) {
    Scene scene;

    EXPECT_TRUE(scene.getQuads().empty());
    EXPECT_EQ(scene.getName(), "New Scene");
    EXPECT_FALSE(scene.getId().isNull());
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

} // namespace test
} // namespace quadmapper
