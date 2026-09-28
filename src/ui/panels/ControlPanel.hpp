#pragma once

#include <string>

namespace quadmapper {

struct EditorContext;
struct ControlPreferences;
namespace control {
class ExternalControl;
struct OscFeedbackSettings;
}

// The OSC listen and feedback settings and the MIDI inputs to open, applied to
// `control` at once. Each render returns true when `prefs` changed, so the caller
// saves. The Control window and Edit > Preferences each hold one and edit the same
// ControlPreferences: an editor reloads its text fields (port, address, feedback host
// and port) when the other one applied new values.
class ControlSettingsEditor {
public:
    // Enable, UDP port, bind address (Apply), and whether it is listening.
    bool renderOsc(control::ExternalControl& control, ControlPreferences& prefs);
    // Enable, reply-to-sender or a fixed host, port (Apply), id addressing; status,
    // destination, packets sent, Send all.
    bool renderFeedback(control::ExternalControl& control, ControlPreferences& prefs);
    // Refresh, one checkbox per device (present or wanted), and each one's status.
    bool renderMidiDevices(control::ExternalControl& control, ControlPreferences& prefs);

    // The feedback settings the preferences hold.
    static control::OscFeedbackSettings feedbackSettings(const ControlPreferences& prefs);

private:
    void syncEdits(const ControlPreferences& prefs);

    bool loaded_ = false;
    // The prefs' applied values as this editor last saw them.
    int appliedPort_ = 0;
    std::string appliedBind_;
    std::string appliedFeedbackHost_;
    int appliedFeedbackPort_ = 0;
    // The fields being edited.
    int portEdit_ = 8000;
    char bindEdit_[64] = "0.0.0.0";
    bool feedbackReplyEdit_ = true;       // reply to the sender (else a fixed host)
    char feedbackHostEdit_[64] = "";
    int feedbackPortEdit_ = 9000;
};

// The "Control" dockable window (ADR-048 §4): external control by OSC and MIDI.
//   - OSC: enable, port, bind address; status (listening / error / off), packet and
//     message counters, the last messages received and unknown addresses.
//   - OSC feedback: enable, reply-to-sender or a fixed host, port, id addressing;
//     status and destination, packets sent, Send all.
//   - MIDI: input devices to open (by name, reopened on reconnect), their status and
//     the last message.
//   - Targets: every current control target, with Learn (the next note/CC binds;
//     Esc cancels) and an activity light.
//   - Mappings: the show's MIDI mappings with mode editing, "any device", delete,
//     activity, 14-bit CC pairs marked, and unresolved targets (a deleted quad or
//     scene) marked.
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

    ControlSettingsEditor settings_;
    char targetFilter_[64] = "";
};

}  // namespace quadmapper
