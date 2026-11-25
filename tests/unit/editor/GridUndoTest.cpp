// Grid undo (editor/GridUndo.hpp) through the id-based command, on a scene list as the
// editor keeps it. Moved from scene/CommandsTest.cpp (ADR-051 E4).

#include "editor/GridUndo.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace quadmapper;

TEST(GridUndoTest, GridChangeUndoRestoresGridAndIsSafeAfterDelete) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    SceneListResolver resolver{scenes};
    CommandHistory history;
    Scene& scene = *scenes.front();
    Quad* quad = scene.getQuadManager().createQuad();
    const QuadRef ref{&resolver, scene.getId(), quad->getId()};

    QuadGeometry& geo = quad->getGeometry();
    geo.setGridHandlePosition(1, GRID_HANDLE_V_AFTER, Vec2(0.75f, 0.4f));  // a grid-line handle
    GridSnapshot before = captureGrid(geo);
    geo.insertUDivision(0.3f);
    pushGridChange(history, ref, "Add Column", before, captureGrid(geo));
    const int columnsAfter = geo.getColumns();

    history.undo();
    EXPECT_EQ(geo.getUCoords(), before.u);
    EXPECT_EQ(geo.getMeshVertices(), before.offsets);
    EXPECT_EQ(geo.getVertexTangents(), before.tangents);
    EXPECT_TRUE(geo.getVertexTangent(1).isExplicit(GRID_HANDLE_V_AFTER));

    history.redo();
    EXPECT_EQ(geo.getColumns(), columnsAfter);

    scene.getQuadManager().deleteQuad(quad->getId());
    EXPECT_FALSE(history.undo());
}
