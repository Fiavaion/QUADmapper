#pragma once

// Clearing the open project before another replaces it (New, Load, recovery), in one
// place and one order, so the three paths cannot drift apart (TD-14a).

#include "EditorState.hpp"
#include "MediaAssign.hpp"
#include "MeshLoads.hpp"
#include "MissingMedia.hpp"
#include "core/UUID.hpp"
#include "source/SourceManager.hpp"

namespace quadmapper {

// Forget the open project's content, in this order:
//  1. slice warp editing and the slice selection (a corner drag in progress is recorded
//     and then dropped with the history), the blend test pattern and the scene
//     crossfades in progress on `outputs` (null: none), so the next show's projectors
//     show the show;
//  2. the undo history, before the scenes its commands point into;
//     then every OBJ parse in progress (MeshLoads.hpp);
//  3. the scenes, their per-quad bindings, the object selection and any calibration;
//  4. the project's sources, released now (the media bin's own sources stay, so its
//     items remain assignable), so the next project's media opens fresh instead of
//     inheriting live decoders found by path; with them go the pause-while-unseen and
//     released-transport records, which name sources of this project only;
//  5. the missing-media records.
// Call before the next project creates any source. The outputs themselves, MIDI
// mappings and the app's own queues (scene takes, the missing-media search) are
// the caller's.
inline void clearProjectContent(EditorState& es, SourceManager* sm, OutputManager* outputs) {
    es.endSliceWarp();
    es.selectedSliceId = UUID();
    if (outputs) {
        outputs->setBlendTestPattern(false);
        outputs->clearSceneFades();
    }
    es.commandHistory.clear();
    cancelMeshLoads(es);  // before the scenes the parses would fill (TD-16a)
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
