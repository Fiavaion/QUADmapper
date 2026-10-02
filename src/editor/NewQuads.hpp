#pragma once

#include "EditorState.hpp"
#include "core/UUID.hpp"
#include "renderer/UvTestGrid.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"

namespace quadmapper {

// Once a frame, before the sources update (Application::render; the perform lock sweep
// runs it too, TD-36): a fresh session gets its first quad, selected; and with the UV
// test grid toggle on (#2), each quad not seen before gets the coloured UV reference
// grid. Running every frame covers every path that adds a quad (default, new project,
// the Quad List "+"); quads that exist when the toggle is turned on are only marked
// seen, so it affects later quads. Main thread (the grid is a GL texture).
inline void prepareNewQuads(EditorState& es) {
    QuadManager& quadManager = es.quadManager();
    if (!es.initialized) {
        quadManager.createQuad("Quad 1");
        if (quadManager.getQuadCount() > 0) {
            quadManager.selectQuad(quadManager.getQuads()[0]->getId());
        }
        es.initialized = true;
    }
    for (const auto& qptr : quadManager.getQuads()) {
        const UUID& id = qptr->getId();
        if (es.seenQuadIds.insert(id).second && es.uvGridOnNewQuads) {
            assignUvTestGrid(es.quadTextures, es.quadTexturePaths, id, 1024, 8);
        }
    }
}

}  // namespace quadmapper
