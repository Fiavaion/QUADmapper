#pragma once

#include "ControlDispatcher.hpp"
#include "ControlEventQueue.hpp"
#include "ControlRegistry.hpp"
#include "MidiInput.hpp"
#include "OscServer.hpp"

#include <string>
#include <vector>

namespace quadmapper::control {

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

private:
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
};

}  // namespace quadmapper::control
