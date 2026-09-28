#pragma once

// What selecting a quad shows in the Properties panel: Quad Properties, whether the
// quad was picked in the Quad List or on the canvas, and a texture Path field that
// names the quad's current media.

#include "EditorState.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"

#include <cstdio>
#include <string>

namespace quadmapper {

// Show Quad Properties and clear the media bin and output selections.
inline void showQuadProperties(EditorState& es) {
    es.currentSelectionType = EditorState::SelectionType::Quad;
    es.selectedMediaId = UUID();
    es.selectedOutputId = UUID();
}

// A fresh left-press on the canvas at `point` (ADR-042). Unless it is on the selected
// quad's own handle, it picks the topmost quad under the cursor. A press on a quad,
// the selected one included, or on the selected quad's handle shows Quad Properties,
// as a click in the Quad List does. True when the primary selection changed, so the
// caller drops the previous quad's vertex selection; drags and vertex selection are
// the caller's and are not touched here.
inline bool pickQuadOnCanvas(EditorState& es, QuadManager& quads, Vec2 point, bool onSelectedHandle) {
    Quad* hit = onSelectedHandle ? nullptr : quads.quadAtPoint(point);
    if (!onSelectedHandle && !hit) {
        return false;  // empty canvas: the selection and the panel stay
    }
    showQuadProperties(es);
    const Quad* selected = quads.getPrimarySelectedQuad();
    if (!hit || (selected && hit->getId() == selected->getId())) {
        return false;
    }
    quads.selectQuad(hit->getId());
    return true;
}

// The Properties panel's texture Path field (es.currentTexturePath) follows the shown
// quad's media: whenever the quad shown, or the path recorded for it
// (es.quadTexturePaths), differs from what the field last took (`shownQuad`,
// `shownPath`), the field is refilled with that path, or cleared. Every way media
// changes (the bin, the Video Player, Load, Clear, a relocation, a project load)
// records the path, so none of them can leave the field naming older media; typing
// in the field changes neither, so a path being typed stays.
inline void followTexturePath(EditorState& es, const Quad* quad, UUID& shownQuad, std::string& shownPath) {
    const UUID id = quad ? quad->getId() : UUID();
    const auto it = quad ? es.quadTexturePaths.find(id) : es.quadTexturePaths.end();
    const std::string path = it != es.quadTexturePaths.end() ? it->second : std::string();
    if (id == shownQuad && path == shownPath) {
        return;
    }
    shownQuad = id;
    shownPath = path;
    std::snprintf(es.currentTexturePath, sizeof(es.currentTexturePath), "%s", path.c_str());
}

}  // namespace quadmapper
