#include <gtest/gtest.h>
#include "scene/Commands.hpp"
#include "scene/QuadManager.hpp"
#include "core/CommandHistory.hpp"

namespace quadmapper {
namespace test {

class SceneCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<QuadManager>();
        history_ = std::make_unique<CommandHistory>();
    }

    std::unique_ptr<QuadManager> manager_;
    std::unique_ptr<CommandHistory> history_;
};

// MoveCornerCommand tests
TEST_F(SceneCommandsTest, MoveCornerCommandExecutes) {
    Quad* quad = manager_->createQuad();
    Vec2 oldPos = quad->getGeometry().getCorners()[0];
    Vec2 newPos(0.5f, 0.5f);

    history_->execute(std::make_unique<MoveCornerCommand>(quad, 0, oldPos, newPos));

    EXPECT_EQ(quad->getGeometry().getCorners()[0], newPos);
}

TEST_F(SceneCommandsTest, MoveCornerCommandUndoes) {
    Quad* quad = manager_->createQuad();
    Vec2 oldPos = quad->getGeometry().getCorners()[0];
    Vec2 newPos(0.5f, 0.5f);

    history_->execute(std::make_unique<MoveCornerCommand>(quad, 0, oldPos, newPos));
    history_->undo();

    EXPECT_EQ(quad->getGeometry().getCorners()[0], oldPos);
}

TEST_F(SceneCommandsTest, MoveCornerCommandDescription) {
    Quad* quad = manager_->createQuad();
    auto cmd = std::make_unique<MoveCornerCommand>(quad, 0, Vec2(0, 0), Vec2(1, 1));

    EXPECT_NE(cmd->getDescription().find("top-left"), std::string::npos);
}

TEST_F(SceneCommandsTest, MoveCornerCommandMerges) {
    Quad* quad = manager_->createQuad();
    Vec2 pos0 = quad->getGeometry().getCorners()[0];
    Vec2 pos1(0.25f, 0.25f);
    Vec2 pos2(0.5f, 0.5f);

    history_->execute(std::make_unique<MoveCornerCommand>(quad, 0, pos0, pos1));
    history_->execute(std::make_unique<MoveCornerCommand>(quad, 0, pos1, pos2));

    // Should merge into single command
    EXPECT_EQ(history_->getUndoCount(), 1);

    history_->undo();
    EXPECT_EQ(quad->getGeometry().getCorners()[0], pos0);
}

// CreateQuadCommand tests
TEST_F(SceneCommandsTest, CreateQuadCommandExecutes) {
    EXPECT_EQ(manager_->getQuadCount(), 0);

    history_->execute(std::make_unique<CreateQuadCommand>(manager_.get()));

    EXPECT_EQ(manager_->getQuadCount(), 1);
}

TEST_F(SceneCommandsTest, CreateQuadCommandWithName) {
    history_->execute(std::make_unique<CreateQuadCommand>(manager_.get(), "Custom Name"));

    Quad* quad = manager_->getQuads()[0].get();
    EXPECT_EQ(quad->getName(), "Custom Name");
}

TEST_F(SceneCommandsTest, CreateQuadCommandUndoes) {
    history_->execute(std::make_unique<CreateQuadCommand>(manager_.get()));
    EXPECT_EQ(manager_->getQuadCount(), 1);

    history_->undo();

    EXPECT_EQ(manager_->getQuadCount(), 0);
}

TEST_F(SceneCommandsTest, CreateQuadCommandRedoes) {
    history_->execute(std::make_unique<CreateQuadCommand>(manager_.get()));
    history_->undo();
    history_->redo();

    EXPECT_EQ(manager_->getQuadCount(), 1);
}

// DeleteQuadCommand tests
TEST_F(SceneCommandsTest, DeleteQuadCommandExecutes) {
    Quad* quad = manager_->createQuad();
    UUID id = quad->getId();

    history_->execute(std::make_unique<DeleteQuadCommand>(manager_.get(), id));

    EXPECT_EQ(manager_->getQuadCount(), 0);
}

TEST_F(SceneCommandsTest, DeleteQuadCommandUndoRestoresQuad) {
    Quad* quad = manager_->createQuad("Test Quad");
    quad->setOpacity(0.75f);
    quad->setVisible(false);
    UUID id = quad->getId();

    history_->execute(std::make_unique<DeleteQuadCommand>(manager_.get(), id));
    history_->undo();

    EXPECT_EQ(manager_->getQuadCount(), 1);
    Quad* restored = manager_->getQuads()[0].get();
    EXPECT_EQ(restored->getName(), "Test Quad");
    EXPECT_FLOAT_EQ(restored->getOpacity(), 0.75f);
    EXPECT_FALSE(restored->isVisible());
}

// SetQuadVisibilityCommand tests
TEST_F(SceneCommandsTest, SetQuadVisibilityCommandExecutes) {
    Quad* quad = manager_->createQuad();
    EXPECT_TRUE(quad->isVisible());

    history_->execute(std::make_unique<SetQuadVisibilityCommand>(quad, false));

    EXPECT_FALSE(quad->isVisible());
}

