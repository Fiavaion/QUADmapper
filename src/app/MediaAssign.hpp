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
#include "scene/MeshObject.hpp"
#include "scene/MediaReferences.hpp"
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

// Resolve a MediaItem to a (deduped) SourceManager source by type. Null on failure or
// for a type that cannot texture anything (audio).
inline UUID resolveMediaSource(SourceManager& sm, const MediaItem& item, bool& isVideo) {
    isVideo = false;
    switch (item.getType()) {
        case MediaType::IMAGE:
            return sm.getOrCreateImageSource(item.getPath());
        case MediaType::VIDEO:
            isVideo = true;
            return sm.getOrCreateVideoSource(item.getPath());
        case MediaType::PROCEDURAL:
            return item.getSourceId();  // procedural sources are pre-registered in the bin
        default:
            return UUID();              // audio / unknown: nothing to texture
    }
}

// Start playback so an assignment is visible immediately (loop default). Any
// playable source, FFmpeg-decoded or HAP, plays the same way.
inline void playAssignedVideo(SourceManager& sm, const UUID& sid) {
    if (auto* playable = sm.getSourceAs<PlayableSource>(sid)) playable->play();
}

// Resolve a MediaItem to a source, then bind it to the quad. Returns the bound
// source UUID, or null on failure / unsupported type (audio).
inline UUID assignMediaToQuad(EditorState& es, SourceManager* sm, Project* project,
                              Quad& quad, const MediaItem& item) {
    if (!sm) return UUID();
    bool isVideo = false;
    const UUID sid = resolveMediaSource(*sm, item, isVideo);
    if (sid.isNull()) return UUID();

    bindSourceToQuad(es, project, quad, sid,
                     item.hasPath() ? item.getPath().string() : std::string(), isVideo);
    if (isVideo) playAssignedVideo(*sm, sid);
    return sid;
}

// The same for a 3D object (ADR-052). Objects have no legacy texturePath: the
// "sources" table alone persists the binding.
inline UUID assignMediaToObject(EditorState& es, SourceManager* sm, Project* project,
                                MeshObject& object, const MediaItem& item) {
    if (!sm) return UUID();
    bool isVideo = false;
    const UUID sid = resolveMediaSource(*sm, item, isVideo);
    if (sid.isNull()) return UUID();

    object.getTexture().mediaId = sid;
    if (isVideo) {
        es.activeVideoSourceId = sid;
        playAssignedVideo(*sm, sid);
    }
    if (project) project->setModified(true);
    return sid;
}

// Sources the editor keeps alive whatever the project references: the Video
// Player's source, the media bin's hover preview, and every source the bin offers
// (its procedural generators), so bin items stay assignable.
inline std::unordered_set<UUID> editorOwnedSources(const EditorState& es) {
    std::unordered_set<UUID> ids{es.activeVideoSourceId, es.mediaBinPanel.getPreviewSourceId()};
    if (const MediaLibrary* library = es.mediaBinPanel.getMediaLibrary()) {
        for (const auto& item : library->getItems()) {
            if (item && item->hasSource()) ids.insert(item->getSourceId());
        }
    }
    return ids;
}

// Frame-boundary source housekeeping (QM-088): release sources that no quad in any
// scene uses, that no undo/redo step can restore, and that the editor does not keep
// alive, so orphaned videos and generators stop decoding, rendering and uploading.
//
// A quad whose source is missing (a binding restored from an older history, for
// example) is re-resolved from its saved media path (videos restart playing); if that
// fails the binding is cleared so it is not retried every frame.
inline void releaseUnusedSources(EditorState& es, SourceManager& sm) {
    std::unordered_set<UUID> onScreen;  // bound to a quad that exists right now
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            UUID& mediaId = quad->getTexture().mediaId;
            if (!mediaId.isNull() && sm.hasSource(mediaId)) onScreen.insert(mediaId);
            if (mediaId.isNull() || sm.hasSource(mediaId)) continue;
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
            if (!mediaId.isNull()) onScreen.insert(mediaId);
        }
        for (const auto& object : scene->getObjects()) {
            const UUID& mediaId = object->getTexture().mediaId;
            if (!mediaId.isNull() && sm.hasSource(mediaId)) onScreen.insert(mediaId);
        }
    }
    std::unordered_set<UUID> inUse = referencedMediaIds(es.scenes, es.commandHistory);
    const std::unordered_set<UUID> owned = editorOwnedSources(es);
    inUse.insert(owned.begin(), owned.end());
    sm.releaseUnreferenced(inUse);

    // A video kept only so undo can restore its deleted quad should not keep decoding
    // and uploading unseen; pause it, and resume it once undo brings it back.
    for (auto it = es.sourcesPausedWhileUnseen.begin(); it != es.sourcesPausedWhileUnseen.end();) {
        auto* playable = sm.getSourceAs<PlayableSource>(*it);
        if (!playable) {
            it = es.sourcesPausedWhileUnseen.erase(it);  // released meanwhile
        } else if (onScreen.count(*it)) {
            playable->play();
            it = es.sourcesPausedWhileUnseen.erase(it);
        } else {
            ++it;
        }
    }
    for (const UUID& id : inUse) {
        if (onScreen.count(id) || owned.count(id)) continue;
        auto* playable = sm.getSourceAs<PlayableSource>(id);
        if (playable && playable->getPlaybackState() == PlaybackState::PLAYING) {
            playable->pause();
            es.sourcesPausedWhileUnseen.insert(id);
        }
    }
}

// Called when the whole project is replaced (New, Load, recovery), after the old
// scenes and undo history are gone and before the new project's sources are created:
// releases the previous project's sources right away, so a reloaded show starts its
// media fresh instead of inheriting live decoders found by path.
inline void releaseProjectSources(EditorState& es, SourceManager& sm) {
    es.sourcesPausedWhileUnseen.clear();
    sm.releaseUnreferenced(editorOwnedSources(es));
}

}  // namespace quadmapper
