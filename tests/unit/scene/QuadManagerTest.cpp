#include <gtest/gtest.h>
#include "scene/QuadManager.hpp"

namespace quadmapper {
namespace test {

class QuadManagerTest : public ::testing::Test {
protected:
    QuadManager manager;
};

// ============ Creation and Deletion Tests ============

TEST_F(QuadManagerTest, DefaultManagerIsEmpty) {
    EXPECT_TRUE(manager.isEmpty());
    EXPECT_EQ(manager.getQuadCount(), 0);
}

TEST_F(QuadManagerTest, CreateQuadAddsToManager) {
    Quad* quad = manager.createQuad();

    EXPECT_NE(quad, nullptr);
    EXPECT_EQ(manager.getQuadCount(), 1);
    EXPECT_FALSE(manager.isEmpty());
}

TEST_F(QuadManagerTest, CreateQuadWithNameSetsName) {
    Quad* quad = manager.createQuad("My Quad");

    EXPECT_EQ(quad->getName(), "My Quad");
}

TEST_F(QuadManagerTest, CreateQuadGeneratesUniqueName) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    EXPECT_NE(quad1->getName(), quad2->getName());
    EXPECT_NE(quad2->getName(), quad3->getName());
    EXPECT_NE(quad1->getName(), quad3->getName());
}

TEST_F(QuadManagerTest, DeleteQuadRemovesFromManager) {
    Quad* quad = manager.createQuad();
    UUID id = quad->getId();

    manager.deleteQuad(id);

    EXPECT_TRUE(manager.isEmpty());
    EXPECT_EQ(manager.getQuad(id), nullptr);
}

TEST_F(QuadManagerTest, InsertQuadKeepsIdAndPlacesAtIndex) {
    manager.createQuad("A");
    manager.createQuad("B");
    const UUID id = UUID::generate();
    auto quad = std::make_unique<Quad>(id);
    quad->setName("Restored");

    Quad* inserted = manager.insertQuad(std::move(quad), 1);

    ASSERT_NE(inserted, nullptr);
    EXPECT_EQ(inserted->getId(), id);
    EXPECT_EQ(manager.getQuads()[1]->getId(), id);
    EXPECT_EQ(inserted->getZOrder(), 1);
    EXPECT_EQ(manager.getQuads()[2]->getZOrder(), 2);
}

TEST_F(QuadManagerTest, InsertQuadRejectsDuplicateIdAndClampsIndex) {
    Quad* existing = manager.createQuad();

    EXPECT_EQ(manager.insertQuad(std::make_unique<Quad>(existing->getId()), 0), nullptr);
    EXPECT_EQ(manager.insertQuad(nullptr, 0), nullptr);

    Quad* last = manager.insertQuad(std::make_unique<Quad>(), 99);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->getZOrder(), 1);
}

TEST_F(QuadManagerTest, ClearRemovesAllQuads) {
    manager.createQuad();
    manager.createQuad();
    manager.createQuad();

    manager.clear();

    EXPECT_TRUE(manager.isEmpty());
    EXPECT_FALSE(manager.hasSelection());
}

// ============ Access Tests ============

TEST_F(QuadManagerTest, GetQuadByIdReturnsCorrectQuad) {
    Quad* quad = manager.createQuad("Test");
    UUID id = quad->getId();

    Quad* found = manager.getQuad(id);

    EXPECT_EQ(found, quad);
    EXPECT_EQ(found->getName(), "Test");
}

TEST_F(QuadManagerTest, GetQuadByIdReturnsNullForMissing) {
    UUID fakeId = UUID::generate();

    Quad* found = manager.getQuad(fakeId);

    EXPECT_EQ(found, nullptr);
}

TEST_F(QuadManagerTest, GetQuadsByZOrderReturnsSorted) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    quad1->setZOrder(3);
    quad2->setZOrder(1);
    quad3->setZOrder(2);

    auto sorted = manager.getQuadsByZOrder();

    EXPECT_EQ(sorted[0], quad2);
    EXPECT_EQ(sorted[1], quad3);
    EXPECT_EQ(sorted[2], quad1);
}

