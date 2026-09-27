// The mesh-edit keyboard workflow (app/MeshEditKeys.hpp): the success-metric vertex
// nudge in mesh edit, and the grid-line handles of ADR-049 §3, through real commands
// and undo, as ViewportEditor drives them each frame.

#include <gtest/gtest.h>

#include "app/MeshEditKeys.hpp"
#include "core/CommandHistory.hpp"
#include "scene/Commands.hpp"
#include "scene/Scene.hpp"

#include <memory>
#include <vector>

namespace quadmapper {
namespace test {

class MeshEditKeysTest : public ::testing::Test {
protected:
    void SetUp() override {
        scenes_.push_back(std::make_unique<Scene>());
        quad_ = scenes_.front()->getQuadManager().createQuad();
        QuadGeometry& g = geometry();
        g.setCorners({Vec2(0.1f, 0.1f), Vec2(0.9f, 0.3f), Vec2(0.9f, 0.7f), Vec2(0.1f, 0.9f)});
        g.setUniformGrid(2);
        g.setMeshMode(MeshMode::MESH_EDIT);
    }

    QuadGeometry& geometry() { return quad_->getGeometry(); }
    QuadRef ref() { return QuadRef{&resolver_, scenes_.front()->getId(), quad_->getId()}; }

    // One frame with these keys.
    bool press(const CanvasKeys& keys) {
        return applyMeshEditKeys(keys, ref(), geometry(), history_, vertex_, handle_);
    }
    static CanvasKeys tab(bool shift = false) { CanvasKeys k; k.tab = true; k.shift = shift; return k; }
    static CanvasKeys h(bool shift = false) { CanvasKeys k; k.handle = true; k.shift = shift; return k; }
    static CanvasKeys right(bool shift = false, bool alt = false) {
        CanvasKeys k; k.right = true; k.shift = shift; k.alt = alt; return k;
    }
    static CanvasKeys up() { CanvasKeys k; k.up = true; return k; }

