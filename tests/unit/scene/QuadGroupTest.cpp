#include <gtest/gtest.h>
#include "scene/QuadGroup.hpp"
#include "scene/Scene.hpp"

namespace quadmapper {
namespace test {

// ============ QuadGroup Basic Tests ============

class QuadGroupTest : public ::testing::Test {
protected:
    QuadGroup group;
};

TEST_F(QuadGroupTest, DefaultConstructorGeneratesId) {
    EXPECT_FALSE(group.getId().toString().empty());
}

TEST_F(QuadGroupTest, DefaultNameIsNewGroup) {
    EXPECT_EQ(group.getName(), "New Group");
}

TEST_F(QuadGroupTest, SetNameChangesName) {
    group.setName("My Group");
    EXPECT_EQ(group.getName(), "My Group");
}

TEST_F(QuadGroupTest, DefaultVisibilityIsTrue) {
    EXPECT_TRUE(group.isVisible());
}

TEST_F(QuadGroupTest, SetVisibilityChangesValue) {
    group.setVisible(false);
    EXPECT_FALSE(group.isVisible());
}

TEST_F(QuadGroupTest, DefaultLockedIsFalse) {
    EXPECT_FALSE(group.isLocked());
}

TEST_F(QuadGroupTest, SetLockedChangesValue) {
    group.setLocked(true);
    EXPECT_TRUE(group.isLocked());
}

TEST_F(QuadGroupTest, DefaultCollapsedIsFalse) {
    EXPECT_FALSE(group.isCollapsed());
}

TEST_F(QuadGroupTest, SetCollapsedChangesValue) {
    group.setCollapsed(true);
    EXPECT_TRUE(group.isCollapsed());
}

TEST_F(QuadGroupTest, DefaultGroupIsEmpty) {
    EXPECT_TRUE(group.isEmpty());
    EXPECT_EQ(group.getMemberCount(), 0);
}

// ============ Member Management Tests ============

TEST_F(QuadGroupTest, AddMemberIncreasesMemberCount) {
    UUID quadId = UUID::generate();
    group.addMember(quadId);

    EXPECT_FALSE(group.isEmpty());
    EXPECT_EQ(group.getMemberCount(), 1);
}

TEST_F(QuadGroupTest, AddMemberMultipleTimesNoDuplicate) {
    UUID quadId = UUID::generate();
    group.addMember(quadId);
    group.addMember(quadId); // Add same ID again

    EXPECT_EQ(group.getMemberCount(), 1); // Should not duplicate
}

TEST_F(QuadGroupTest, HasMemberReturnsTrueForMember) {
    UUID quadId = UUID::generate();
    group.addMember(quadId);

    EXPECT_TRUE(group.hasMember(quadId));
}

TEST_F(QuadGroupTest, HasMemberReturnsFalseForNonMember) {
    UUID quadId = UUID::generate();
    UUID otherId = UUID::generate();
    group.addMember(quadId);

    EXPECT_FALSE(group.hasMember(otherId));
}

TEST_F(QuadGroupTest, RemoveMemberDecreasesMemberCount) {
    UUID quadId = UUID::generate();
    group.addMember(quadId);
    group.removeMember(quadId);

    EXPECT_TRUE(group.isEmpty());
    EXPECT_EQ(group.getMemberCount(), 0);
}

TEST_F(QuadGroupTest, RemoveNonExistentMemberDoesNothing) {
    UUID quadId = UUID::generate();
    UUID otherId = UUID::generate();
    group.addMember(quadId);

    group.removeMember(otherId); // Remove ID that's not in group

    EXPECT_EQ(group.getMemberCount(), 1); // Should still have one member
}

TEST_F(QuadGroupTest, GetMemberQuadIdsReturnsAllMembers) {
    UUID id1 = UUID::generate();
    UUID id2 = UUID::generate();
    UUID id3 = UUID::generate();

    group.addMember(id1);
    group.addMember(id2);
    group.addMember(id3);

    const auto& members = group.getMemberQuadIds();
    EXPECT_EQ(members.size(), 3);
}

// ============ Parent Group Tests ============

TEST_F(QuadGroupTest, DefaultParentGroupIsEmpty) {
    EXPECT_FALSE(group.getParentGroup().has_value());
}

TEST_F(QuadGroupTest, SetParentGroupSetsValue) {
    UUID parentId = UUID::generate();
    group.setParentGroup(parentId);

    EXPECT_TRUE(group.getParentGroup().has_value());
    EXPECT_EQ(group.getParentGroup().value(), parentId);
}

TEST_F(QuadGroupTest, ClearParentGroupSetsToNullopt) {
    UUID parentId = UUID::generate();
    group.setParentGroup(parentId);
    group.setParentGroup(std::nullopt);

    EXPECT_FALSE(group.getParentGroup().has_value());
}

// ============ Scene Group Management Tests ============

class SceneGroupTest : public ::testing::Test {
protected:
    Scene scene;
};

TEST_F(SceneGroupTest, DefaultSceneHasNoGroups) {
    EXPECT_EQ(scene.getGroupCount(), 0);
}

TEST_F(SceneGroupTest, CreateGroupReturnsGroup) {
    QuadGroup* group = scene.createGroup("Test Group");

    EXPECT_NE(group, nullptr);
    EXPECT_EQ(group->getName(), "Test Group");
    EXPECT_EQ(scene.getGroupCount(), 1);
}

TEST_F(SceneGroupTest, CreateGroupWithEmptyNameGeneratesUniqueName) {
    QuadGroup* group = scene.createGroup();

    EXPECT_NE(group, nullptr);
    EXPECT_FALSE(group->getName().empty());
    EXPECT_TRUE(group->getName().find("Group") != std::string::npos);
}

TEST_F(SceneGroupTest, GetGroupReturnsCorrectGroup) {
    QuadGroup* created = scene.createGroup("Test");
    QuadGroup* found = scene.getGroup(created->getId());

    EXPECT_EQ(found, created);
}

TEST_F(SceneGroupTest, GetGroupReturnsNullForInvalidId) {
    UUID fakeId = UUID::generate();
    QuadGroup* found = scene.getGroup(fakeId);

    EXPECT_EQ(found, nullptr);
}

TEST_F(SceneGroupTest, DeleteGroupRemovesFromScene) {
    QuadGroup* group = scene.createGroup("Test");
    UUID groupId = group->getId();

    scene.deleteGroup(groupId);

    EXPECT_EQ(scene.getGroupCount(), 0);
    EXPECT_EQ(scene.getGroup(groupId), nullptr);
}

TEST_F(SceneGroupTest, DeleteGroupDoesNotDeleteQuads) {
    Quad* quad = scene.createQuad();
    QuadGroup* group = scene.createGroup("Test");
    scene.addQuadToGroup(group->getId(), quad->getId());

    scene.deleteGroup(group->getId());

    // Quad should still exist
    EXPECT_NE(scene.getQuad(quad->getId()), nullptr);
}

TEST_F(SceneGroupTest, ClearGroupsRemovesAllGroups) {
    scene.createGroup("Group 1");
    scene.createGroup("Group 2");
    scene.createGroup("Group 3");

    scene.clearGroups();

    EXPECT_EQ(scene.getGroupCount(), 0);
}

// ============ Quad-to-Group Association Tests ============

TEST_F(SceneGroupTest, AddQuadToGroupAddsQuad) {
    Quad* quad = scene.createQuad();
    QuadGroup* group = scene.createGroup("Test");

    scene.addQuadToGroup(group->getId(), quad->getId());

    EXPECT_TRUE(group->hasMember(quad->getId()));
}

TEST_F(SceneGroupTest, RemoveQuadFromGroupRemovesQuad) {
    Quad* quad = scene.createQuad();
    QuadGroup* group = scene.createGroup("Test");
    scene.addQuadToGroup(group->getId(), quad->getId());

    scene.removeQuadFromGroup(group->getId(), quad->getId());

    EXPECT_FALSE(group->hasMember(quad->getId()));
}

TEST_F(SceneGroupTest, IsQuadInGroupReturnsCorrectValue) {
    Quad* quad = scene.createQuad();
    QuadGroup* group = scene.createGroup("Test");
    scene.addQuadToGroup(group->getId(), quad->getId());

    EXPECT_TRUE(scene.isQuadInGroup(group->getId(), quad->getId()));

    UUID fakeQuadId = UUID::generate();
    EXPECT_FALSE(scene.isQuadInGroup(group->getId(), fakeQuadId));
}

TEST_F(SceneGroupTest, GetGroupsForQuadReturnsAllGroups) {
    Quad* quad = scene.createQuad();
    QuadGroup* group1 = scene.createGroup("Group 1");
    QuadGroup* group2 = scene.createGroup("Group 2");
    scene.createGroup("Group 3"); // Not containing this quad

    scene.addQuadToGroup(group1->getId(), quad->getId());
    scene.addQuadToGroup(group2->getId(), quad->getId());

    auto groups = scene.getGroupsForQuad(quad->getId());
    EXPECT_EQ(groups.size(), 2);
}

TEST_F(SceneGroupTest, QuadCanBeInMultipleGroups) {
    Quad* quad = scene.createQuad();
    QuadGroup* group1 = scene.createGroup("Group 1");
    QuadGroup* group2 = scene.createGroup("Group 2");
    QuadGroup* group3 = scene.createGroup("Group 3");

    scene.addQuadToGroup(group1->getId(), quad->getId());
    scene.addQuadToGroup(group2->getId(), quad->getId());
    scene.addQuadToGroup(group3->getId(), quad->getId());

    EXPECT_TRUE(group1->hasMember(quad->getId()));
    EXPECT_TRUE(group2->hasMember(quad->getId()));
    EXPECT_TRUE(group3->hasMember(quad->getId()));
}

// ============ Group from Selection Tests ============

TEST_F(SceneGroupTest, CreateGroupFromSelectionCreatesGroup) {
    Quad* quad1 = scene.createQuad();
    Quad* quad2 = scene.createQuad();

    scene.getQuadManager().addToSelection(quad1->getId());
    scene.getQuadManager().addToSelection(quad2->getId());

    QuadGroup* group = scene.createGroupFromSelection("Selected");

    EXPECT_NE(group, nullptr);
    EXPECT_EQ(group->getName(), "Selected");
    EXPECT_EQ(group->getMemberCount(), 2);
    EXPECT_TRUE(group->hasMember(quad1->getId()));
    EXPECT_TRUE(group->hasMember(quad2->getId()));
}

TEST_F(SceneGroupTest, CreateGroupFromSelectionReturnsNullIfNoSelection) {
    scene.createQuad(); // Create but don't select

    QuadGroup* group = scene.createGroupFromSelection("Selected");

    EXPECT_EQ(group, nullptr);
}

// ============ Group Visibility/Lock Application Tests ============

TEST_F(SceneGroupTest, ApplyGroupVisibilitySetsAllMemberQuads) {
    Quad* quad1 = scene.createQuad();
    Quad* quad2 = scene.createQuad();
    QuadGroup* group = scene.createGroup("Test");
    scene.addQuadToGroup(group->getId(), quad1->getId());
    scene.addQuadToGroup(group->getId(), quad2->getId());

    scene.applyGroupVisibility(group->getId(), false);

    EXPECT_FALSE(quad1->isVisible());
    EXPECT_FALSE(quad2->isVisible());
    EXPECT_FALSE(group->isVisible());
}

TEST_F(SceneGroupTest, ApplyGroupLockSetsAllMemberQuads) {
    Quad* quad1 = scene.createQuad();
    Quad* quad2 = scene.createQuad();
    QuadGroup* group = scene.createGroup("Test");
    scene.addQuadToGroup(group->getId(), quad1->getId());
    scene.addQuadToGroup(group->getId(), quad2->getId());

    scene.applyGroupLock(group->getId(), true);

    EXPECT_TRUE(quad1->isLocked());
    EXPECT_TRUE(quad2->isLocked());
    EXPECT_TRUE(group->isLocked());
}

// ============ Quad Deletion Removes from Groups ============

TEST_F(SceneGroupTest, DeleteQuadRemovesFromAllGroups) {
    Quad* quad = scene.createQuad();
    QuadGroup* group1 = scene.createGroup("Group 1");
    QuadGroup* group2 = scene.createGroup("Group 2");

    scene.addQuadToGroup(group1->getId(), quad->getId());
    scene.addQuadToGroup(group2->getId(), quad->getId());

    scene.deleteQuad(quad->getId());

    EXPECT_FALSE(group1->hasMember(quad->getId()));
    EXPECT_FALSE(group2->hasMember(quad->getId()));
}

// ============ Unique Name Generation Tests ============

TEST_F(SceneGroupTest, GenerateUniqueGroupNameAvoidsCollisions) {
    scene.createGroup("Group 1");
    scene.createGroup("Group 2");
    scene.createGroup("Group 3");

    std::string name = scene.generateUniqueGroupName();

    // Should not be any of the existing names
    EXPECT_NE(name, "Group 1");
    EXPECT_NE(name, "Group 2");
    EXPECT_NE(name, "Group 3");
}

} // namespace test
} // namespace quadmapper
