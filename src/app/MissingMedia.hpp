#pragma once

// Missing media (PRD §4.1.8). A project that opens with media or mesh files gone keeps
// every reference to them: quads and objects stay bound to the missing source (drawn
// black on projectors and as a checker in the editor, see MissingMediaLook.hpp), and
// saving writes the original paths back. The Missing Media window lists these files; relocating one
// loads the file at its new place and rebinds everything that used it, then tries its
// missing neighbours in the same folder. The matching rules are in MediaRelocation.hpp.

#include "ControlBindings.hpp"
#include "EditorState.hpp"
#include "MediaAssign.hpp"
#include "core/FileSystem.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "media/MediaLibrary.hpp"
#include "project/MediaRelocation.hpp"
#include "project/Project.hpp"
#include "scene/MeshObject.hpp"
#include "scene/ObjLoader.hpp"
#include "source/SourceManager.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace quadmapper {

// One missing file and what uses it in the live scenes.
struct MissingMediaItem {
    enum class Kind { Source, Mesh };
    Kind kind = Kind::Source;
    UUID sourceId;     // Kind::Source: the id quads and objects reference
    std::string path;  // where the file was
    int quadUses = 0;
    int objectUses = 0;
};

inline bool isMeshMissing(const MeshObject& object) {
    return !object.getSourcePath().empty() && object.getMesh().empty();
}

// Missing files the scenes use, sorted by path. A source only an undo step still
// holds is left out: nothing on screen needs it.
inline std::vector<MissingMediaItem> collectMissingMedia(const EditorState& es) {
    std::vector<MissingMediaItem> items;
    std::unordered_map<UUID, size_t> sourceItems;
    std::unordered_map<std::string, size_t> meshItems;
    auto sourceItem = [&](const UUID& id) -> MissingMediaItem* {
        auto missing = es.missingSources.find(id);
        if (missing == es.missingSources.end()) return nullptr;
        auto [it, added] = sourceItems.emplace(id, items.size());
        if (added) {
            MissingMediaItem item;
            item.sourceId = id;
            item.path = missing->second.path;
            items.push_back(std::move(item));
        }
        return &items[it->second];
    };
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (MissingMediaItem* item = sourceItem(quad->getTexture().mediaId)) ++item->quadUses;
        }
        for (const auto& object : scene->getObjects()) {
            if (!object) continue;
            if (MissingMediaItem* item = sourceItem(object->getTexture().mediaId)) ++item->objectUses;
            if (!isMeshMissing(*object)) continue;
            auto [it, added] = meshItems.emplace(object->getSourcePath(), items.size());
            if (added) {
                MissingMediaItem item;
                item.kind = MissingMediaItem::Kind::Mesh;
                item.path = object->getSourcePath();
                items.push_back(std::move(item));
            }
            ++items[it->second].objectUses;
        }
    }
    std::sort(items.begin(), items.end(), [](const MissingMediaItem& a, const MissingMediaItem& b) {
        return std::tie(a.path, a.kind) < std::tie(b.path, b.kind);
    });
    return items;
}

inline std::string absoluteMediaPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    return utf8FromPath(ec ? path : abs.lexically_normal());
}

// Move everything that referenced the missing source missingId (loaded from oldPath)
// onto the live source newId at newPath: quads, objects, the media paths undo steps
// re-bind from, and MIDI mappings aimed at it (load keeps them on the missing id).
// isVideo makes newId the Video Player's source when it has none.
inline void rebindMissingSource(EditorState& es, const UUID& missingId, const std::string& oldPath,
                                const UUID& newId, const std::string& newPath, bool isVideo) {
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (quad->getTexture().mediaId != missingId) continue;
            quad->getTexture().mediaId = newId;
            es.quadTexturePaths[quad->getId()] = newPath;
        }
        for (const auto& object : scene->getObjects()) {
            if (object && object->getTexture().mediaId == missingId) object->getTexture().mediaId = newId;
        }
    }
    // Quads only an undo step holds re-bind from their media path once restored.
    for (auto& [quadId, quadPath] : es.quadTexturePaths) {
        if (quadPath == oldPath) quadPath = newPath;
    }
    const std::unordered_map<UUID, UUID> moved{{missingId, newId}};
    for (auto& mapping : es.midiMappings) {
        mapping.target = remapTargetPath(mapping.target, "source", moved);
    }
    if (isVideo && es.activeVideoSourceId.isNull()) es.activeVideoSourceId = newId;
    es.missingSources.erase(missingId);
}

