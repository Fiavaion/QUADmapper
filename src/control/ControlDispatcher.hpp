#pragma once

#include "ControlEvent.hpp"
#include "ControlRegistry.hpp"
#include "MidiMapping.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace quadmapper::control {

// Applies drained ControlEvents to the registry's targets on the main thread
// (ADR-048 §2, §4): OSC messages by address, MIDI messages through the show's
// mappings, and MIDI learn. Keeps the counters and recent-activity lists the
// Control panel shows. Holds no scene state; targets reach it through the registry.
class ControlDispatcher {
public:
    static constexpr size_t kRecentMessages = 20;
    static constexpr size_t kRecentUnknown = 8;

    explicit ControlDispatcher(ControlRegistry& registry) : registry_(registry) {}

    // Apply one event. `mappings` are the show's MIDI mappings (a learn appends to
    // them); `now` is the caller's clock in seconds (activity indicators).
    void dispatch(const ControlEvent& event, std::vector<MidiMapping>& mappings, double now);

    // ---- MIDI learn --------------------------------------------------------
    // The next note-on, CC or program change binds to `targetPath` (and is not
    // applied). A new learn replaces a pending one.
    void beginLearn(const std::string& targetPath) { learnTarget_ = targetPath; }
    void cancelLearn() { learnTarget_.reset(); }
    bool isLearning() const { return learnTarget_.has_value(); }
    const std::string& learnTarget() const;
    // Index into `mappings` of the mapping the last learn created or found, or -1.
    int lastLearnedIndex() const { return lastLearnedIndex_; }

    // ---- Status for the Control panel --------------------------------------
    uint64_t oscMessages() const { return oscMessages_; }
    uint64_t unknownAddresses() const { return unknownAddresses_; }
    uint64_t badArguments() const { return badArguments_; }
    uint64_t midiMessages() const { return midiMessages_; }
    const std::deque<std::string>& recentOsc() const { return recentOsc_; }            // newest first
    const std::deque<std::string>& recentUnknown() const { return recentUnknown_; }    // newest first
    const std::string& lastMidi() const { return lastMidi_; }
    double lastOscTime() const { return lastOscTime_; }
    double lastMidiTime() const { return lastMidiTime_; }
    // When a target (by canonical path) was last driven, or < 0.
    double lastActivity(const std::string& canonicalPath) const;

    // Default mode for a freshly learned control (see MidiMapping.hpp).
    static MidiMode defaultMode(MidiControlType type, TargetKind kind);

    // Human-readable forms, for the panel and the logs.
    static std::string describe(const OscMessage& message);
    static std::string describe(const MidiMessage& message, const std::string& device);

private:
    void dispatchOsc(const OscMessage& message, double now);
    void dispatchMidi(const MidiMessage& message, const std::string& device,
                      std::vector<MidiMapping>& mappings, double now);
    bool learn(const MidiMessage& message, const std::string& device,
               std::vector<MidiMapping>& mappings);
    void applyMapping(MidiMapping& mapping, const MidiMessage& message, double now);

    // Target operations shared by OSC and MIDI (see TargetKind / MidiMode).
    void fire(const ControlRegistry::Resolved& resolved);              // press / no-arg message
    bool setValue(const ControlRegistry::Resolved& resolved, float value);  // false = rejected
    void setOnOff(const ControlRegistry::Resolved& resolved, bool on);
    void toggle(const ControlRegistry::Resolved& resolved);
    void noteActivity(const ControlRegistry::Resolved& resolved, double now);

    ControlRegistry& registry_;
    std::optional<std::string> learnTarget_;
    int lastLearnedIndex_ = -1;

    uint64_t oscMessages_ = 0;
    uint64_t unknownAddresses_ = 0;
    uint64_t badArguments_ = 0;
    uint64_t midiMessages_ = 0;
    std::deque<std::string> recentOsc_;
    std::deque<std::string> recentUnknown_;
    std::string lastMidi_;
    double lastOscTime_ = -1.0;
    double lastMidiTime_ = -1.0;
    std::unordered_map<std::string, double> activity_;
};

}  // namespace quadmapper::control
