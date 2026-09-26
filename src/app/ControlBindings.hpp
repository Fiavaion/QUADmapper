#pragma once

// The application's control targets (ADR-048 §2): what OSC addresses and MIDI
// mappings can drive, bound to the live scenes, sources and outputs. Header-only so
// the bindings are unit-tested without GL or the Application.
//
// External control changes live values only: quad opacity/visibility, source
// transport, scene takes and blackout. It never touches geometry, files or undo,
// and every target re-resolves its quad/source by id when it runs, so a target
// whose quad was deleted does nothing.
//
// Adding a target is one registration below. Audio (ADR-050) adds, in
// registerControlTargets():
//   registry.addEntityTarget("source", {"volume", TargetKind::Float, 0.0f, 1.0f, true, apply, read});
//   registry.addGlobal("/qm/master/volume", {"", TargetKind::Float, 0.0f, 1.0f, true, apply, read});
//   registry.addGlobal("/qm/master/mute", {"", TargetKind::Bool, 0.0f, 1.0f, true, apply, read});

#include "../control/ControlRegistry.hpp"
#include "../control/MidiMapping.hpp"
#include "../core/UUID.hpp"
#include "../project/Project.hpp"
#include "../scene/Quad.hpp"
#include "../scene/Scene.hpp"
#include "../source/SourceManager.hpp"
#include "../source/TextureSource.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quadmapper {

// What the targets act on. Pointers are the Application's long-lived members.
struct ControlBindingHost {
    std::vector<std::unique_ptr<Scene>>* scenes = nullptr;
    SourceManager* sources = nullptr;
    std::function<void(int number)> takeScene;  // queue a take of scene `number` (1-based)
    std::function<bool()> isBlackout;
    std::function<void(bool on)> setBlackout;
};

namespace control_bindings {

// UUID::fromString throws on malformed text; paths come from the network and files.
inline UUID parseUuid(const std::string& text) {
    try {
        return UUID::fromString(text);
    } catch (const std::exception&) {
        return UUID::null();
    }
}

inline Quad* findQuad(std::vector<std::unique_ptr<Scene>>& scenes, const std::string& id) {
    const UUID uuid = parseUuid(id);
    if (uuid.isNull()) return nullptr;
    for (auto& scene : scenes) {
        if (!scene) continue;
        if (Quad* quad = scene->getQuadManager().getQuad(uuid)) return quad;
    }
    return nullptr;
}

inline int sceneNumber(const std::vector<std::unique_ptr<Scene>>& scenes, const std::string& id) {
    const UUID uuid = parseUuid(id);
    for (size_t i = 0; i < scenes.size(); ++i) {
        if (scenes[i] && scenes[i]->getId() == uuid) return static_cast<int>(i) + 1;
    }
    return 0;
}

inline PlayableSource* findPlayable(SourceManager* sources, const std::string& id) {
    if (!sources) return nullptr;
    const UUID uuid = parseUuid(id);
    if (uuid.isNull()) return nullptr;
    return dynamic_cast<PlayableSource*>(sources->getSource(uuid));
}

}  // namespace control_bindings

inline void registerControlTargets(control::ControlRegistry& registry, const ControlBindingHost& host) {
    using control::ControlTarget;
    using control::TargetKind;
    using namespace control_bindings;

    // ---- Global ------------------------------------------------------------
    registry.addGlobal("/qm/blackout",
        {"", TargetKind::Bool, 0.0f, 1.0f, true,
         [host](const std::string&, float v) { if (host.setBlackout) host.setBlackout(v >= 0.5f); },
         [host](const std::string&) -> std::optional<float> {
             if (!host.isBlackout) return std::nullopt;
             return host.isBlackout() ? 1.0f : 0.0f;
         }});
    // /qm/scene/take i n: the nth scene, the same path as the 1-9 keys. OSC only (a
    // MIDI control binds to a particular scene's take instead).
    registry.addGlobal("/qm/scene/take",
        {"", TargetKind::Index, 1.0f, 1.0e6f, false,
         [host](const std::string&, float n) { if (host.takeScene) host.takeScene(static_cast<int>(n)); },
         nullptr});

    // ---- Scenes ------------------------------------------------------------
    registry.addEntityTarget("scene",
        {"take", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (!host.scenes || !host.takeScene) return;
             if (const int number = sceneNumber(*host.scenes, id)) host.takeScene(number);
         },
         nullptr});

    // ---- Quads -------------------------------------------------------------
    registry.addEntityTarget("quad",
        {"opacity", TargetKind::Float, 0.0f, 1.0f, true,
         [host](const std::string& id, float v) {
             if (!host.scenes) return;
             if (Quad* quad = findQuad(*host.scenes, id)) quad->setOpacity(v);
         },
         [host](const std::string& id) -> std::optional<float> {
             if (!host.scenes) return std::nullopt;
             const Quad* quad = findQuad(*host.scenes, id);
             return quad ? std::optional<float>(quad->getOpacity()) : std::nullopt;
         }});
    registry.addEntityTarget("quad",
        {"visible", TargetKind::Bool, 0.0f, 1.0f, true,
         [host](const std::string& id, float v) {
             if (!host.scenes) return;
             if (Quad* quad = findQuad(*host.scenes, id)) quad->setVisible(v >= 0.5f);
         },
         [host](const std::string& id) -> std::optional<float> {
             if (!host.scenes) return std::nullopt;
             const Quad* quad = findQuad(*host.scenes, id);
             return quad ? std::optional<float>(quad->isVisible() ? 1.0f : 0.0f) : std::nullopt;
         }});

    // ---- Sources (the PlayableSource transport) ----------------------------
    registry.addEntityTarget("source",
        {"play", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (PlayableSource* p = findPlayable(host.sources, id)) p->play();
         },
         nullptr});
    registry.addEntityTarget("source",
        {"pause", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (PlayableSource* p = findPlayable(host.sources, id)) p->pause();
         },
         nullptr});
    registry.addEntityTarget("source",
        {"restart", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (PlayableSource* p = findPlayable(host.sources, id)) {
                 if (p->canSeek()) p->seek(0.0);
                 p->play();
             }
         },
         nullptr});
    registry.addEntityTarget("source",
        {"speed", TargetKind::Float, 0.1f, 4.0f, true,
         [host](const std::string& id, float v) {
             if (PlayableSource* p = findPlayable(host.sources, id)) p->setSpeed(v);
         },
         [host](const std::string& id) -> std::optional<float> {
             const PlayableSource* p = findPlayable(host.sources, id);
             return p ? std::optional<float>(p->getSpeed()) : std::nullopt;
         }});
}