    std::vector<std::unique_ptr<Scene>> scenes_;
    SceneListResolver resolver_{scenes_};
    CommandHistory history_;
    Quad* quad_ = nullptr;
    int vertex_ = -1;
    int handle_ = -1;
};

TEST_F(MeshEditKeysTest, TabCyclesVerticesAndArrowsNudgeWithShiftAndAlt) {
    press(tab());
    press(tab());
    EXPECT_EQ(vertex_, 1);
    press(tab(/*shift=*/true));
    press(tab(/*shift=*/true));
    EXPECT_EQ(vertex_, geometry().getVertexCount() - 1);  // wraps backwards

    press(tab());  // vertex 0
    const Vec2 start = geometry().generateMeshPositions()[0];
    EXPECT_TRUE(press(right()));
    EXPECT_TRUE(press(right(/*shift=*/true)));
    EXPECT_TRUE(press(right(false, /*alt=*/true)));
    const Vec2 moved = geometry().generateMeshPositions()[0];
    EXPECT_NEAR(moved.x - start.x, 0.001f + 0.01f + 0.0001f, 1e-6f);
    EXPECT_NEAR(moved.y, start.y, 1e-6f);

    // The three nudges of one vertex are one undo step.
    ASSERT_TRUE(history_.undo());
    EXPECT_EQ(geometry().getMeshVertexByIndex(0), Vec2(0.0f));
    ASSERT_TRUE(history_.redo());
    EXPECT_NEAR(geometry().generateMeshPositions()[0].x, moved.x, 1e-6f);
}

TEST_F(MeshEditKeysTest, HCyclesTheSelectedVertexsHandlesAndTabDropsThem) {
    // Interior vertex 4 of the 3x3 grid has all four handles.
    vertex_ = 4;
    press(h());
    EXPECT_EQ(handle_, GRID_HANDLE_U_BEFORE);
    press(h());
    press(h());
    press(h());
    EXPECT_EQ(handle_, GRID_HANDLE_V_AFTER);
    press(h());
    EXPECT_EQ(handle_, -1) << "after the last handle, back to the vertex";
    press(h(/*shift=*/true));
    EXPECT_EQ(handle_, GRID_HANDLE_V_AFTER);

    press(tab());
    EXPECT_EQ(vertex_, 5);
    EXPECT_EQ(handle_, -1);

    // The top-left corner has only the handles pointing into the grid.
    vertex_ = 0;
    press(h());
    EXPECT_EQ(handle_, GRID_HANDLE_U_AFTER);
    press(h());
    EXPECT_EQ(handle_, GRID_HANDLE_V_AFTER);
    press(h());
    EXPECT_EQ(handle_, -1);

    // Without a selected vertex, H selects nothing.
    vertex_ = -1;
    press(h());
    EXPECT_EQ(handle_, -1);
}

TEST_F(MeshEditKeysTest, ArrowsNudgeTheSelectedHandleByTheVertexSteps) {
    vertex_ = 4;
    press(h());
    press(h());
    ASSERT_EQ(handle_, GRID_HANDLE_U_AFTER);
    const Vec2 vertexBefore = geometry().generateMeshPositions()[4];
    const Vec2 start = geometry().gridHandlePosition(4, GRID_HANDLE_U_AFTER);

    EXPECT_TRUE(press(right()));
    EXPECT_TRUE(press(right(/*shift=*/true)));
    EXPECT_TRUE(press(right(false, /*alt=*/true)));
    EXPECT_TRUE(press(up()));
    const Vec2 moved = geometry().gridHandlePosition(4, GRID_HANDLE_U_AFTER);
    EXPECT_NEAR(moved.x - start.x, 0.0111f, 1e-5f);
    EXPECT_NEAR(moved.y - start.y, -0.001f, 1e-5f);
    EXPECT_TRUE(geometry().getVertexTangent(4).isExplicit(GRID_HANDLE_U_AFTER));
    EXPECT_FALSE(geometry().getVertexTangent(4).isExplicit(GRID_HANDLE_U_BEFORE));
    // The vertex itself stays put.
    EXPECT_EQ(geometry().generateMeshPositions()[4], vertexBefore);

    // One undo step for the handle's nudges; redo reapplies them.
    ASSERT_TRUE(history_.undo());
    EXPECT_EQ(geometry().getVertexTangent(4), VertexTangents{});
    EXPECT_FALSE(geometry().hasMeshModifications());
    ASSERT_TRUE(history_.redo());
    const Vec2 redone = geometry().gridHandlePosition(4, GRID_HANDLE_U_AFTER);
    EXPECT_NEAR(redone.x, moved.x, 1e-6f);
    EXPECT_NEAR(redone.y, moved.y, 1e-6f);

    // Nudging another handle is a separate undo step.
    press(h());
    ASSERT_EQ(handle_, GRID_HANDLE_V_BEFORE);
    press(right());
    ASSERT_TRUE(history_.undo());
    EXPECT_FALSE(geometry().getVertexTangent(4).isExplicit(GRID_HANDLE_V_BEFORE));
    EXPECT_TRUE(geometry().getVertexTangent(4).isExplicit(GRID_HANDLE_U_AFTER));
}

TEST_F(MeshEditKeysTest, AStaleHandleSelectionFallsBackToTheVertex) {
    vertex_ = 4;
    handle_ = GRID_HANDLE_U_BEFORE;
    geometry().setUniformGrid(1);  // vertex 4 no longer exists
    EXPECT_FALSE(press(right()));
    EXPECT_EQ(handle_, -1);

    vertex_ = 1;  // top-right corner of the 1x1 grid: no U_AFTER handle
    handle_ = GRID_HANDLE_U_AFTER;
    EXPECT_TRUE(press(right()));  // nudges the vertex instead
    EXPECT_EQ(handle_, -1);
    EXPECT_NEAR(geometry().getMeshVertexByIndex(1).x, 0.001f, 1e-7f);
}

TEST_F(MeshEditKeysTest, KeysAfterTheQuadIsGoneAreSafe) {
    vertex_ = 4;
    press(h());
    press(right());
    scenes_.front()->getQuadManager().deleteQuad(quad_->getId());
    EXPECT_FALSE(history_.undo());
}

}  // namespace test
}  // namespace quadmapper
