#pragma once

// Clearing the open project before another replaces it (New, Load, recovery), in one
// place and one order, so the three paths cannot drift apart (TD-14a).

#include "EditorState.hpp"
#include "MediaAssign.hpp"
#include "MissingMedia.hpp"
#include "core/UUID.hpp"
#include "source/SourceManager.hpp"

namespace quadmapper {

// Forget the open project's content, in this order:
//  1. the undo history, before the scenes its commands point into;
//  2. the scenes, their per-quad bindings, the object selection and any calibration;
//  3. the project's sources, released now (the media bin's own sources stay, so its
//     items remain assignable), so the next project's media opens fresh instead of
//     inheriting live decoders found by path; with them go the pause-while-unseen and
//     released-transport records, which name sources of this project only;
//  4. the missing-media records.
// Call before the next project creates any source. Outputs, MIDI mappings and the
// Application's own queues (scene takes, the missing-media search) are the caller's.
inline void clearProjectContent(EditorState& es, SourceManager* sm) {
    es.commandHistory.clear();
    es.selectedObjectId = UUID();
    es.endCalibration();
    es.scenes.clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();
    es.activeVideoSourceId = UUID();  // the Video Player's clip belongs to the project
    es.sourcesPausedWhileUnseen.clear();
    es.releasedTransports.clear();
    if (sm) sm->releaseUnreferenced(editorOwnedSources(es));
    clearMissingMedia(es);
}

}  // namespace quadmapper