// ============ Selection Tests - Single Select ============

TEST_F(QuadManagerTest, SelectQuadSelectsOnlyOne) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();

    manager.selectQuad(quad1->getId());

    EXPECT_TRUE(manager.isSelected(quad1->getId()));
    EXPECT_FALSE(manager.isSelected(quad2->getId()));
    EXPECT_EQ(manager.getSelectionCount(), 1);
}

TEST_F(QuadManagerTest, SelectQuadClearsPreviousSelection) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();

    manager.selectQuad(quad1->getId());
    manager.selectQuad(quad2->getId());

    EXPECT_FALSE(manager.isSelected(quad1->getId()));
    EXPECT_TRUE(manager.isSelected(quad2->getId()));
    EXPECT_EQ(manager.getSelectionCount(), 1);
}

TEST_F(QuadManagerTest, DeselectQuadRemovesFromSelection) {
    Quad* quad = manager.createQuad();

    manager.selectQuad(quad->getId());
    manager.deselectQuad(quad->getId());

    EXPECT_FALSE(manager.isSelected(quad->getId()));
    EXPECT_FALSE(manager.hasSelection());
}

TEST_F(QuadManagerTest, DeselectAllClearsSelection) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();

    manager.addToSelection(quad1->getId());
    manager.addToSelection(quad2->getId());
    manager.deselectAll();

    EXPECT_FALSE(manager.hasSelection());
    EXPECT_EQ(manager.getSelectionCount(), 0);
}

TEST_F(QuadManagerTest, ToggleSelectionToggles) {
    Quad* quad = manager.createQuad();

    EXPECT_FALSE(manager.isSelected(quad->getId()));

    manager.toggleSelection(quad->getId());
    EXPECT_TRUE(manager.isSelected(quad->getId()));

    manager.toggleSelection(quad->getId());
    EXPECT_FALSE(manager.isSelected(quad->getId()));
}

// ============ Selection Tests - Multi-Select ============

TEST_F(QuadManagerTest, AddToSelectionAddsWithoutClearing) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    manager.addToSelection(quad1->getId());
    manager.addToSelection(quad2->getId());
    manager.addToSelection(quad3->getId());

    EXPECT_TRUE(manager.isSelected(quad1->getId()));
    EXPECT_TRUE(manager.isSelected(quad2->getId()));
    EXPECT_TRUE(manager.isSelected(quad3->getId()));
    EXPECT_EQ(manager.getSelectionCount(), 3);
}

TEST_F(QuadManagerTest, SelectAllSelectsEverything) {
    manager.createQuad();
    manager.createQuad();
    manager.createQuad();

    manager.selectAll();

    EXPECT_EQ(manager.getSelectionCount(), 3);
    for (const auto& quad : manager.getQuads()) {
        EXPECT_TRUE(manager.isSelected(quad->getId()));
    }
}

TEST_F(QuadManagerTest, SelectRangeSelectsRange) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();
    Quad* quad4 = manager.createQuad();

    manager.selectRange(quad1->getId(), quad3->getId());

    EXPECT_TRUE(manager.isSelected(quad1->getId()));
    EXPECT_TRUE(manager.isSelected(quad2->getId()));
    EXPECT_TRUE(manager.isSelected(quad3->getId()));
    EXPECT_FALSE(manager.isSelected(quad4->getId()));
}

TEST_F(QuadManagerTest, GetSelectedQuadsReturnsAll) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    manager.createQuad();

    manager.addToSelection(quad1->getId());
    manager.addToSelection(quad2->getId());

    auto selected = manager.getSelectedQuads();

    EXPECT_EQ(selected.size(), 2);
}

// ============ Primary Selection Tests ============

TEST_F(QuadManagerTest, PrimarySelectedIsLastSelected) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();

    manager.addToSelection(quad1->getId());
    manager.addToSelection(quad2->getId());

    EXPECT_EQ(manager.getPrimarySelectedId().value(), quad2->getId());
    EXPECT_EQ(manager.getPrimarySelectedQuad(), quad2);
}

