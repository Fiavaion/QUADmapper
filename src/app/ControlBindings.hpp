#pragma once

// The application's control targets (ADR-048 §2): what OSC addresses and MIDI
// mappings can drive, bound to the live scenes, sources and outputs. Header-only so
// the bindings are unit-tested without GL or the Application.
//
// External control changes live values only: quad opacity/visibility, source
// transport and sound, the master timeline, the master sound, scene takes and blackout.
// It never touches geometry, files or undo,
// and every target re-resolves its quad/source by id when it runs, so a target
// whose quad was deleted does nothing.
//
// Adding a target is one registration below.

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
    std::function<int()> activeSceneNumber;     // the active scene (1-based), 0 = none
    std::function<bool()> isBlackout;
    std::function<void(bool on)> setBlackout;
    // A message a target would not act on: its canonical address and why.
    std::function<void(const std::string& address, const std::string& reason)> refuse;
    // Master sound (ADR-050 §2), kept by the app's audio engine.
    std::function<float()> getMasterVolume;
    std::function<void(float volume)> setMasterVolume;
    std::function<bool()> isMasterMuted;
    std::function<void(bool muted)> setMasterMuted;
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

// A clip locked to the master timeline follows it and ignores its own play, pause,
// seek and speed (ADR-050 §3), and the Video Player greys those out. A transport
// message for it is refused rather than half-applied (a pause that freezes the
// projector with no way to resume there); the master is driven by /qm/timeline/*.
// Returns the clip to drive, or null when there is none or it was refused.
inline PlayableSource* unlockedPlayable(const ControlBindingHost& host, const std::string& id,
                                        const char* action) {
    PlayableSource* p = findPlayable(host.sources, id);
    if (!p || !p->isTimelineLocked()) return p;
    if (host.refuse) {
        host.refuse("/qm/source/" + id + "/" + action,
                    "locked to master timeline; use /qm/timeline/play, pause or restart");
    }
    return nullptr;
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
    // Master sound (ADR-050 §2).
    registry.addGlobal("/qm/master/volume",
        {"", TargetKind::Float, 0.0f, 1.0f, true,
         [host](const std::string&, float v) { if (host.setMasterVolume) host.setMasterVolume(v); },
         [host](const std::string&) -> std::optional<float> {
             if (!host.getMasterVolume) return std::nullopt;
             return host.getMasterVolume();
         }});
    registry.addGlobal("/qm/master/mute",
        {"", TargetKind::Bool, 0.0f, 1.0f, true,
         [host](const std::string&, float v) { if (host.setMasterMuted) host.setMasterMuted(v >= 0.5f); },
         [host](const std::string&) -> std::optional<float> {
             if (!host.isMasterMuted) return std::nullopt;
             return host.isMasterMuted() ? 1.0f : 0.0f;
         }});
    // /qm/scene/take i n: the nth scene, the same path as the 1-9 keys. OSC only (a
    // MIDI control binds to a particular scene's take instead).
    registry.addGlobal("/qm/scene/take",
        {"", TargetKind::Index, 1.0f, 1.0e6f, false,
         [host](const std::string&, float n) { if (host.takeScene) host.takeScene(static_cast<int>(n)); },
         nullptr, /*exclusive=*/true});
    // /qm/scene/active i n: which scene is active, sent as OSC feedback so a radio
    // button on a control surface follows the show. Setting it takes that scene.
    registry.addGlobal("/qm/scene/active",
        {"", TargetKind::Index, 1.0f, 1.0e6f, false,
         [host](const std::string&, float n) { if (host.takeScene) host.takeScene(static_cast<int>(n)); },
         [host](const std::string&) -> std::optional<float> {
             if (!host.activeSceneNumber) return std::nullopt;
             const int n = host.activeSceneNumber();
             if (n < 1) return std::nullopt;
             return static_cast<float>(n);
         }, /*exclusive=*/true});
    // The master timeline every locked clip follows (ADR-050 §3): the same calls as
    // the menu bar's Timeline Play/Pause and Restart buttons.
    registry.addGlobal("/qm/timeline/play",
        {"", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string&, float) { if (host.sources) host.sources->playMasterTimeline(); },
         nullptr});
    registry.addGlobal("/qm/timeline/pause",
        {"", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string&, float) { if (host.sources) host.sources->pauseMasterTimeline(); },
         nullptr});
    registry.addGlobal("/qm/timeline/restart",
        {"", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string&, float) { if (host.sources) host.sources->restartMasterTimeline(); },
         nullptr});
    // Running state of the master timeline: a play/pause toggle whose light follows it.
    registry.addGlobal("/qm/timeline/running",
        {"", TargetKind::Bool, 0.0f, 1.0f, true,
         [host](const std::string&, float v) {
             if (!host.sources) return;
             if (v >= 0.5f) host.sources->playMasterTimeline();
             else           host.sources->pauseMasterTimeline();
         },
         [host](const std::string&) -> std::optional<float> {
             if (!host.sources) return std::nullopt;
             return host.sources->getMasterTimeline().running() ? 1.0f : 0.0f;
         }});

    // ---- Scenes ------------------------------------------------------------
    registry.addEntityTarget("scene",
        {"take", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (!host.scenes || !host.takeScene) return;
             if (const int number = sceneNumber(*host.scenes, id)) host.takeScene(number);
         },
         nullptr, /*exclusive=*/true});

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
             if (PlayableSource* p = unlockedPlayable(host, id, "play")) p->play();
         },
         nullptr});
    registry.addEntityTarget("source",
        {"pause", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (PlayableSource* p = unlockedPlayable(host, id, "pause")) p->pause();
         },
         nullptr});
    registry.addEntityTarget("source",
        {"restart", TargetKind::Trigger, 0.0f, 1.0f, true,
         [host](const std::string& id, float) {
             if (PlayableSource* p = unlockedPlayable(host, id, "restart")) {
                 if (p->canSeek()) p->seek(0.0);
                 p->play();
             }
         },
         nullptr});
    registry.addEntityTarget("source",
        {"speed", TargetKind::Float, 0.1f, 4.0f, true,
         [host](const std::string& id, float v) {
             if (PlayableSource* p = unlockedPlayable(host, id, "speed")) p->setSpeed(v);
         },
         [host](const std::string& id) -> std::optional<float> {
             const PlayableSource* p = findPlayable(host.sources, id);
             return p ? std::optional<float>(p->getSpeed()) : std::nullopt;
         }});
    // The source's sound (ADR-050 §2): what its Video Player volume slider and mute set.
    registry.addEntityTarget("source",
        {"volume", TargetKind::Float, 0.0f, 1.0f, true,
         [host](const std::string& id, float v) {
             if (PlayableSource* p = findPlayable(host.sources, id)) p->setAudioVolume(v);
         },
         [host](const std::string& id) -> std::optional<float> {
             const PlayableSource* p = findPlayable(host.sources, id);
             return p ? std::optional<float>(p->getAudioVolume()) : std::nullopt;
         }});
    registry.addEntityTarget("source",
        {"mute", TargetKind::Bool, 0.0f, 1.0f, true,
         [host](const std::string& id, float v) {
             if (PlayableSource* p = findPlayable(host.sources, id)) p->setAudioMuted(v >= 0.5f);
         },
         [host](const std::string& id) -> std::optional<float> {
             const PlayableSource* p = findPlayable(host.sources, id);
             return p ? std::optional<float>(p->isAudioMuted() ? 1.0f : 0.0f) : std::nullopt;
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
    d.bits = m.bits;
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
    m.bits = d.bits;
    return m;
}

// Rewrite a target's `collection` id through savedToLive. Loading a project gives its
// sources new live ids (and a quad a new one only when its saved id clashes), and
// relocating a missing file moves its bindings to a new source id. A target whose
// entity is not in the map is kept as it is (unresolved if the entity is gone).
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
