#pragma once

// The show transport: the top bar's Play/Pause and Restart, a tap of Space, and the
// OSC /qm/timeline/* messages. They drive every video in the show, not only those
// locked to the master timeline (owner, 2026-11-20; ADR-050 amendment). A locked
// video follows the master clock; an unlocked one keeps its own clock and is paused,
// resumed and restarted with it.

#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"

namespace quadmapper {

inline void playShow(SourceManager& sources) {
    sources.playMasterTimeline();
    for (const UUID& id : sources.getAllSourceIds()) {
        PlayableSource* video = sources.getSourceAs<PlayableSource>(id);
        // Paused only: a one-shot clip that finished stays finished.
        if (video && !video->isTimelineLocked() &&
            video->getPlaybackState() == PlaybackState::PAUSED) {
            video->play();
        }
    }
}

inline void pauseShow(SourceManager& sources) {
    sources.pauseMasterTimeline();
    for (const UUID& id : sources.getAllSourceIds()) {
        PlayableSource* video = sources.getSourceAs<PlayableSource>(id);
        if (video && !video->isTimelineLocked() &&
            video->getPlaybackState() == PlaybackState::PLAYING) {
            video->pause();
        }
    }
}

inline bool showRunning(const SourceManager& sources) {
    return sources.getMasterTimeline().running();
}

inline void toggleShow(SourceManager& sources) {
    if (showRunning(sources)) pauseShow(sources);
    else playShow(sources);
}

// Every video back to its start and the master timeline to 0, keeping play or pause.
inline void restartShow(SourceManager& sources) { sources.seekAllToStart(); }

}  // namespace quadmapper
