#pragma once

#include "ControlEvent.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace quadmapper::control {

// What OSC feedback sends, decided without sockets or clocks so it is tested with
// injected time (ADR-048 amendment). The owner samples every feedback target's
// current value (setValues) and asks which messages are due (collect):
//   - on change only: a value is sent when it differs from the last one sent to the
//     controller (or was never sent, or a send-all is pending);
//   - rate-limited per address: at most one message per minInterval; changes in
//     between are coalesced, so only the latest value goes out, and a value that
//     returns to what the controller already has sends nothing;
//   - bounded per call: at most maxMessages, the rest stay due for the next call,
//     which starts where this one stopped (round robin).
// Keys are canonical target paths (stable across renames); each carries the address
// its value is sent to. A key missing from the latest sample is forgotten.
class OscFeedback {
public:
    static constexpr double kDefaultMinInterval = 1.0 / 30.0;  // <= 30 Hz per address

    struct Sample {
        std::string key;       // canonical path
        std::string address;   // where the value is sent
        OscArg value;          // Int or Float
    };

    explicit OscFeedback(double minInterval = kDefaultMinInterval) : minInterval_(minInterval) {}

    // Replace the current values. An address change (a renamed quad) counts as
    // never sent, since the controller has nothing on the new address.
    void setValues(const std::vector<Sample>& samples);

    // The controller already shows `value` for `key` (it just sent it to us), so an
    // unchanged value is not echoed back. Unknown keys are ignored.
    void markKnown(const std::string& key, const OscArg& value);

    // Send every current value again (a new controller, the panel's Send all).
    void requestSendAll();

    // Append the messages due at `now` to `out` (at most maxMessages) and record them
    // as sent. Returns how many were appended.
    size_t collect(double now, std::vector<OscMessage>& out, size_t maxMessages);

    // Forget everything sent (feedback turned off or pointed elsewhere).
    void reset();

    size_t size() const { return entries_.size(); }
    // Values that differ from what the controller has (sent or waiting on the rate limit).
    size_t pendingCount() const;

private:
    struct Entry {
        std::string address;
        OscArg value;
        bool everSent = false;
        OscArg sent;                 // the last value sent (or known to the controller)
        double lastSendTime = 0.0;
        bool sentOnce = false;       // lastSendTime is valid
        bool force = false;          // send-all pending
    };
    static bool sameValue(const OscArg& a, const OscArg& b);
    static bool needsSend(const Entry& e);

    double minInterval_;
    std::map<std::string, Entry> entries_;  // ordered: deterministic send order
    std::string resumeKey_;                 // where a capped collect() stopped
};

}  // namespace quadmapper::control
