#pragma once

#include "EditorState.hpp"
#include "scene/Scene.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Quad.hpp"
#include "source/SourceManager.hpp"
#include "renderer/MissingMediaLook.hpp"
#include "renderer/Texture.hpp"
#include "core/UUID.hpp"

#include <memory>
#include <unordered_map>

namespace quadmapper {

// The texture drawn for a missing source in this view (MissingMediaLook.hpp), created
// on first use and kept for the session. Null if it could not be created.
inline Texture* missingMediaTexture(EditorState& es, SurfaceView view) {
    const bool output = view == SurfaceView::Output;
    std::unique_ptr<Texture>& slot = output ? es.missingBlackTexture : es.missingPatternTexture;
    if (!slot) {
        auto texture = std::make_unique<Texture>();
        bool created = false;
        if (output) {
            const unsigned char black[4] = {0, 0, 0, 255};
            created = texture->create(1, 1, 4, black);
        } else {
            const auto pixels = generateMissingPatternRGBA();
            created = texture->create(MISSING_PATTERN_SIZE, MISSING_PATTERN_SIZE, 4, pixels.data());
            if (created) texture->setFilterMode(false);  // crisp checker edges
        }
        if (!created) return nullptr;
        slot = std::move(texture);
    }
    return slot.get();
}

// Build the quad-id -> Texture* map the Renderer consumes (ADR-040 P2). Each
// quad's image is resolved through its TextureBinding::mediaId via the
// SourceManager — the single media model the renderer reads. Anything without a
// live source falls back to the legacy quadTextures map (UV test grid + any
// not-yet-migrated assignment). A surface bound to a missing media file gets the
// missing look for `view`: black on projector outputs, a checker in the editor.
//
// Every scene's quads and 3D objects are included (the map is keyed by globally-unique id),
// so the editor viewport, the per-output render, and the preview FBOs can all
// share one builder and resolve inactive scenes too.
inline std::unordered_map<UUID, Texture*>
buildQuadTextureMap(EditorState& es, SourceManager* sm, SurfaceView view) {
    std::unordered_map<UUID, Texture*> map;
    auto resolve = [&](const UUID& mediaId, Texture* fallback) -> Texture* {
        Texture* tex = (sm && !mediaId.isNull()) ? sm->getTexture(mediaId) : nullptr;
        if (!tex) tex = fallback;
        const bool missing = !mediaId.isNull() && es.missingSources.count(mediaId) > 0;
        switch (chooseSurfaceDraw(tex != nullptr, missing, view)) {
            case SurfaceDraw::Texture:        return tex;
            case SurfaceDraw::MissingPattern:
            case SurfaceDraw::Black:          return missingMediaTexture(es, view);
            case SurfaceDraw::Plain:          break;
        }
        return nullptr;
    };
    for (const auto& scenePtr : es.scenes) {
        if (!scenePtr) continue;
        for (const auto& quadPtr : scenePtr->getQuadManager().getQuads()) {
            if (!quadPtr) continue;
            auto legacy = es.quadTextures.find(quadPtr->getId());
            Texture* fallback = legacy != es.quadTextures.end() ? legacy->second.get() : nullptr;
            if (Texture* tex = resolve(quadPtr->getTexture().mediaId, fallback)) {
                map[quadPtr->getId()] = tex;
            }
        }
        // 3D objects (ADR-052) resolve through the same media model, keyed by object id.
        for (const auto& objectPtr : scenePtr->getObjects()) {
            if (!objectPtr) continue;
            if (Texture* tex = resolve(objectPtr->getTexture().mediaId, nullptr)) {
                map[objectPtr->getId()] = tex;
            }
        }
    }
    return map;
}

}  // namespace quadmapper
