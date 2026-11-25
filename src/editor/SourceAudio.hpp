#pragma once

// Sound for video sources (ADR-050 §2): each frame, every FFmpeg or HAP video source
// hands its playback clock to the app's AudioEngine, which keeps the source's audio
// attachment on it. Header-only so the clock mapping is unit-tested without the app.

#include "audio/AudioEngine.hpp"
#include "core/UUID.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneFade.hpp"
#include "source/CompositionSource.hpp"
#include "source/HapFileSource.hpp"
#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"
#include "source/VideoFileSource.hpp"
#include "video/PlaybackClock.hpp"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace quadmapper {

// Audio plays only at 1x; anything else is silent (tape-style varispeed is later work).
inline bool isAudioSpeed(double speed) { return std::abs(speed - 1.0) < 1e-4; }

// The audio's view of a clock at `now`: audible only while it runs forward at 1x.
inline AudioTransport audioTransportOf(const PlaybackClock& clock, PlaybackTimePoint now) {
    AudioTransport t;
    t.wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    t.timeline = clock.timeline(now);
    const ClipPosition clip = clock.sample(now);
    t.clip = clip.position;
    t.discontinuity = clock.discontinuity(now);
    t.running = clock.running();
    t.duration = clock.duration();
    t.loop = clock.loopMode() == LoopMode::LOOP;
    const bool forwardClock = clock.baseDirection() > 0;
    const bool unitSpeed = isAudioSpeed(clock.speed());
    t.audible = t.running && unitSpeed && forwardClock && clip.direction > 0 && !clip.ended;
    // On a Ping-Pong reverse leg the next forward leg is known ahead: pre-roll it.
    if (t.running && unitSpeed && forwardClock && clip.direction < 0 &&
        clock.loopMode() == LoopMode::PING_PONG) {
        const PlaybackLeg leg = legAt(t.timeline, t.duration, clock.loopMode());
        if (!leg.final) {
            const PlaybackLeg next = nextLeg(leg, t.duration, clock.loopMode());
            if (next.direction > 0) t.nextAudibleStart = next.start;
        }
    }
    return t;
}

// A source that is not PLAYING is silent even while its clock runs: a timeline-locked
// source that is paused (by the user, or because it is off screen) keeps following
// the master timeline.
inline AudioTransport withPlaybackState(AudioTransport transport, PlaybackState state) {
    if (state != PlaybackState::PLAYING) {
        transport.running = false;
        transport.audible = false;
        transport.nextAudibleStart.reset();
    }
    return transport;
}

// Why a playing source is silent, for the UI; nullptr when it is not.
inline const char* audioSilenceReason(const PlayableSource& source) {
    const PlaybackClock& clock = source.getClock();
    if (source.getPlaybackState() != PlaybackState::PLAYING || !clock.running()) return nullptr;
    if (!isAudioSpeed(clock.speed())) return "Silent at non-1x speed";
    if (clock.baseDirection() < 0 || clock.direction(source.getClockNow()) < 0) {
        return "Silent while reversing";
    }
    return nullptr;
}

// The file whose audio a source plays, or empty for sources without one.
inline std::filesystem::path audioPathOf(const TextureSource& source) {
    if (const auto* video = dynamic_cast<const VideoFileSource*>(&source)) return video->getPath();
    if (const auto* hap = dynamic_cast<const HapFileSource*>(&source)) return hap->getPath();
    return {};
}

// How loud each heard scene is (the audibility rule, ADR-050 §2, with the scene
// crossfades of ADR-047 §5). A source is heard only through a scene that is the
// editor's active scene or that an enabled one of `outputs` shows (a container of
// OutputWindow pointers, as OutputManager::getOutputs() returns; a template so this
// header needs no output module). Such scenes are at 1; while an enabled output fades
// (`fadeOf(output)` returns its fade, as OutputManager::sceneFade does, or null), its
// scenes are at sceneFadeAudioGains at `now` (the scene fading out at
// crossfadeGains(mix).out, the one fading in at .in). A scene several outputs weigh
// takes the largest weight. The active scene is not raised to 1 while it fades in: a
// take makes it active at the start of its fade. Scenes absent from the map are
// silent: their sources keep decoding and stay in step, gated, so a take switches the
// soundtrack with the picture.
template <typename Outputs, typename FadeOf>
std::unordered_map<UUID, float> sceneAudioWeights(const UUID& activeSceneId, const Outputs& outputs,
                                                  FadeOf fadeOf, double now) {
    std::unordered_map<UUID, float> weights;
    auto raise = [&](const UUID& id, float weight) {
        if (id.isNull() || !(weight > 0.0f)) return;
        float& w = weights[id];
        w = std::max(w, weight);
    };
    std::unordered_set<UUID> fadingIn;
    for (const auto& output : outputs) {
        if (!output || !output->isEnabled() || output->getSceneId().isNull()) continue;
        if (const SceneFade* fade = fadeOf(*output)) {
            for (const SceneFadeLeg& leg : sceneFadeAudioGains(*fade, now)) raise(leg.sceneId, leg.weight);
            fadingIn.insert(fade->toSceneId);
        } else {
            raise(output->getSceneId(), 1.0f);
        }
    }
    if (fadingIn.count(activeSceneId) == 0) raise(activeSceneId, 1.0f);
    return weights;
}

