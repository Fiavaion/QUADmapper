#pragma once

// Sound for video sources (ADR-050 §2): each frame, every FFmpeg or HAP video source
// hands its playback clock to the app's AudioEngine, which keeps the source's audio
// attachment on it. Header-only so the clock mapping is unit-tested without the app.

#include "audio/AudioEngine.hpp"
#include "core/UUID.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "source/HapFileSource.hpp"
#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"
#include "source/VideoFileSource.hpp"
#include "video/PlaybackClock.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
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

// The scenes that are live: those an enabled one of `outputs` shows (a container of
// OutputWindow pointers, as OutputManager::getOutputs() returns; a template so this
// header needs no output module). Several outputs may show one scene (ADR-047).
template <typename Outputs>
std::unordered_set<UUID> liveSceneIds(const Outputs& outputs) {
    std::unordered_set<UUID> ids;
    for (const auto& output : outputs) {
        if (output && output->isEnabled() && !output->getSceneId().isNull()) {
            ids.insert(output->getSceneId());
        }
    }
    return ids;
}

// Which sources are heard: a source is audible only when a quad or 3D object using it
// belongs to the editor's active scene or to a scene an enabled output shows. Sources
// used only by other scenes keep decoding and stay in step, gated silent, so a scene
// take switches the soundtrack with the picture.
inline std::unordered_set<UUID> audibleSourceIds(const SceneList& scenes, const UUID& activeSceneId,
                                                 const std::unordered_set<UUID>& liveScenes) {
    std::unordered_set<UUID> ids;
    for (const auto& scene : scenes) {
        if (!scene) continue;
        const bool heard = scene->getId() == activeSceneId || liveScenes.count(scene->getId()) > 0;
        if (!heard) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (!quad->getTexture().mediaId.isNull()) ids.insert(quad->getTexture().mediaId);
        }
        for (const auto& object : scene->getObjects()) {
            if (!object->getTexture().mediaId.isNull()) ids.insert(object->getTexture().mediaId);
        }
    }
    return ids;
}

// Once per frame, after SourceManager::updateAll(): keep every video source's audio on
// its clock (creating attachments as sources appear) and free the audio of sources
// that are gone — released by the source GC, unloaded, or replaced by a project load.
// Sources outside `audible` are gated silent (see audibleSourceIds).
inline void syncSourceAudio(AudioEngine& audio, SourceManager& sources,
                            const std::unordered_set<UUID>& audible) {
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
        transport.gated = audible.count(id) == 0;
        audio.sync(id, path, transport);
    }
    audio.retainOnly(live);
    audio.poll();
}

}  // namespace quadmapper