// Load newPath in place of a missing source and rebind everything that used it.
// Returns why the file was not accepted, or empty once it was. The file may have
// another extension or type than the one it replaces; it is accepted only if it loads.
inline std::string relocateMissingSource(EditorState& es, SourceManager& sm, const UUID& missingId,
                                         const std::filesystem::path& newPath) {
    auto missing = es.missingSources.find(missingId);
    if (missing == es.missingSources.end()) return "It is no longer missing.";
    const Project::SourceData original = missing->second;
    const std::string path = absoluteMediaPath(newPath);
    const std::filesystem::path file = pathFromUtf8(path);

    const std::optional<MediaType> type =
        MediaLibrary::getTypeFromExtension(utf8FromPath(newPath.extension()));
    if (!type) return "Not an image or video file QUADmapper can open.";
    const bool isVideo = (*type == MediaType::VIDEO);
    const bool alreadyOpen = !(isVideo ? sm.findVideoSourceByPath(file)
                                       : sm.findImageSourceByPath(file)).isNull();
    const UUID id = isVideo ? sm.getOrCreateVideoSource(file) : sm.getOrCreateImageSource(file);
    if (id.isNull()) return "The file could not be loaded.";

    // A clip opened here plays as it was saved (or loops from the start, as on assign).
    // One already open elsewhere keeps its own playback.
    if (!alreadyOpen) applySavedTransport(sm, id, original.transport);

    rebindMissingSource(es, missingId, original.path, id, path, isVideo);
    return {};
}

// Load newPath as the mesh of every object whose mesh at oldPath is missing.
inline std::string relocateMissingMesh(EditorState& es, const std::string& oldPath,
                                       const std::filesystem::path& newPath) {
    auto mesh = loadObj(newPath);
    if (!mesh) return mesh.error().message;
    const std::string path = absoluteMediaPath(newPath);
    int relocated = 0;
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& object : scene->getObjects()) {
            if (!object || object->getSourcePath() != oldPath || !isMeshMissing(*object)) continue;
            object->getMesh() = mesh.value();
            object->setSourcePath(path);
            ++relocated;
        }
    }
    return relocated > 0 ? std::string() : "It is no longer missing.";
}

inline std::string relocateMissingItem(EditorState& es, SourceManager* sm,
                                       const MissingMediaItem& item,
                                       const std::filesystem::path& newPath) {
    if (item.kind == MissingMediaItem::Kind::Mesh) return relocateMissingMesh(es, item.path, newPath);
    if (!sm) return "Media cannot be loaded right now.";
    return relocateMissingSource(es, *sm, item.sourceId, newPath);
}

// Record the outcome of one attempt for the window to show. True when it succeeded.
inline bool noteRelocation(EditorState& es, const MissingMediaItem& item, const std::string& error) {
    if (error.empty()) {
        es.missingMediaErrors.erase(item.path);
        return true;
    }
    es.missingMediaErrors[item.path] = error;
    return false;
}

// "Locate...": relocate item to the file the user picked, then every other missing
// file that sits in the same folder under its own name. Returns how many were resolved.
inline int locateMissingMedia(EditorState& es, SourceManager* sm, Project* project,
                              const MissingMediaItem& item, const std::filesystem::path& newPath) {
    if (!noteRelocation(es, item, relocateMissingItem(es, sm, item, newPath))) return 0;
    int resolved = 1;
    const std::filesystem::path folder = newPath.parent_path();
    for (const MissingMediaItem& other : collectMissingMedia(es)) {
        const std::filesystem::path sibling = relocate::findSibling(folder, pathFromUtf8(other.path));
        if (!sibling.empty() && noteRelocation(es, other, relocateMissingItem(es, sm, other, sibling))) {
            ++resolved;
        }
    }
    if (project) project->setModified(true);
    return resolved;
}

// "Search folder...": relocate every missing file found under the searched folder.
inline int relocateMissingFromIndex(EditorState& es, SourceManager* sm, Project* project,
                                    const relocate::FolderIndex& index) {
    int resolved = 0;
    for (const MissingMediaItem& item : collectMissingMedia(es)) {
        const std::filesystem::path found = relocate::findInIndex(index, pathFromUtf8(item.path));
        if (!found.empty() && noteRelocation(es, item, relocateMissingItem(es, sm, item, found))) {
            ++resolved;
        }
    }
    if (resolved > 0 && project) project->setModified(true);
    return resolved;
}

// The project is being replaced (New, Load, recovery): forget its missing files.
inline void clearMissingMedia(EditorState& es) {
    es.missingSources.clear();
    es.missingMediaErrors.clear();
    es.showMissingMedia = false;
}

}  // namespace quadmapper
