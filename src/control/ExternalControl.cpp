#include "ExternalControl.hpp"

#include <algorithm>

namespace quadmapper::control {

ExternalControl::ExternalControl() : osc_(queue_), midi_(queue_), dispatcher_(registry_) {}

ExternalControl::~ExternalControl() {
    // Stop the producers before the queue (and the registry their events name) go.
    osc_.stop();
    midi_.closeAll();
}

bool ExternalControl::startOsc(const std::string& bindAddress, int port) {
    auto result = osc_.start(bindAddress, port);
    oscError_ = result ? std::string() : result.error().message;
    return static_cast<bool>(result);
}

void ExternalControl::stopOsc() {
    osc_.stop();
    oscError_.clear();
}

void ExternalControl::setWantedMidiDevices(const std::vector<std::string>& names) {
    wantedMidi_ = names;
    for (const std::string& open : midi_.openDeviceNames()) {
        if (std::find(wantedMidi_.begin(), wantedMidi_.end(), open) == wantedMidi_.end()) {
            midi_.close(open);
        }
    }
    rescanMidi();
}

std::string ExternalControl::midiDeviceStatus(const std::string& name) const {
    for (const auto& [device, reason] : midiErrors_) {
        if (device == name) return reason;
    }
    return std::string();
}

void ExternalControl::rescanMidi() {
    availableMidi_ = MidiInput::listDevices();
    midi_.closeMissing(availableMidi_);
    midiErrors_.clear();
    for (const std::string& name : wantedMidi_) {
        if (midi_.isOpen(name)) continue;
        if (std::find(availableMidi_.begin(), availableMidi_.end(), name) == availableMidi_.end()) {
            midiErrors_.emplace_back(name, "not connected");
            continue;
        }
        auto result = midi_.open(name);
        if (!result) midiErrors_.emplace_back(name, result.error().message);
    }
}

void ExternalControl::maintainMidi(double now) {
    if (now < nextMidiScan_) return;
    nextMidiScan_ = now + kMidiRescanSeconds;
    rescanMidi();
}

size_t ExternalControl::processEvents(std::vector<MidiMapping>& mappings, double now,
                                      size_t maxEvents) {
    drained_.clear();
    const size_t n = queue_.drain(drained_, maxEvents);
    for (const ControlEvent& event : drained_) dispatcher_.dispatch(event, mappings, now);
    drained_.clear();
    return n;
}

}  // namespace quadmapper::control