TEST_F(QuadManagerTest, PrimarySelectedQuadNullWhenNoSelection) {
    manager.createQuad();

    EXPECT_EQ(manager.getPrimarySelectedQuad(), nullptr);
    EXPECT_FALSE(manager.getPrimarySelectedId().has_value());
}

TEST_F(QuadManagerTest, DeleteSelectedUpdatesPrimarySelection) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    manager.addToSelection(quad1->getId());
    manager.addToSelection(quad2->getId());

    // Primary is quad2
    manager.deleteQuad(quad2->getId());

    // Primary should update to remaining selected (quad1)
    EXPECT_EQ(manager.getPrimarySelectedId().value(), quad1->getId());
    EXPECT_FALSE(manager.isSelected(quad2->getId()));
    EXPECT_TRUE(manager.isSelected(quad1->getId()));
}

// ============ Ordering Tests ============

TEST_F(QuadManagerTest, MoveQuadUpChangesOrder) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    // Initial order: quad1(0), quad2(1), quad3(2)
    manager.moveQuadUp(quad2->getId());

    // New order: quad2(0), quad1(1), quad3(2)
    EXPECT_EQ(quad2->getZOrder(), 0);
    EXPECT_EQ(quad1->getZOrder(), 1);
    EXPECT_EQ(quad3->getZOrder(), 2);
}

TEST_F(QuadManagerTest, MoveQuadDownChangesOrder) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    // Initial order: quad1(0), quad2(1), quad3(2)
    manager.moveQuadDown(quad2->getId());

    // New order: quad1(0), quad3(1), quad2(2)
    EXPECT_EQ(quad1->getZOrder(), 0);
    EXPECT_EQ(quad3->getZOrder(), 1);
    EXPECT_EQ(quad2->getZOrder(), 2);
}

TEST_F(QuadManagerTest, MoveQuadToTopMovesToFront) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    manager.moveQuadToTop(quad3->getId());

    EXPECT_EQ(quad3->getZOrder(), 0);
    EXPECT_EQ(quad1->getZOrder(), 1);
    EXPECT_EQ(quad2->getZOrder(), 2);
}

TEST_F(QuadManagerTest, MoveQuadToBottomMovesToEnd) {
    Quad* quad1 = manager.createQuad();
    Quad* quad2 = manager.createQuad();
    Quad* quad3 = manager.createQuad();

    manager.moveQuadToBottom(quad1->getId());

    EXPECT_EQ(quad2->getZOrder(), 0);
    EXPECT_EQ(quad3->getZOrder(), 1);
    EXPECT_EQ(quad1->getZOrder(), 2);
}

// ============ Duplicate Tests ============

TEST_F(QuadManagerTest, DuplicateQuadCreatesNewQuad) {
    Quad* original = manager.createQuad("Original");
    original->setOpacity(0.5f);
    original->setVisible(false);

    Quad* duplicate = manager.duplicateQuad(original->getId());

    EXPECT_NE(duplicate, nullptr);
    EXPECT_NE(duplicate->getId(), original->getId());
    EXPECT_EQ(duplicate->getOpacity(), 0.5f);
    EXPECT_FALSE(duplicate->isVisible());
    EXPECT_EQ(manager.getQuadCount(), 2);
}

TEST_F(QuadManagerTest, DuplicateQuadHasCopyInName) {
    Quad* original = manager.createQuad("MyQuad");

    Quad* duplicate = manager.duplicateQuad(original->getId());

    EXPECT_TRUE(duplicate->getName().find("Copy") != std::string::npos);
}

