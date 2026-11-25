#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include "ui/ConfirmPopup.hpp"

namespace quadmapper {

struct EditorContext;
class CompositionSource;

// The Compositions tab of the Resources window (ADR-061): make a composition (a
// Timeline that runs its cues by their hold times) or a Switcher (cues change on
// Next / Previous / Cue n, by hand, OSC or MIDI), fill its cues from the media bin,
// set the crossfade, and put it on the selected quads. Prev / Next / Cue and Play /
// Pause stay usable under the perform lock; editing does not.
class CompositionPanel : public Panel {
public:
    static constexpr const char* ID = "Compositions";

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
    bool editsShow() const override { return false; }  // the transport works locked
    void drawContent(EditorContext& ctx) override;

private:
    void drawList(EditorContext& ctx);
    void drawTransport(EditorContext& ctx, CompositionSource& comp);
    void drawSettings(EditorContext& ctx, CompositionSource& comp, bool locked);
    void drawCues(EditorContext& ctx, CompositionSource& comp, bool locked);

    UUID selected_;
    ConfirmPopup deleteConfirm_{"compositiondelete"};
    UUID pendingDeleteId_;  // the composition the open confirmation is about
    char nameEdit_[64] = "";
    UUID nameFor_;
};

}  // namespace quadmapper
