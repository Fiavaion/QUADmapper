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
#include "source/SourceManager.hpp"
#include "source/VideoFileSource.hpp"
#include "scene/Quad.hpp"
#include "project/Project.hpp"

#include <string>

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

    // A paused video uploads no frames, so the quad would sample an empty (black)
    // texture. Start playback so the assignment is visible immediately (loop default).
    if (isVideo) {
        if (auto* vs = sm->getSourceAs<VideoFileSource>(sid)) vs->play();
    }
    return sid;
}

}  // namespace quadmapper
