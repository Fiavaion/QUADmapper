#pragma once

// Replace in show (QM-082): after Convert to HAP, move every quad and 3D object that
// shows a clip onto the converted file, as one undoable step. The new clip picks up
// the old one's transport (loop mode, speed, sound, lock, play state and position).

#include "EditorState.hpp"
#include "MediaAssign.hpp"
#include "MissingMedia.hpp"  // absoluteMediaPath
#include "ControlBindings.hpp"
#include "core/CommandHistory.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"
#include "source/SourceManager.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quadmapper {

// Rebinds what shows any of `fromIds` to `toId`. The first execute records exactly
// what it moved; undo puts back those bindings and redo moves the same ones again, so
// a quad given the new clip afterwards is left alone by undo. Holds ids only (see
// Commands.hpp): a quad or scene deleted since is skipped.
class ReplaceSourceCommand : public Command {
public:
    ReplaceSourceCommand(EditorState& es, std::unordered_set<UUID> fromIds, UUID toId, std::string toPath)
        : es_(es), fromIds_(std::move(fromIds)), toId_(toId), toPath_(std::move(toPath)) {}

    bool execute() override {
        if (!recorded_) record();
        if (moves_.empty()) return false;
        for (const Move& m : moves_) {  // (redo: skip what was rebound after the undo)
            if (Quad* quad = findQuad(m)) {
                if (quad->getTexture().mediaId != m.before) continue;
                quad->getTexture().mediaId = toId_;
                es_.quadTexturePaths[m.id] = toPath_;
            } else if (MeshObject* object = findObject(m)) {
                if (object->getTexture().mediaId == m.before) object->getTexture().mediaId = toId_;
            }
        }
        for (const Mapping& m : mappings_) {
            if (m.index < es_.midiMappings.size() && es_.midiMappings[m.index].target == m.before) {
                es_.midiMappings[m.index].target = m.after;
            }
        }
        activeBefore_ = UUID();
        if (fromIds_.count(es_.activeVideoSourceId)) {
            activeBefore_ = es_.activeVideoSourceId;
            es_.activeVideoSourceId = toId_;
        }
        return true;
    }

    bool undo() override {
        for (const Move& m : moves_) {
            if (Quad* quad = findQuad(m)) {
                if (quad->getTexture().mediaId != toId_) continue;  // rebound since: leave it
                quad->getTexture().mediaId = m.before;
                if (m.hadPath) es_.quadTexturePaths[m.id] = m.beforePath;
                else           es_.quadTexturePaths.erase(m.id);
            } else if (MeshObject* object = findObject(m)) {
                if (object->getTexture().mediaId == toId_) object->getTexture().mediaId = m.before;
            }
        }
        for (const Mapping& m : mappings_) {
            if (m.index < es_.midiMappings.size() && es_.midiMappings[m.index].target == m.after) {
                es_.midiMappings[m.index].target = m.before;
            }
        }
        if (!activeBefore_.isNull() && es_.activeVideoSourceId == toId_) es_.activeVideoSourceId = activeBefore_;
        return true;
    }

    std::string getDescription() const override { return "Replace clip with converted file"; }

    // Undo needs the old clip loaded, redo the new one.
    void collectMediaIds(std::unordered_set<UUID>& out) const override {
        out.insert(fromIds_.begin(), fromIds_.end());
        out.insert(toId_);
    }

private:
    struct Move {
        bool isQuad = true;
        UUID sceneId;
        UUID id;
        UUID before;
        bool hadPath = false;
        std::string beforePath;
    };
    struct Mapping {
        size_t index = 0;
        std::string before;
        std::string after;
    };

    void record() {
        recorded_ = true;
        for (const auto& scene : es_.scenes) {
            if (!scene) continue;
            for (const auto& quad : scene->getQuadManager().getQuads()) {
                const UUID mediaId = quad->getTexture().mediaId;
                if (!fromIds_.count(mediaId)) continue;
                Move m;
                m.sceneId = scene->getId();
                m.id = quad->getId();
                m.before = mediaId;
                auto path = es_.quadTexturePaths.find(m.id);
                m.hadPath = path != es_.quadTexturePaths.end();
                if (m.hadPath) m.beforePath = path->second;
                moves_.push_back(std::move(m));
            }
            for (const auto& object : scene->getObjects()) {
                if (!object || !fromIds_.count(object->getTexture().mediaId)) continue;
                Move m;
                m.isQuad = false;
                m.sceneId = scene->getId();
                m.id = object->getId();
                m.before = object->getTexture().mediaId;
                moves_.push_back(std::move(m));
            }
        }
        std::unordered_map<UUID, UUID> moved;
        for (const UUID& id : fromIds_) moved.emplace(id, toId_);
        for (size_t i = 0; i < es_.midiMappings.size(); ++i) {
            const std::string& before = es_.midiMappings[i].target;
            const std::string after = remapTargetPath(before, "source", moved);
            if (after != before) mappings_.push_back({i, before, after});
        }
    }