// Each source's scene gain from the scene weights: the sum over the scenes using it,
// at most 1. A source playing in both scenes of a fade is one sound, not two, so it
// stays at full level (out + in >= 1 across an equal-power fade) instead of dipping.
inline std::unordered_map<UUID, float> sourceSceneGains(const SceneList& scenes,
                                                       const std::unordered_map<UUID, float>& weights) {
    std::unordered_map<UUID, float> gains;
    for (const auto& scene : scenes) {
        if (!scene) continue;
        auto weight = weights.find(scene->getId());
        if (weight == weights.end()) continue;
        std::unordered_set<UUID> used;  // a source counts once per scene
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (!quad->getTexture().mediaId.isNull()) used.insert(quad->getTexture().mediaId);
        }
        for (const auto& object : scene->getObjects()) {
            if (!object->getTexture().mediaId.isNull()) used.insert(object->getTexture().mediaId);
        }
        for (const UUID& id : used) {
            float& gain = gains[id];
            gain = std::min(1.0f, gain + weight->second);
        }
    }
    return gains;
}

// Each heard source's gain now, from the editor's active scene and an output manager
// (anything with getOutputs(), sceneFade(output) and frameTime(), as OutputManager).
// The one rule for both the sound (syncSourceAudio) and the UI's "Silent" label.
template <typename OutputSource>
std::unordered_map<UUID, float> heardSourceGains(const SceneList& scenes, const UUID& activeSceneId,
                                                 const OutputSource& outputs) {
    const auto weights = sceneAudioWeights(
        activeSceneId, outputs.getOutputs(), [&outputs](const auto& o) { return outputs.sceneFade(o); },
        outputs.frameTime());
    return sourceSceneGains(scenes, weights);
}

// A composition's sound (ADR-061) is its cues' clips: the cue coming on is heard at the
// composition's gain times how far it has faded in, the one going out at the rest, the
// other cues not at all. Raises (never lowers) `gains`, at most 1.
inline void addCompositionCueGains(SourceManager& sources, std::unordered_map<UUID, float>& gains) {
    for (const UUID& id : sources.getCompositionIds()) {
        const auto heard = gains.find(id);
        if (heard == gains.end()) continue;
        const float gain = heard->second;
        const auto* composition = sources.getSourceAs<CompositionSource>(id);
        if (!composition) continue;
        const CompositionSettings& s = composition->settings();
        const CompositionPlayhead& p = composition->playhead();
        const float mix = static_cast<float>(p.fadeMix(s));
        auto raise = [&](int index, float weight) {
            if (index < 0 || index >= static_cast<int>(s.cues.size()) || !(weight > 0.0f)) return;
            const UUID& cue = s.cues[static_cast<size_t>(index)].sourceId;
            if (cue.isNull()) return;
            float& g = gains[cue];
            g = std::min(1.0f, std::max(g, gain * weight));
        };
        raise(p.current(), p.previous() >= 0 ? mix : 1.0f);
        raise(p.previous(), 1.0f - mix);
    }
}

// Once per frame, after SourceManager::updateAll(): keep every video source's audio on
// its clock (creating attachments as sources appear) and free the audio of sources
// that are gone — released by the source GC, unloaded, or replaced by a project load.
// Each source plays at its scene gain (sourceSceneGains); sources without one are
// gated silent, still in step.
inline void syncSourceAudio(AudioEngine& audio, SourceManager& sources,
                            const std::unordered_map<UUID, float>& sceneGains) {
    std::unordered_set<UUID> live;
    for (const UUID& id : sources.getAllSourceIds()) {
        TextureSource* source = sources.getSource(id);
        auto* playable = dynamic_cast<PlayableSource*>(source);
        if (!playable) continue;
        const std::filesystem::path path = audioPathOf(*source);
        if (path.empty()) continue;
        live.insert(id);
        if (!source->isReady()) continue;
        AudioTransport transport = withPlaybackState(
            audioTransportOf(playable->getClock(), playable->getClockNow()), playable->getPlaybackState());
        transport.volume = playable->getAudioVolume();
        transport.muted = playable->isAudioMuted();
        auto gain = sceneGains.find(id);
        transport.gated = gain == sceneGains.end();
        // Gated: gain 0 as well as muted, so no callback can see the volume rise
        // between the two stores at the end of a fade.
        transport.sceneGain = transport.gated ? 0.0f : gain->second;
        audio.sync(id, path, transport);
    }
    audio.retainOnly(live);
    audio.poll();
}

}  // namespace quadmapper
