#pragma once

// Slice editing (ADR-047 part B, QM-121): undoable edits of an output's slices, the
// canvas keyboard for a slice's warp corners, auto-blend and the scene canvas size.
// Kept apart from the panels and ViewportEditor so the workflow, undo included, runs
// in tests without a window.

#include "EditorState.hpp"
#include "input/VertexKeyboard.hpp"
#include "scene/Commands.hpp"
#include "scene/Slice.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quadmapper {

// Index of the slice with this id, or -1.
inline int findSliceIndex(const std::vector<Slice>& slices, const UUID& id) {
    for (size_t i = 0; i < slices.size(); ++i) {
        if (slices[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// The output whose slice warp the canvas edits: set by EditorState::beginSliceWarp,
// while that output and its selected slice exist. Warp editing whose output or slice
// is gone (deleted, undone, a project loaded) is ended here, so the canvas goes back
// to the quads.
inline OutputWindow* sliceWarpOutput(EditorState& es, OutputManager* outputs) {
    if (es.sliceWarpOutputId.isNull()) return nullptr;
    OutputWindow* output = outputs ? outputs->getOutput(es.sliceWarpOutputId) : nullptr;
    if (!output || findSliceIndex(output->getSlices(), es.selectedSliceId) < 0) {
        es.endSliceWarp();
        return nullptr;
    }
    return output;
}

// What the canvas editing keys (Tab, the arrows, H) act on this frame. Calibration
// comes first, then slice warp editing, which the operator turns on explicitly in the
// slice editor; otherwise the selected quad, exactly as without slices. Selecting a
// slice in the slice editor alone never takes the keys from a quad.
enum class CanvasKeyOwner { None, Quad, Calibration, SliceWarp };

inline CanvasKeyOwner canvasKeyOwner(EditorState& es, OutputManager* outputs) {
    if (!es.calibratingOutputId.isNull()) {
        const OutputWindow* calibrated = outputs ? outputs->getOutput(es.calibratingOutputId) : nullptr;
        const bool points = calibrated && calibrated->getSceneId() == es.activeScene().getId() &&
                            !calibrated->getCalibrationPoints().empty();
        return points ? CanvasKeyOwner::Calibration : CanvasKeyOwner::None;
    }
    if (sliceWarpOutput(es, outputs)) return CanvasKeyOwner::SliceWarp;
    return es.quadManager().getPrimarySelectedQuad() ? CanvasKeyOwner::Quad : CanvasKeyOwner::None;
}

// Record one edit of `output`'s slices as an undo step: `edit` changes a copy of the
// slice list and the command applies it. Edits with the same non-empty mergeKey on
// one output merge into one step (a drag, a run of nudges on one corner).
template <typename Edit>
void editSlices(EditorState& es, OutputWindow& output, const std::string& description,
                const std::string& mergeKey, Edit&& edit) {
    OutputSlices before{output.getSlices(), output.getSlicesCanvas()};
    OutputSlices after = before;
    edit(after.slices);
    es.commandHistory.execute(std::make_unique<SliceEditCommand>(
        es.outputRef(output.getId()), description, mergeKey, std::move(before), std::move(after)));
}

// A slice input rectangle as typed into the slice editor, kept on `canvas`: x and y in
// 0..size - 1, width and height at least 1 and no further than the canvas's far edge.
inline CanvasRect clampInputToCanvas(const CanvasRect& input, IVec2 canvas) {
    const float w = static_cast<float>(std::max(canvas.x, 1));
    const float h = static_cast<float>(std::max(canvas.y, 1));
    CanvasRect r;
    r.x = std::clamp(input.x, 0.0f, w - 1.0f);
    r.y = std::clamp(input.y, 0.0f, h - 1.0f);
    r.width = std::clamp(input.width, 1.0f, w - r.x);
    r.height = std::clamp(input.height, 1.0f, h - r.y);
    return r;
}

// One frame of canvas keys on the edited slice's warp, the quad corner model
// (VertexKeyboard.hpp): Tab / Shift+Tab cycles the four corners (TL, TR, BR, BL), the
// arrows nudge the selected corner by nudgeStep (Shift 10x, Alt 0.1x) in the output's
// normalized space. Each nudge is an undoable SliceEditCommand; nudges of one corner
// merge. Returns true when the warp changed.
inline bool applySliceWarpKeys(const CanvasKeys& keys, EditorState& es, OutputWindow& output) {
    const int index = findSliceIndex(output.getSlices(), es.selectedSliceId);
    if (index < 0) return false;
    int& corner = es.sliceWarpSelectedCorner;
    if (keys.tab) corner = cycleVertex(corner, 4, keys.shift);
    if (corner < 0 || corner >= 4 || !keys.arrow()) return false;
    const int c = corner;
    const Vec2 delta = keys.nudge();
    editSlices(es, output, "Move slice warp corner", sliceWarpCornerKey(es.selectedSliceId, c),
               [&](std::vector<Slice>& slices) {
                   QuadGeometry& warp = slices[static_cast<size_t>(index)].warp;
                   warp.setCorner(c, warp.getCorners()[static_cast<size_t>(c)] + delta);
               });
    return true;
}

// Auto-blend (ADR-047 §3): every facing edge of overlapping slices of different
// outputs that show `sceneId` gets the overlap's width, gamma 2.2 and the default
// curve (autoBlendEdges); other edges keep their values. One undo step. Returns how
// many edges were set: 0 (and no step) when no two outputs' slices overlap. The output
// being calibrated takes no part: calibration needs its default slicing (canCalibrate).
inline int autoBlendScene(EditorState& es, OutputManager& outputs, const UUID& sceneId) {
    std::vector<OutputWindow*> shown;
    std::vector<SlicePlacement> placements;
    std::vector<std::pair<size_t, size_t>> where;  // placement -> (output, slice)
    for (const auto& output : outputs.getOutputs()) {
        if (!output || sceneId.isNull() || output->getSceneId() != sceneId) continue;
        if (output->getId() == es.calibratingOutputId) continue;
        for (size_t s = 0; s < output->getSlices().size(); ++s) {
            placements.push_back(SlicePlacement{output->getId(), output->getSlices()[s].input});
            where.emplace_back(shown.size(), s);
        }
        shown.push_back(output.get());
    }
    const std::vector<AutoBlendEdge> edges = autoBlendEdges(placements);
    if (edges.empty()) return 0;

    std::vector<std::vector<Slice>> after;
    for (OutputWindow* output : shown) after.push_back(output->getSlices());
    std::vector<bool> touched(shown.size(), false);
    for (const AutoBlendEdge& edge : edges) {
        const auto [o, s] = where[edge.slice];
        BlendEdge blend;  // gamma 2.2, default curve
        blend.width = edge.width;
        after[o][s].blend[static_cast<size_t>(edge.edge)] = blend;
        touched[o] = true;
    }
    es.commandHistory.beginCompoundCommand("Auto-blend");
    for (size_t o = 0; o < shown.size(); ++o) {
        if (!touched[o]) continue;
        editSlices(es, *shown[o], "Auto-blend", "",
                   [&](std::vector<Slice>& slices) { slices = after[o]; });
    }
    es.commandHistory.endCompoundCommand();
    return static_cast<int>(edges.size());
}

// Set a scene's canvas size (clamped to 1..MAX_CANVAS_DIMENSION, as on load) and
// whether it is fixed (Scene::isCanvasLocked), as one undo step. Quads keep their
// normalized positions; the slices of every output showing the scene are rescaled to
// the new size (rescaleSlices, through es.outputResolver), so they keep their place on
// the picture, and so are those of the outputs showing it when the step is undone or
// redone. Returns false (no step) when nothing changes or there is no such scene.
inline bool setSceneCanvas(EditorState& es, const UUID& sceneId, IVec2 size, bool locked) {
    const int index = findSceneIndex(es.scenes, sceneId);
    if (index < 0) return false;
    Scene& scene = *es.scenes[static_cast<size_t>(index)];
    const CanvasSetting before{scene.getCanvasSize(), scene.isCanvasLocked()};
    const CanvasSetting after{IVec2(std::clamp(size.x, 1, MAX_CANVAS_DIMENSION),
                                    std::clamp(size.y, 1, MAX_CANVAS_DIMENSION)),
                              locked};
    if (after.size == before.size && after.locked == before.locked) return false;

    es.commandHistory.execute(std::make_unique<SetSceneCanvasCommand>(
        es.quadResolver, &es.outputResolver, sceneId, before, after));
    return true;
}

}  // namespace quadmapper
