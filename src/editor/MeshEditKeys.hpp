#pragma once

// The corner-pin and mesh-edit keyboard, the success-metric vertex UX (CLAUDE.md) plus
// the grid-line handles of ADR-049 §3. Kept apart from ViewportEditor so the whole key
// workflow, including undo, runs in tests without a window.

#include "core/CommandHistory.hpp"
#include "core/Types.hpp"
#include "input/VertexKeyboard.hpp"
#include "scene/Commands.hpp"
#include "scene/QuadGeometry.hpp"

#include <memory>

namespace quadmapper {

// One frame of canvas keys in corner pin (CORNERS_ONLY): Tab / Shift+Tab cycles the
// four corners (TL, TR, BR, BL), the arrows nudge the selected corner by nudgeStep
// (Shift 10x, Alt 0.1x). Each nudge is a MoveCornerCommand, which applies it;
// consecutive nudges of one corner merge into one undo step. A `locked` quad (the
// Quad List's lock) still cycles but never moves. Returns true when the geometry
// changed.
inline bool applyCornerKeys(const CanvasKeys& keys, const QuadRef& target,
                            const QuadGeometry& geometry, CommandHistory& history, int& corner,
                            bool locked = false) {
    if (keys.tab) {
        corner = cycleVertex(corner, 4, keys.shift);
    }
    if (corner < 0 || corner >= 4 || !keys.arrow() || locked) {
        return false;
    }
    const Vec2 oldPos = geometry.getCorners()[static_cast<size_t>(corner)];
    history.execute(std::make_unique<MoveCornerCommand>(target, corner, oldPos, oldPos + keys.nudge()));
    return true;
}

// One frame of canvas keys in mesh edit:
// - Tab / Shift+Tab cycles the control vertices and drops the handle selection;
// - H / Shift+H cycles the selected vertex's handles, then back to the vertex;
// - the arrows nudge the selected handle, else the selected vertex, by nudgeStep
//   (Shift 10x, Alt 0.1x).
// Each nudge is an id-keyed undoable command; consecutive nudges of one vertex, or of
// one handle, merge into one undo step. A `locked` quad still cycles but never moves.
// Returns true when the geometry changed.
inline bool applyMeshEditKeys(const CanvasKeys& keys, const QuadRef& target,
                              QuadGeometry& geometry, CommandHistory& history, int& vertex,
                              int& handle, bool locked = false) {
    const int count = geometry.getVertexCount();
    if (keys.tab) {
        vertex = cycleVertex(vertex, count, keys.shift);
        handle = -1;
    }
    if (vertex < 0 || vertex >= count) {
        handle = -1;
        return false;
    }
    const unsigned available = geometry.availableHandles(vertex);
    if (keys.handle) {
        handle = cycleHandle(handle, available, keys.shift);
    }
    if (handle >= 0 && !((available >> handle) & 1u)) {
        handle = -1;  // the grid changed under a stale selection
    }
    if (!keys.arrow() || locked) {
        return false;
    }

    if (handle >= 0) {
        const VertexTangents before = geometry.getVertexTangent(vertex);
        geometry.setGridHandlePosition(vertex, handle,
                                       geometry.gridHandlePosition(vertex, handle) + keys.nudge());
        history.execute(std::make_unique<MoveGridHandleCommand>(
            target, vertex, handle, before, geometry.getVertexTangent(vertex)));
    } else {
        const Vec2 oldOffset = geometry.getMeshVertexByIndex(vertex);
        history.execute(std::make_unique<MoveMeshVertexCommand>(
            target, vertex, oldOffset, oldOffset + keys.nudge()));
    }
    return true;
}

}  // namespace quadmapper
