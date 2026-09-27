#pragma once

// Centralized media -> quad binding (ADR-042 P2). The pattern of "resolve a source,
// point the quad's TextureBinding::mediaId at it, record the path for .qmap, drop any
// legacy per-quad texture, mark modified" was duplicated across the Inspector, the
// Video Player and the drag-drop targets — and the Inspector copy wrongly created an
// IMAGE source even for videos. This single point fixes that and is reused everywhere.

#include "EditorState.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"          // MediaType
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "source/SourceManager.hpp"
#include "scene/Quad.hpp"
#include "scene/MeshObject.hpp"
#include "scene/MediaReferences.hpp"
#include "project/Project.hpp"

#include <iterator>
#include <optional>
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

// A clip's transport as .qmap saves it.
inline Project::TransportData transportOf(const PlayableSource& p) {
    Project::TransportData t;
    t.playing = (p.getPlaybackState() == PlaybackState::PLAYING);
    t.loopMode = p.getLoopMode();
    t.speed = p.getSpeed();
    t.timelineLocked = p.isTimelineLocked();
    t.volume = p.getAudioVolume();
    t.muted = p.isAudioMuted();
    return t;
}

// Put a clip just opened for a saved source into the state it was saved in: loop mode,
// speed, sound (volume, mute), lock to the master timeline, then playing or paused. Load, relocating a
// missing file and re-opening a released source all come through here, so none of
// them can drop part of the transport. With nothing saved (a file older than v3.1)
// the clip plays, looping, as on assign.
inline void applySavedTransport(SourceManager& sm, const UUID& id,
                                const std::optional<Project::TransportData>& transport) {
    auto* playable = sm.getSourceAs<PlayableSource>(id);
    if (!playable) return;
    if (!transport) {
        playable->play();
        return;
    }
    playable->setLoopMode(transport->loopMode);
    playable->setSpeed(transport->speed);
    playable->setAudioVolume(transport->volume);
    playable->setAudioMuted(transport->muted);
    // Locking sets the clip playing, so the saved play/pause is applied after it.
    if (transport->timelineLocked) sm.setTimelineLocked(id, true);
    if (transport->playing) playable->play();
    else                    playable->pause();
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
                     item.hasPath() ? utf8FromPath(item.getPath()) : std::string(), isVideo);
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

// Pause-while-unseen (part of releaseUnusedSources): a playing clip that only the
// undo history still holds (its quad was deleted) should not keep decoding and
// uploading unseen. Pause it and remember it in es.sourcesPausedWhileUnseen; resume it
// once a quad shows it again (undo). A clip the user had paused is never touched, and
// neither is one the editor keeps (owned: the Video Player's, the bin's).
// onScreen: sources bound to a quad that exists now; inUse: every source still
// referenced, by a scene or by the undo history.
inline void pauseSourcesOnlyUndoHolds(EditorState& es, SourceManager& sm,
                                      const std::unordered_set<UUID>& onScreen,
                                      const std::unordered_set<UUID>& inUse,
                                      const std::unordered_set<UUID>& owned) {
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

// Frame-boundary source housekeeping (QM-088): release sources that no quad in any
// scene uses, that no undo/redo step can restore, and that the editor does not keep
// alive, so orphaned videos and generators stop decoding, rendering and uploading.
//
// A quad whose source is gone (a binding restored from an older history, for example)
// is re-resolved from its saved media path; a video re-opened this way gets back the
// transport it had when it was released (EditorState::releasedTransports), else plays
// as on assign. If that fails the binding is cleared so it is not retried every frame.
// A binding to a missing file (EditorState::missingSources) is kept as it is, for the
// user to relocate.
inline void releaseUnusedSources(EditorState& es, SourceManager& sm) {
    std::unordered_set<UUID> onScreen;  // bound to a quad that exists right now
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            UUID& mediaId = quad->getTexture().mediaId;
            if (!mediaId.isNull() && sm.hasSource(mediaId)) onScreen.insert(mediaId);
            if (mediaId.isNull() || sm.hasSource(mediaId) || es.missingSources.count(mediaId)) continue;
            auto pathIt = es.quadTexturePaths.find(quad->getId());
            UUID healed;
            if (pathIt != es.quadTexturePaths.end() && !pathIt->second.empty()) {
                const std::filesystem::path path = pathFromUtf8(pathIt->second);
                const std::optional<MediaType> type =
                    MediaLibrary::getTypeFromExtension(utf8FromPath(path.extension()));
                if (type == MediaType::VIDEO) {
                    // A clip already open for another quad keeps its own playback.
                    const bool alreadyOpen = !sm.findVideoSourceByPath(path).isNull();
                    healed = sm.getOrCreateVideoSource(path);
                    if (!healed.isNull() && !alreadyOpen) {
                        std::optional<Project::TransportData> saved;
                        auto savedIt = es.releasedTransports.find(mediaId);
                        if (savedIt != es.releasedTransports.end()) {
                            saved = savedIt->second;
                            es.releasedTransports.erase(savedIt);
                        }
                        applySavedTransport(sm, healed, saved);
                    }
                } else if (type == MediaType::IMAGE) {
                    healed = sm.getOrCreateImageSource(path);
                }  // any other file cannot be re-opened: the binding clears
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
    // The media bin's hover previews are open clips like any other, so a Load or an
    // assignment can find one by path and bind it. A bound preview belongs to its new
    // user; one whose hover ended and that nothing bound is removed here.
    const auto bound = [&](const UUID& id) {
        return inUse.count(id) > 0 || id == es.activeVideoSourceId;
    };
    for (const UUID& id : es.mediaBinPanel.takeEndedPreviews()) {
        if (!bound(id)) sm.removeSource(id);
    }
    const UUID preview = es.mediaBinPanel.getPreviewSourceId();
    if (!preview.isNull() && bound(preview)) es.mediaBinPanel.handOverPreviewSource();
    const std::unordered_set<UUID> owned = editorOwnedSources(es);
    inUse.insert(owned.begin(), owned.end());
    // Remember how each clip about to be released was playing, for a binding that
    // comes back to it later (the re-open above).
    for (const UUID& id : sm.getAllSourceIds()) {
        if (inUse.count(id)) continue;
        if (const auto* playable = sm.getSourceAs<PlayableSource>(id)) {
            es.releasedTransports[id] = transportOf(*playable);
        }
    }
    sm.releaseUnreferenced(inUse);
    // A missing file nothing refers to any more (its quads deleted beyond undo) is
    // no longer worth relocating or saving.
    for (auto it = es.missingSources.begin(); it != es.missingSources.end();) {
        it = inUse.count(it->first) ? std::next(it) : es.missingSources.erase(it);
    }

    pauseSourcesOnlyUndoHolds(es, sm, onScreen, inUse, owned);
}

}  // namespace quadmapper