TEST_F(SceneCommandsTest, SetQuadVisibilityCommandUndoes) {
    Quad* quad = manager_->createQuad();
    history_->execute(std::make_unique<SetQuadVisibilityCommand>(quad, false));
    history_->undo();

    EXPECT_TRUE(quad->isVisible());
}

TEST_F(SceneCommandsTest, SetQuadVisibilityCommandDescription) {
    Quad* quad = manager_->createQuad();

    auto showCmd = std::make_unique<SetQuadVisibilityCommand>(quad, true);
    EXPECT_NE(showCmd->getDescription().find("Show"), std::string::npos);

    quad->setVisible(true);
    auto hideCmd = std::make_unique<SetQuadVisibilityCommand>(quad, false);
    EXPECT_NE(hideCmd->getDescription().find("Hide"), std::string::npos);
}

// SetQuadLockedCommand tests
TEST_F(SceneCommandsTest, SetQuadLockedCommandExecutes) {
    Quad* quad = manager_->createQuad();
    EXPECT_FALSE(quad->isLocked());

    history_->execute(std::make_unique<SetQuadLockedCommand>(quad, true));

    EXPECT_TRUE(quad->isLocked());
}

// SetQuadOpacityCommand tests
TEST_F(SceneCommandsTest, SetQuadOpacityCommandExecutes) {
    Quad* quad = manager_->createQuad();

    history_->execute(std::make_unique<SetQuadOpacityCommand>(quad, 0.5f));

    EXPECT_FLOAT_EQ(quad->getOpacity(), 0.5f);
}

TEST_F(SceneCommandsTest, SetQuadOpacityCommandMerges) {
    Quad* quad = manager_->createQuad();

    history_->execute(std::make_unique<SetQuadOpacityCommand>(quad, 0.75f));
    history_->execute(std::make_unique<SetQuadOpacityCommand>(quad, 0.5f));
    history_->execute(std::make_unique<SetQuadOpacityCommand>(quad, 0.25f));

    // Should merge
    EXPECT_EQ(history_->getUndoCount(), 1);

    history_->undo();
    EXPECT_FLOAT_EQ(quad->getOpacity(), 1.0f);  // Original value
}

// RenameQuadCommand tests
TEST_F(SceneCommandsTest, RenameQuadCommandExecutes) {
    Quad* quad = manager_->createQuad("Original");

    history_->execute(std::make_unique<RenameQuadCommand>(quad, "New Name"));

    EXPECT_EQ(quad->getName(), "New Name");
}

TEST_F(SceneCommandsTest, RenameQuadCommandUndoes) {
    Quad* quad = manager_->createQuad("Original");
    history_->execute(std::make_unique<RenameQuadCommand>(quad, "New Name"));
    history_->undo();

    EXPECT_EQ(quad->getName(), "Original");
}

// MoveQuadCommand tests
TEST_F(SceneCommandsTest, MoveQuadCommandMovesAllCorners) {
    Quad* quad = manager_->createQuad();
    auto originalCorners = quad->getGeometry().getCorners();

    Vec2 delta(0.1f, 0.2f);
    history_->execute(std::make_unique<MoveQuadCommand>(quad, delta));

    auto newCorners = quad->getGeometry().getCorners();
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(newCorners[i].x, originalCorners[i].x + delta.x);
        EXPECT_FLOAT_EQ(newCorners[i].y, originalCorners[i].y + delta.y);
    }
}

TEST_F(SceneCommandsTest, MoveQuadCommandMerges) {
    Quad* quad = manager_->createQuad();
    auto originalCorners = quad->getGeometry().getCorners();

    history_->execute(std::make_unique<MoveQuadCommand>(quad, Vec2(0.1f, 0.0f)));
    history_->execute(std::make_unique<MoveQuadCommand>(quad, Vec2(0.0f, 0.1f)));

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
    Quad* quad = manager_->createQuad();
    int originalRes = quad->getGeometry().getMeshResolution();

    history_->execute(std::make_unique<SetMeshResolutionCommand>(quad, 32));

    EXPECT_EQ(quad->getGeometry().getMeshResolution(), 32);
}

TEST_F(SceneCommandsTest, SetMeshResolutionCommandUndoRestoresVertices) {
    Quad* quad = manager_->createQuad();
    int originalRes = quad->getGeometry().getMeshResolution();
    auto originalVertices = quad->getGeometry().getMeshVertices();

    history_->execute(std::make_unique<SetMeshResolutionCommand>(quad, 32));
    history_->undo();

    EXPECT_EQ(quad->getGeometry().getMeshResolution(), originalRes);
    EXPECT_EQ(quad->getGeometry().getMeshVertices().size(), originalVertices.size());
}

// SetPropertyCommand tests (template)
TEST_F(SceneCommandsTest, SetPropertyCommandWorks) {
    int value = 10;

    auto cmd = std::make_unique<SetPropertyCommand<int>>(
        "test value",
        10, 20,
        [&value](const int& v) { value = v; }
    );

    cmd->execute();
    EXPECT_EQ(value, 20);

    cmd->undo();
    EXPECT_EQ(value, 10);
}

} // namespace test
} // namespace quadmapper
