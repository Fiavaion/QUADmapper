#pragma once

#include "ControlDispatcher.hpp"
#include "ControlEventQueue.hpp"
#include "ControlRegistry.hpp"
#include "MidiInput.hpp"
#include "OscFeedback.hpp"
#include "OscServer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace quadmapper::control {

// Where and how OSC feedback goes (the app keeps it in ControlPreferences).
struct OscFeedbackSettings {
    bool enabled = false;
    std::string host;       // dotted IPv4; empty = reply to whoever last sent us OSC
    int port = 9000;        // 1-65535; 0 = the sender's own port (reply mode only)
    bool useIds = false;    // address quads/scenes/sources by UUID, not by unique name
};

// Everything external control owns (ADR-048), in one object the application keeps:
// the event queue, the OSC server, MIDI input, the target registry and the
// dispatcher. The queue is declared first so it outlives the threads feeding it.
class ExternalControl {
public:
    // Events applied per frame at most, so a flood cannot stall rendering; the rest
    // wait in the queue for the next frame (ADR-048 §5).
    static constexpr size_t kMaxEventsPerFrame = 256;
    // How often the MIDI device list is re-read for hot-plug.
    static constexpr double kMidiRescanSeconds = 2.0;
    // Feedback messages sent per update at most; the rest go on the next one.
    static constexpr size_t kMaxFeedbackPerUpdate = 256;
    // Trigger target registered here: resend every feedback value.
    static constexpr const char* kFeedbackRefreshPath = "/qm/feedback/refresh";

    ExternalControl();
    ~ExternalControl();

    ExternalControl(const ExternalControl&) = delete;
    ExternalControl& operator=(const ExternalControl&) = delete;

    ControlEventQueue& queue() { return queue_; }
    OscServer& osc() { return osc_; }
    MidiInput& midi() { return midi_; }
    ControlRegistry& registry() { return registry_; }
    ControlDispatcher& dispatcher() { return dispatcher_; }
    const ControlDispatcher& dispatcher() const { return dispatcher_; }

    // ---- OSC ----------------------------------------------------------------
    // Start (or restart) listening; on failure the reason is kept for the panel.
    bool startOsc(const std::string& bindAddress, int port);
    void stopOsc();
    const std::string& oscError() const { return oscError_; }

    // ---- MIDI ---------------------------------------------------------------
    // The devices the user chose. Each is opened when present and reopened when it
    // is plugged back in; devices not chosen are closed.
    void setWantedMidiDevices(const std::vector<std::string>& names);
    const std::vector<std::string>& wantedMidiDevices() const { return wantedMidi_; }
    const std::vector<std::string>& availableMidiDevices() const { return availableMidi_; }
    // Why a wanted device is not open ("not connected", a WinMM error), or empty.
    std::string midiDeviceStatus(const std::string& name) const;
    // Re-read the device list now (the panel's Refresh button).
    void rescanMidi();
    // Periodic hot-plug upkeep; cheap when not due. `now` in seconds.
    void maintainMidi(double now);

    // ---- Per frame ----------------------------------------------------------
    // Apply up to maxEvents queued events. Returns how many were applied.
    size_t processEvents(std::vector<MidiMapping>& mappings, double now,
                         size_t maxEvents = kMaxEventsPerFrame);
    bool hasPendingEvents() const { return queue_.size() > 0; }

    // ---- OSC feedback (ADR-048 amendment) -----------------------------------
    // Every Float/Bool/Index target's value is sampled at most every
    // OscFeedback::kDefaultMinInterval and sent, on change and rate-limited, from the
    // OSC input socket (so OSC input must be on). A new destination (a different
    // controller replying, new settings, OSC restarted) gets every value once.
    void setFeedback(const OscFeedbackSettings& settings);
    const OscFeedbackSettings& feedbackSettings() const { return feedbackSettings_; }
    // A sample is due and has somewhere to go: the caller refreshes the registry's
    // entities, then calls updateFeedback.
    bool feedbackDue(double now) const;
    // Sample, and send what changed. Cheap when not due. `now` in seconds.
    void updateFeedback(double now);
    // Send every value again on the next update (the panel's Send all).
    void sendAllFeedback() { feedback_.requestSendAll(); }

    enum class FeedbackStatus { Off, InvalidSettings, NeedsOscInput, WaitingForController, Sending };
    FeedbackStatus feedbackStatus() const;
    const std::string& feedbackError() const { return feedbackError_; }  // InvalidSettings
    // Where feedback goes now, when it goes anywhere.
    std::optional<OscServer::Endpoint> feedbackDestination() const;
    size_t feedbackAddressCount() const { return feedback_.size(); }

private:
    void onOscApplied(const ControlRegistry::Resolved& target, float sentValue, uint32_t senderIp);

    ControlEventQueue queue_;
    OscServer osc_;
    MidiInput midi_;
    ControlRegistry registry_;
    ControlDispatcher dispatcher_;

    std::string oscError_;
    std::vector<std::string> wantedMidi_;
    std::vector<std::string> availableMidi_;
    std::vector<std::pair<std::string, std::string>> midiErrors_;  // name -> reason
    double nextMidiScan_ = 0.0;
    std::vector<ControlEvent> drained_;  // reused per frame

    OscFeedbackSettings feedbackSettings_;
    std::string feedbackError_;
    std::optional<uint32_t> feedbackHostIp_;             // parsed fixed host
    OscFeedback feedback_;
    std::optional<OscServer::Endpoint> lastDestination_;  // what feedback_ has sent to
    double nextFeedback_ = 0.0;
    std::vector<OscFeedback::Sample> samples_;           // reused per update
    std::vector<OscMessage> outgoing_;                   // reused per update
    std::vector<uint8_t> datagram_;                      // reused per message
};

}  // namespace quadmapper::control
