#pragma once

// Undo support for control-grid edits — add/remove division lines and the even-grid
// reset (ADR-043). These change the whole grid (coords + offsets), so we snapshot the
// grid before and after and replay via setGridCoords + setMeshVertices. Reuses the
// generic SetPropertyCommand; the captured Quad* stays valid because Application clears
// the history whenever a quad is deleted.

#include "core/CommandHistory.hpp"
#include "core/Types.hpp"
#include "scene/Commands.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGeometry.hpp"

#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

struct GridSnapshot {
    std::vector<float> u;
    std::vector<float> v;
    std::vector<Vec2> offsets;
};

inline GridSnapshot captureGrid(const QuadGeometry& g) {
    return GridSnapshot{ g.getUCoords(), g.getVCoords(), g.getMeshVertices() };
}

inline void applyGridSnapshot(Quad* quad, const GridSnapshot& s) {
    if (!quad) return;
    quad->getGeometry().setGridCoords(s.u, s.v);
    quad->getGeometry().setMeshVertices(s.offsets);
}

// Record a completed grid change (already applied to the quad) as one undo step.
inline void pushGridChange(CommandHistory& hist, Quad* quad, const std::string& desc,
                           const GridSnapshot& before, const GridSnapshot& after) {
    hist.execute(std::make_unique<SetPropertyCommand<GridSnapshot>>(
        desc, before, after,
        [quad](const GridSnapshot& s) { applyGridSnapshot(quad, s); }));
}

}  // namespace quadmapper