TEST_F(QuadManagerTest, DuplicateCopiesFullStateIncludingMeshBezierAndMedia) {
    Quad* original = manager.createQuad("Original");
    auto& geo = original->getGeometry();
    geo.setMeshMode(MeshMode::MESH_EDIT);
    geo.setMeshVertexByIndex(4, Vec2(0.02f, 0.04f));
    geo.setBezierEnabled(true);
    geo.setBezierControl(1, 0, Vec2(0.9f, 0.4f));
    original->getTexture().mediaId = UUID::generate();
    original->getTexture().cropRect = NormalizedRect{0.1f, 0.1f, 0.5f, 0.5f};
    original->getTexture().flipV = true;
    ColorCorrection cc;
    cc.contrast = 1.5f;
    original->setColorCorrection(cc);
    SoftEdge se;
    se.right = 0.2f;
    original->setSoftEdge(se);

    Quad* duplicate = manager.duplicateQuad(original->getId());

    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->getGeometry().getMeshVertices(), geo.getMeshVertices());
    EXPECT_EQ(duplicate->getGeometry().getUCoords(), geo.getUCoords());
    EXPECT_EQ(duplicate->getGeometry().getVCoords(), geo.getVCoords());
    EXPECT_TRUE(duplicate->getGeometry().isBezierEnabled());
    EXPECT_EQ(duplicate->getGeometry().getBezierOffsets(), geo.getBezierOffsets());
    EXPECT_EQ(duplicate->getTexture().mediaId, original->getTexture().mediaId);
    EXPECT_FLOAT_EQ(duplicate->getTexture().cropRect.width, 0.5f);
    EXPECT_TRUE(duplicate->getTexture().flipV);
    EXPECT_FLOAT_EQ(duplicate->getColorCorrection().contrast, 1.5f);
    EXPECT_FLOAT_EQ(duplicate->getSoftEdge().right, 0.2f);
    // Offset so it's visible; everything else is relative to the corners.
    EXPECT_FLOAT_EQ(duplicate->getGeometry().getCorners()[0].x, geo.getCorners()[0].x + 0.05f);
    EXPECT_EQ(duplicate->getZOrder(), 1);
    EXPECT_EQ(manager.getPrimarySelectedId(), duplicate->getId());
}

TEST_F(QuadManagerTest, DuplicateWithTakenIdReturnsNull) {
    Quad* a = manager.createQuad();
    Quad* b = manager.createQuad();

    EXPECT_EQ(manager.duplicateQuad(a->getId(), b->getId()), nullptr);
    EXPECT_EQ(manager.getQuadCount(), 2);
}

TEST_F(QuadManagerTest, DuplicateInvalidIdReturnsNull) {
    UUID fakeId = UUID::generate();

    Quad* duplicate = manager.duplicateQuad(fakeId);

    EXPECT_EQ(duplicate, nullptr);
}

// ============ Selection Callback Tests ============

TEST_F(QuadManagerTest, SelectionCallbackIsCalled) {
    int callCount = 0;
    manager.setSelectionChangedCallback([&callCount]() {
        callCount++;
    });

    Quad* quad = manager.createQuad();
    manager.selectQuad(quad->getId());

    EXPECT_EQ(callCount, 1);

    manager.deselectAll();
    EXPECT_EQ(callCount, 2);
}

// ============ Edge Cases ============

TEST_F(QuadManagerTest, SelectNonexistentQuadDoesNothing) {
    UUID fakeId = UUID::generate();

    manager.selectQuad(fakeId);

    EXPECT_FALSE(manager.hasSelection());
}

TEST_F(QuadManagerTest, MoveQuadUpAtTopDoesNothing) {
    Quad* quad1 = manager.createQuad();
    manager.createQuad();

    int originalZOrder = quad1->getZOrder();
    manager.moveQuadUp(quad1->getId());

    EXPECT_EQ(quad1->getZOrder(), originalZOrder);
}

TEST_F(QuadManagerTest, MoveQuadDownAtBottomDoesNothing) {
    manager.createQuad();
    Quad* quad2 = manager.createQuad();

    int originalZOrder = quad2->getZOrder();
    manager.moveQuadDown(quad2->getId());

    EXPECT_EQ(quad2->getZOrder(), originalZOrder);
}

TEST_F(QuadManagerTest, GenerateUniqueNameAvoidsCollisions) {
    manager.createQuad("Quad 1");
    manager.createQuad("Quad 2");
    manager.createQuad("Quad 3");

    std::string name = manager.generateUniqueName();

    // Should not be any of the existing names
    EXPECT_NE(name, "Quad 1");
    EXPECT_NE(name, "Quad 2");
    EXPECT_NE(name, "Quad 3");
}

} // namespace test
} // namespace quadmapper
