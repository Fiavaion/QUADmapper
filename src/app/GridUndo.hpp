#pragma once

// Undo support for control-grid edits — add/remove division lines and the even-grid
// reset (ADR-043). These change the whole grid (coords + offsets), so we snapshot the
// grid before and after and replay via setGridCoords + setMeshVertices. Reuses the
// id-keyed QuadPropertyCommand, so a deleted quad makes the step a safe no-op.

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

inline void applyGridSnapshot(Quad& quad, const GridSnapshot& s) {
    quad.getGeometry().setGridCoords(s.u, s.v);
    quad.getGeometry().setMeshVertices(s.offsets);
}

// Record a completed grid change (already applied to the quad) as one undo step.
inline void pushGridChange(CommandHistory& hist, const QuadRef& target, const std::string& desc,
                           const GridSnapshot& before, const GridSnapshot& after) {
    hist.execute(std::make_unique<QuadPropertyCommand<GridSnapshot>>(
        desc, target, before, after, applyGridSnapshot));
}

}  // namespace quadmapper