// Refresh the entities paths can name from the live project. Scenes and quads in
// list order (quads scene by scene); playable sources in the order the scenes' quads
// first show them, then any others (e.g. the Video Player's) by id.
inline void refreshControlEntities(control::ControlRegistry& registry,
                                   const std::vector<std::unique_ptr<Scene>>& scenes,
                                   SourceManager* sources) {
    std::vector<control::ControlEntity> sceneEntities;
    std::vector<control::ControlEntity> quadEntities;
    std::vector<UUID> sourceOrder;
    std::unordered_set<UUID> seenSources;
    for (const auto& scene : scenes) {
        if (!scene) continue;
        sceneEntities.push_back({scene->getId().toString(), scene->getName()});
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (!quad) continue;
            quadEntities.push_back({quad->getId().toString(), quad->getName()});
            const UUID& mediaId = quad->getTexture().mediaId;
            if (!mediaId.isNull() && seenSources.insert(mediaId).second) sourceOrder.push_back(mediaId);
        }
    }

    std::vector<control::ControlEntity> sourceEntities;
    if (sources) {
        std::vector<UUID> others;
        for (const UUID& id : sources->getAllSourceIds()) {
            if (!seenSources.count(id)) others.push_back(id);
        }
        std::sort(others.begin(), others.end(),
                  [](const UUID& a, const UUID& b) { return a.toString() < b.toString(); });
        sourceOrder.insert(sourceOrder.end(), others.begin(), others.end());
        for (const UUID& id : sourceOrder) {
            if (auto* playable = dynamic_cast<PlayableSource*>(sources->getSource(id))) {
                sourceEntities.push_back({id.toString(), playable->getName()});
            }
        }
    }

    registry.setEntities("scene", std::move(sceneEntities));
    registry.setEntities("quad", std::move(quadEntities));
    registry.setEntities("source", std::move(sourceEntities));
}

// ---- Mapping persistence ------------------------------------------------------

inline Project::MidiMappingData toProjectMapping(const control::MidiMapping& m) {
    Project::MidiMappingData d;
    d.device = m.device;
    d.type = control::toString(m.type);
    d.channel = m.channel;
    d.number = m.number;
    d.target = m.target;
    d.mode = control::toString(m.mode);
    return d;
}

// The serializer only keeps entries with a known type and mode, so the fallbacks
// never apply to a loaded file.
inline control::MidiMapping fromProjectMapping(const Project::MidiMappingData& d) {
    control::MidiMapping m;
    m.device = d.device.empty() ? std::string("any") : d.device;
    m.type = control::parseMidiControlType(d.type).value_or(control::MidiControlType::Note);
    m.channel = d.channel;
    m.number = d.number;
    m.target = d.target;
    m.mode = control::parseMidiMode(d.mode).value_or(control::MidiMode::Trigger);
    return m;
}

// Loading a project gives its quads and sources new live ids (scenes keep theirs),
// so saved targets are rewritten from saved to live ids. A target whose entity is
// not in the map (deleted before the save) is kept as it is and stays unresolved.
inline std::string remapTargetPath(const std::string& path, const std::string& collection,
                                   const std::unordered_map<UUID, UUID>& savedToLive) {
    const std::string prefix = "/qm/" + collection + "/";
    if (path.compare(0, prefix.size(), prefix) != 0) return path;
    const size_t idEnd = path.find('/', prefix.size());
    if (idEnd == std::string::npos) return path;
    const UUID saved = control_bindings::parseUuid(path.substr(prefix.size(), idEnd - prefix.size()));
    if (saved.isNull()) return path;
    auto it = savedToLive.find(saved);
    if (it == savedToLive.end()) return path;
    return prefix + it->second.toString() + path.substr(idEnd);
}

}  // namespace quadmapper
