// The pure helpers behind the list panels' destructive-action prompts and rename
// fields: counting who uses a media source, and refusing a blank name.

#include "ui/ConfirmPopup.hpp"
#include "ui/panels/MediaBinPanel.hpp"
#include "scene/MeshObject.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace quadmapper;

TEST(ListPanelHelpersTest, BlankNamesAreEmptyOrWhitespaceOnly) {
    EXPECT_TRUE(isBlankName(""));
    EXPECT_TRUE(isBlankName("   "));
    EXPECT_TRUE(isBlankName(" \t "));
    EXPECT_FALSE(isBlankName("Stage left"));
    EXPECT_FALSE(isBlankName("  a "));
}

TEST(ListPanelHelpersTest, CountMediaUsersCountsQuadsAndObjectsInEveryScene) {
    const UUID media = UUID::generate();
    const UUID other = UUID::generate();

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->getQuadManager().createQuad("A")->getTexture().mediaId = media;
    scenes[0]->getQuadManager().createQuad("B")->getTexture().mediaId = other;
    scenes[1]->getQuadManager().createQuad("C")->getTexture().mediaId = media;
    scenes[1]->createObject("Cube")->getTexture().mediaId = media;

    EXPECT_EQ(countMediaUsers(scenes, media), 3u);
    EXPECT_EQ(countMediaUsers(scenes, other), 1u);
    EXPECT_EQ(countMediaUsers(scenes, UUID::generate()), 0u);
}

TEST(ListPanelHelpersTest, CountMediaUsersIgnoresTheNullId) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->getQuadManager().createQuad("Unbound");  // mediaId is null

    EXPECT_EQ(countMediaUsers(scenes, UUID()), 0u);
}