    Scene* findScene(const Move& m) const {
        for (const auto& scene : es_.scenes) {
            if (scene && scene->getId() == m.sceneId) return scene.get();
        }
        return nullptr;
    }
    Quad* findQuad(const Move& m) const {
        Scene* scene = m.isQuad ? findScene(m) : nullptr;
        return scene ? scene->getQuadManager().getQuad(m.id) : nullptr;
    }
    MeshObject* findObject(const Move& m) const {
        Scene* scene = m.isQuad ? nullptr : findScene(m);
        if (!scene) return nullptr;
        for (const auto& object : scene->getObjects()) {
            if (object && object->getId() == m.id) return object.get();
        }
        return nullptr;
    }

    EditorState& es_;
    std::unordered_set<UUID> fromIds_;
    UUID toId_;
    std::string toPath_;
    bool recorded_ = false;
    std::vector<Move> moves_;
    std::vector<Mapping> mappings_;
    UUID activeBefore_;
};

// The sources showing `file`: the one opened from that path, and any a quad bound
// under another spelling of it (relative, different case) that names the same file.
inline std::unordered_set<UUID> sourcesShowingFile(const EditorState& es, const SourceManager& sm,
                                                   const std::filesystem::path& file) {
    std::unordered_set<UUID> ids;
    const UUID byPath = sm.findVideoSourceByPath(file);
    if (!byPath.isNull()) ids.insert(byPath);
    for (const auto& [quadId, quadPath] : es.quadTexturePaths) {
        std::error_code ec;
        if (!std::filesystem::equivalent(pathFromUtf8(quadPath), file, ec) || ec) continue;
        for (const auto& scene : es.scenes) {
            if (!scene) continue;
            if (const Quad* quad = scene->getQuadManager().getQuad(quadId)) {
                if (!quad->getTexture().mediaId.isNull()) ids.insert(quad->getTexture().mediaId);
            }
        }
    }
    return ids;
}

// Open `converted` and move everything that shows `original` onto it (undoable).
// Returns why nothing was replaced, or empty once it was.
inline std::string replaceClipInShow(EditorState& es, SourceManager& sm, const std::filesystem::path& original,
                                     const std::filesystem::path& converted) {
    const std::unordered_set<UUID> fromIds = sourcesShowingFile(es, sm, original);
    const std::string name = utf8FromPath(original.filename());
    if (fromIds.empty()) return "Nothing in the show uses " + name + ".";

    const bool alreadyOpen = !sm.findVideoSourceByPath(converted).isNull();
    const UUID toId = sm.getOrCreateVideoSource(converted);
    if (toId.isNull()) return "The converted file could not be opened.";

    const size_t undoBefore = es.commandHistory.getUndoCount();
    es.commandHistory.execute(
        std::make_unique<ReplaceSourceCommand>(es, fromIds, toId, absoluteMediaPath(converted)));
    if (es.commandHistory.getUndoCount() == undoBefore) {
        return "Nothing in the show uses " + name + ".";  // only the bin or an undo step held it
    }

    // The converted clip carries on as the original was playing, from the same place.
    UUID transportFrom = sm.findVideoSourceByPath(original);
    if (transportFrom.isNull()) transportFrom = *fromIds.begin();
    const auto* before = sm.getSourceAs<PlayableSource>(transportFrom);
    auto* after = sm.getSourceAs<PlayableSource>(toId);
    if (before && after && !alreadyOpen) {
        const double position = before->getCurrentTime();
        applySavedTransport(sm, toId, transportOf(*before));
        if (!after->isTimelineLocked()) after->seek(position);
        // It opens off the main thread: its surfaces show the original until its first frame.
        sm.setStandIn(toId, transportFrom);
    }
    return {};
}

}  // namespace quadmapper
