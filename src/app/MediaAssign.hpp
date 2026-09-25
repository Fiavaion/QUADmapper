#pragma once

// Centralized media -> quad binding (ADR-042 P2). The pattern of "resolve a source,
// point the quad's TextureBinding::mediaId at it, record the path for .qmap, drop any
// legacy per-quad texture, mark modified" was duplicated across the Inspector, the
// Video Player and the drag-drop targets — and the Inspector copy wrongly created an
// IMAGE source even for videos. This single point fixes that and is reused everywhere.

#include "EditorState.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"          // MediaType
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "source/SourceManager.hpp"
#include "scene/Quad.hpp"
#include "project/Project.hpp"

#include <string>
#include <unordered_set>

namespace quadmapper {

// Core: bind an already-resolved source (by UUID) to a quad. path may be empty
// (procedural). isVideo makes the source the Video Player's active source.
inline void bindSourceToQuad(EditorState& es, Project* project, Quad& quad,
                             const UUID& sourceId, const std::string& path, bool isVideo) {
    quad.getTexture().mediaId = sourceId;
    es.quadTextures.erase(quad.getId());          // drop any legacy per-quad texture
    if (!path.empty()) es.quadTexturePaths[quad.getId()] = path;
    else               es.quadTexturePaths.erase(quad.getId());
    if (isVideo) es.activeVideoSourceId = sourceId;
    if (project) project->setModified(true);
}

// Resolve a MediaItem to a (deduped) SourceManager source by type, then bind it to the
// quad. Returns the bound source UUID, or null on failure / unsupported type (audio).
inline UUID assignMediaToQuad(EditorState& es, SourceManager* sm, Project* project,
                              Quad& quad, const MediaItem& item) {
    if (!sm) return UUID();

    UUID sid;
    bool isVideo = false;
    switch (item.getType()) {
        case MediaType::IMAGE:
            sid = sm->getOrCreateImageSource(item.getPath());
            break;
        case MediaType::VIDEO:
            sid = sm->getOrCreateVideoSource(item.getPath());
            isVideo = true;
            break;
        case MediaType::PROCEDURAL:
            sid = item.getSourceId();  // procedural sources are pre-registered in the bin
            break;
        default:
            return UUID();             // audio / unknown: nothing to texture
    }
    if (sid.isNull()) return UUID();

    bindSourceToQuad(es, project, quad, sid,
                     item.hasPath() ? item.getPath().string() : std::string(), isVideo);

    // Start playback so the assignment is visible immediately (loop default).
    // Any playable source — FFmpeg-decoded or HAP — plays the same way.
    if (isVideo) {
        if (auto* playable = sm->getSourceAs<PlayableSource>(sid)) playable->play();
    }
    return sid;
}

// Frame-boundary source housekeeping (QM-088): release file-backed sources that no
// quad in any scene uses and that are not the Video Player's or the media bin's
// preview source, so orphaned videos stop decoding and uploading.
//
// Undo can bring back a deleted quad whose source was released meanwhile. Such a
// binding is re-resolved from the quad's saved media path (videos restart playing);
// if that fails the binding is cleared so it is not retried every frame.
inline void releaseUnusedSources(EditorState& es, SourceManager& sm) {
    std::unordered_set<UUID> inUse;
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            UUID& mediaId = quad->getTexture().mediaId;
            if (mediaId.isNull()) continue;
            if (!sm.hasSource(mediaId)) {
                auto pathIt = es.quadTexturePaths.find(quad->getId());
                UUID healed;
                if (pathIt != es.quadTexturePaths.end() && !pathIt->second.empty()) {
                    const std::filesystem::path path = pathIt->second;
                    if (MediaLibrary::getTypeFromExtension(path.extension().string()) == MediaType::VIDEO) {
                        healed = sm.getOrCreateVideoSource(path);
                        if (auto* playable = sm.getSourceAs<PlayableSource>(healed)) playable->play();
                    } else {
                        healed = sm.getOrCreateImageSource(path);
                    }
                }
                mediaId = healed;
                if (mediaId.isNull()) continue;
            }
            inUse.insert(mediaId);
        }
    }
    inUse.insert(es.activeVideoSourceId);
    inUse.insert(es.mediaBinPanel.getPreviewSourceId());
    // Sources the media bin offers (its procedural generators) stay available to assign.
    if (const MediaLibrary* library = es.mediaBinPanel.getMediaLibrary()) {
        for (const auto& item : library->getItems()) {
            if (item && item->hasSource()) inUse.insert(item->getSourceId());
        }
    }
    sm.releaseUnreferenced(inUse);
}

}  // namespace quadmapper
