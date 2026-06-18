#pragma once

#include "EditorState.hpp"
#include "scene/Scene.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Quad.hpp"
#include "source/SourceManager.hpp"
#include "renderer/Texture.hpp"
#include "core/UUID.hpp"

#include <unordered_map>

namespace quadmapper {

// Build the quad-id -> Texture* map the Renderer consumes (ADR-040 P2). Each
// quad's image is resolved through its TextureBinding::mediaId via the
// SourceManager — the single media model the renderer reads. Anything without a
// live source falls back to the legacy quadTextures map (UV test grid + any
// not-yet-migrated assignment). The single legacy video is overlaid onto its
// assigned quad (folded into a SourceManager source in P3).
//
// Every scene's quads are included (the map is keyed by globally-unique quad id),
// so the editor viewport, the per-output render, and the preview FBOs can all
// share one builder and resolve inactive scenes too.
inline std::unordered_map<UUID, Texture*>
buildQuadTextureMap(EditorState& es, SourceManager* sm) {
    std::unordered_map<UUID, Texture*> map;
    for (const auto& scenePtr : es.scenes) {
        if (!scenePtr) continue;
        for (const auto& quadPtr : scenePtr->getQuadManager().getQuads()) {
            if (!quadPtr) continue;
            const UUID& qid = quadPtr->getId();
            const UUID& mid = quadPtr->getTexture().mediaId;
            Texture* tex = nullptr;
            if (sm && !mid.isNull()) {
                tex = sm->getTexture(mid);
            }
            if (!tex) {
                auto it = es.quadTextures.find(qid);
                if (it != es.quadTextures.end()) tex = it->second.get();
            }
            if (tex) map[qid] = tex;
        }
    }
    // Legacy single-video override (P3 folds this into a SourceManager source).
    if (!es.videoAssignedQuadId.isNull() && es.videoTextureId != 0) {
        es.videoTexWrapper.setGLTextureId(es.videoTextureId, es.videoTextureWidth,
                                          es.videoTextureHeight);
        map[es.videoAssignedQuadId] = &es.videoTexWrapper;
    }
    return map;
}

}  // namespace quadmapper
