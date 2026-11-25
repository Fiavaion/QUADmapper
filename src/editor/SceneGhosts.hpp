#pragma once

// Other scenes' quads on the canvas (owner, 2026-11-23): with "Other scenes" on, the
// canvas outlines every visible quad of the other scenes and a dragged corner or mesh
// point snaps to their corners and points too, so two projectors stacked on one
// surface can be mapped onto the same edges.
//
// Each scene is in its own canvas space (normalized [0,1] of its canvas). A ghost is
// placed by pixel: a point at pixel (x, y) of its scene's canvas lands at pixel (x, y)
// of the active canvas, top-left aligned. For projectors of the same resolution that is
// the same normalized position. Everything a quad's shape is made of (corners, mesh
// residuals, curve offsets, handle derivatives) is a position or a derivative of one,
// so scaling them all by the canvas ratio scales the drawn surface exactly.

#include "EditorState.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGeometry.hpp"
#include "scene/Scene.hpp"

#include <vector>

namespace quadmapper {

// `g` with every position scaled component-wise by `scale`.
inline QuadGeometry scaledGeometry(const QuadGeometry& g, const Vec2& scale) {
    QuadGeometry out = g;
    std::array<Vec2, 4> corners = g.getCorners();
    for (Vec2& c : corners) c *= scale;
    out.setCorners(corners);
    std::array<Vec2, 8> offsets = g.getBezierOffsets();
    for (Vec2& o : offsets) o *= scale;
    out.setBezierOffsets(offsets);
    std::vector<Vec2> residuals = g.getMeshVertices();
    for (Vec2& r : residuals) r *= scale;
    out.setMeshVertices(residuals);
    std::vector<VertexTangents> tangents = g.getVertexTangents();
    for (VertexTangents& t : tangents) {
        for (Vec2& d : t.derivative) d *= scale;
    }
    out.setVertexTangents(tangents);
    return out;
}

// The visible quads of every scene but the active one, moved into the active canvas.
// A ghost keeps its quad's id and name; its geometry is in active-canvas space.
inline std::vector<Quad> otherSceneGhosts(EditorState& es) {
    std::vector<Quad> ghosts;
    const Scene& active = es.activeScene();
    const IVec2 to = active.getCanvasSize();
    if (to.x <= 0 || to.y <= 0) return ghosts;
    for (const auto& scene : es.scenes) {
        if (!scene || scene.get() == &active) continue;
        const IVec2 from = scene->getCanvasSize();
        if (from.x <= 0 || from.y <= 0) continue;
        const Vec2 scale(static_cast<float>(from.x) / static_cast<float>(to.x),
                         static_cast<float>(from.y) / static_cast<float>(to.y));
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (!quad || !quad->isVisible()) continue;
            Quad ghost = *quad;
            ghost.getGeometry() = scaledGeometry(quad->getGeometry(), scale);
            ghosts.push_back(std::move(ghost));
        }
    }
    return ghosts;
}

// The points a dragged vertex snaps to on a quad: its corners, and its mesh points
// while it is in mesh edit (the same set as for quads of the active scene).
inline std::vector<Vec2> snapPointsOf(const Quad& quad) {
    const QuadGeometry& g = quad.getGeometry();
    std::vector<Vec2> points(g.getCorners().begin(), g.getCorners().end());
    if (g.getMeshMode() == MeshMode::MESH_EDIT) {
        const std::vector<Vec2> mesh = g.generateMeshPositions();
        points.insert(points.end(), mesh.begin(), mesh.end());
    }
    return points;
}

}  // namespace quadmapper
