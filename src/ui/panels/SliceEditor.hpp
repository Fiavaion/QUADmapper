#pragma once

#include "core/UUID.hpp"

namespace quadmapper {

struct EditorContext;
class OutputWindow;

// The slice editor (ADR-047 part B): the "Slices" section of the Properties panel for
// the selected output. Lists the output's slices (add, remove, select) and edits the
// selected one: its input rectangle on the scene's canvas, its warp corners (numbers,
// or on the canvas with the mouse and Tab / arrows / Shift / Alt), its four blend
// edges and its black level. Also the scene-wide Auto-blend action and the blend test
// pattern toggle. Every edit is an undo step (SliceEditing.hpp) and marks the show
// modified. Disabled while the output is being calibrated.
class SliceEditor {
public:
    SliceEditor() = default;

    // Inside the Properties window, for the output it shows.
    void render(EditorContext& ctx, OutputWindow& output);

private:
    UUID nameSliceId_;  // the slice nameBuffer_ holds the name of
    char nameBuffer_[128] = "";
    bool nameEditing_ = false;  // the name field was being typed in last frame
};

}  // namespace quadmapper
