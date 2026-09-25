#pragma once

#include "Scene.hpp"
#include "QuadManager.hpp"
#include "core/CommandHistory.hpp"
#include "core/UUID.hpp"

#include <memory>
#include <unordered_set>
#include <vector>

namespace quadmapper {

// Every media source a quad or 3D object in any scene is bound to, plus every source
// an undo or redo step could put back onto a quad. A source outside this set (and
// outside what the caller keeps alive for its own reasons, such as the media bin) can
// be released.
inline std::unordered_set<UUID> referencedMediaIds(
        const std::vector<std::unique_ptr<Scene>>& scenes, const CommandHistory& history) {
    std::unordered_set<UUID> ids;
    for (const auto& scene : scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            const UUID& mediaId = quad->getTexture().mediaId;
            if (!mediaId.isNull()) ids.insert(mediaId);
        }
        for (const auto& object : scene->getObjects()) {
            const UUID& mediaId = object->getTexture().mediaId;
            if (!mediaId.isNull()) ids.insert(mediaId);
        }
    }
    history.collectMediaIds(ids);
    return ids;
}

}  // namespace quadmapper
