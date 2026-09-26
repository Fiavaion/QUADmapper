#pragma once

#include <string>

namespace quadmapper {

struct EditorContext;
struct ControlPreferences;
namespace control { class ExternalControl; }

// The "Control" dockable window (ADR-048 §4): external control by OSC and MIDI.
//   - OSC: enable, port, bind address; status (listening / error / off), packet and
//     message counters, the last messages received and unknown addresses.
//   - MIDI: input devices to open (by name, reopened on reconnect), their status and
//     the last message.
//   - Targets: every current control target, with Learn (the next note/CC binds;
//     Esc cancels) and an activity light.
//   - Mappings: the show's MIDI mappings with mode editing, "any device", delete,
//     activity, and unresolved targets (a deleted quad or scene) marked.
// Settings changes are applied to `control` at once; render() returns true when
// `prefs` changed so the caller can save preferences. Visibility is
// ctx.es.showControl. Does not inherit Panel (EditorContext is needed per frame).
class ControlPanel {
public:
    ControlPanel() = default;

    // `now` is the clock the dispatcher records activity with (seconds).
    bool render(EditorContext& ctx, control::ExternalControl& control, ControlPreferences& prefs,
                double now);

private:
    bool renderOsc(control::ExternalControl& control, ControlPreferences& prefs);
    bool renderMidi(control::ExternalControl& control, ControlPreferences& prefs, double now);
    void renderTargets(control::ExternalControl& control, double now);
    void renderMappings(EditorContext& ctx, control::ExternalControl& control, double now);

    bool editsLoaded_ = false;
    int portEdit_ = 8000;
    char bindEdit_[64] = "0.0.0.0";
    char targetFilter_[64] = "";
};

}  // namespace quadmapper
