#pragma once

#include "ControlEvent.hpp"
#include "ControlRegistry.hpp"
#include "MidiMapping.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    static constexpr size_t kMaxWarnedAddresses = 256;  // distinct refused addresses logged

    explicit ControlDispatcher(ControlRegistry& registry) : registry_(registry) {}

    // Apply one event. `mappings` are the show's MIDI mappings (a learn appends to
    // them); `now` is the caller's clock in seconds (activity indicators).
    void dispatch(const ControlEvent& event, std::vector<MidiMapping>& mappings, double now);

    // Called after an OSC message with a value set a target: the target, the value
    // the message carried and its sender (IPv4 host order, port), so feedback does
    // not echo that value back to the controller that just sent it.
    using OscAppliedHook = std::function<void(const ControlRegistry::Resolved& target, float sentValue,
                                              uint32_t senderIp, uint16_t senderPort)>;
    void setOscAppliedHook(OscAppliedHook hook) { oscApplied_ = std::move(hook); }

    // ---- MIDI learn --------------------------------------------------------
    // The next note-on, CC or program change binds to `targetPath` (and is not
    // applied). A new learn replaces a pending one. A CC 0-31 learned this way
    // becomes a 14-bit mapping when its LSB (n + 32, same device and channel)
    // follows within k14BitPairSeconds; an LSB learned first (32-63) becomes one
    // when its MSB follows. That completing message is not applied either.
    void beginLearn(const std::string& targetPath) {
        learnTarget_ = targetPath;
        pendingPair_.reset();
    }
    // Cancels a pending learn, and a 14-bit pair still waiting for its other half: the
    // perform lock cancels learning, and that half would still change the mapping (TD-36).
    void cancelLearn() {
        learnTarget_.reset();
        pendingPair_.reset();
    }
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
    double lastMidiTime() const { return lastMidiTime_; }
    // When a target (by canonical path) was last driven, or < 0.
    double lastActivity(const std::string& canonicalPath) const;

    // A target declined a message (a transport control on a clip locked to the master
    // timeline). Logs one warning per address and keeps the latest for the panel.
    void noteRefused(const std::string& address, const std::string& reason);
    const std::string& lastRefused() const { return lastRefused_; }  // "address: reason"
    uint64_t refusals() const { return refusals_; }

    // Default mode for a freshly learned control (see MidiMapping.hpp).
    static MidiMode defaultMode(MidiControlType type, TargetKind kind);

    // Human-readable forms, for the panel and the logs.
    static std::string describe(const OscMessage& message);
    static std::string describe(const MidiMessage& message, const std::string& device);

private:
    // A CC just learned, waiting for the other half of a 14-bit pair.
    struct PendingPair {
        size_t index = 0;        // into mappings
        std::string device;      // the mapping's device ("any" when unnamed)
        std::string target;
        int channel = 0;
        int number = 0;          // the learned controller (MSB 0-31 or LSB 32-63)
        int value = 0;           // its value
        double time = 0.0;
    };

    void dispatchOsc(const ControlEvent& event, double now);
    void dispatchMidi(const MidiMessage& message, const std::string& device,
                      std::vector<MidiMapping>& mappings, double now);
    bool learn(const MidiMessage& message, const std::string& device,
               std::vector<MidiMapping>& mappings, double now);
    bool completePair(const MidiMessage& message, const std::string& device,
                      std::vector<MidiMapping>& mappings, double now);
    void applyMapping(MidiMapping& mapping, const MidiMessage& message, double now);

    // Target operations shared by OSC and MIDI (see TargetKind / MidiMode).
    void fire(const ControlRegistry::Resolved& resolved);              // press / no-arg message
    bool setValue(const ControlRegistry::Resolved& resolved, float value);  // false = rejected
    void setOnOff(const ControlRegistry::Resolved& resolved, bool on);
    void toggle(const ControlRegistry::Resolved& resolved);
    void noteActivity(const ControlRegistry::Resolved& resolved, double now);

    ControlRegistry& registry_;
    OscAppliedHook oscApplied_;
    std::optional<std::string> learnTarget_;
    std::optional<PendingPair> pendingPair_;
    int lastLearnedIndex_ = -1;

    uint64_t oscMessages_ = 0;
    uint64_t unknownAddresses_ = 0;
    uint64_t badArguments_ = 0;
    uint64_t refusals_ = 0;
    uint64_t midiMessages_ = 0;
    std::deque<std::string> recentOsc_;
    std::deque<std::string> recentUnknown_;
    std::string lastMidi_;
    std::string lastRefused_;
    std::unordered_set<std::string> warnedRefused_;  // addresses already warned about (capped)
    bool warnedRefusedFull_ = false;
    double lastMidiTime_ = -1.0;
    std::unordered_map<std::string, double> activity_;
};

}  // namespace quadmapper::control
