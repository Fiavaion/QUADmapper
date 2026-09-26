#include "ExternalControl.hpp"

#include "OscWriter.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper::control {

namespace {

// The feedback argument for a target's value: Index targets as an integer (scene
// numbers), Float and Bool as a float (0/1 for Bool), which every control surface
// accepts for faders and toggles alike.
OscArg feedbackArg(TargetKind kind, float value) {
    OscArg arg;
    if (kind == TargetKind::Index) {
        arg.type = OscArg::Type::Int;
        arg.tag = 'i';
        arg.i = static_cast<int32_t>(std::lround(value));
    } else {
        arg.type = OscArg::Type::Float;
        arg.tag = 'f';
        arg.f = value;
    }
    return arg;
}

}  // namespace

ExternalControl::ExternalControl() : osc_(queue_), midi_(queue_), dispatcher_(registry_) {
    dispatcher_.setOscAppliedHook(
        [this](const ControlRegistry::Resolved& target, float sentValue, uint32_t senderIp, uint16_t) {
            onOscApplied(target, sentValue, senderIp);
        });
    // A restarted controller asks for the whole state. In reply-to-sender mode its
    // message also makes it the destination (a new one gets everything anyway).
    registry_.addGlobal(kFeedbackRefreshPath,
                        {"", TargetKind::Trigger, 0.0f, 1.0f, true,
                         [this](const std::string&, float) { feedback_.requestSendAll(); }, nullptr});
}

ExternalControl::~ExternalControl() {
    // Stop the producers before the queue (and the registry their events name) go.
    osc_.stop();
    midi_.closeAll();
}

bool ExternalControl::startOsc(const std::string& bindAddress, int port) {
    auto result = osc_.start(bindAddress, port);
    oscError_ = result ? std::string() : result.error().message;
    lastDestination_.reset();  // feedback: a (re)started socket counts as a new connection
    return static_cast<bool>(result);
}

void ExternalControl::stopOsc() {
    osc_.stop();
    oscError_.clear();
    lastDestination_.reset();
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

// ---- OSC feedback ----------------------------------------------------------------

void ExternalControl::setFeedback(const OscFeedbackSettings& settings) {
    feedbackSettings_ = settings;
    feedbackError_.clear();
    feedbackHostIp_.reset();
    if (settings.port < 0 || settings.port > 65535) {
        feedbackError_ = "port " + std::to_string(settings.port) + " is out of range (1-65535)";
    } else if (!settings.host.empty()) {
        feedbackHostIp_ = OscServer::parseIpv4(settings.host);
        if (!feedbackHostIp_) {
            feedbackError_ = "'" + settings.host + "' is not an IPv4 address (like 192.168.1.20)";
        } else if (settings.port == 0) {
            feedbackError_ = "a fixed host needs a port";
        }
    }
    // Start over: the next update sends everything to wherever feedback now goes.
    lastDestination_.reset();
    feedback_.reset();
    nextFeedback_ = 0.0;
}

std::optional<OscServer::Endpoint> ExternalControl::feedbackDestination() const {
    if (!feedbackSettings_.enabled || !feedbackError_.empty() || !osc_.isRunning()) return std::nullopt;
    const auto port = static_cast<uint16_t>(feedbackSettings_.port);
    if (feedbackHostIp_) return OscServer::Endpoint{*feedbackHostIp_, port};
    const auto sender = osc_.lastSender();
    if (!sender) return std::nullopt;
    return OscServer::Endpoint{sender->ip, port == 0 ? sender->port : port};
}

ExternalControl::FeedbackStatus ExternalControl::feedbackStatus() const {
    if (!feedbackSettings_.enabled) return FeedbackStatus::Off;
    if (!feedbackError_.empty()) return FeedbackStatus::InvalidSettings;
    if (!osc_.isRunning()) return FeedbackStatus::NeedsOscInput;
    if (!feedbackDestination()) return FeedbackStatus::WaitingForController;
    return FeedbackStatus::Sending;
}

bool ExternalControl::feedbackDue(double now) const {
    return feedbackSettings_.enabled && now >= nextFeedback_ && feedbackDestination().has_value();
}

void ExternalControl::updateFeedback(double now) {
    if (!feedbackSettings_.enabled || now < nextFeedback_) return;
    nextFeedback_ = now + OscFeedback::kDefaultMinInterval;

    const auto destination = feedbackDestination();
    if (!destination) {
        lastDestination_.reset();
        feedback_.reset();
        return;
    }
    if (!lastDestination_ || *lastDestination_ != *destination) {
        lastDestination_ = destination;  // a new controller: it has none of our values
        feedback_.reset();
    }

    samples_.clear();
    for (const auto& target : registry_.feedbackTargets(!feedbackSettings_.useIds)) {
        const std::optional<float> value = target.target->read(target.entityId);
        if (!value || !std::isfinite(*value)) continue;
        samples_.push_back({target.canonicalPath, target.address, feedbackArg(target.target->kind, *value)});
    }
    feedback_.setValues(samples_);

    outgoing_.clear();
    feedback_.collect(now, outgoing_, kMaxFeedbackPerUpdate);
    for (const OscMessage& message : outgoing_) {
        datagram_.clear();
        if (writeOscMessage(message, datagram_)) osc_.sendTo(*destination, datagram_);
    }
}

void ExternalControl::onOscApplied(const ControlRegistry::Resolved& target, float sentValue,
                                   uint32_t senderIp) {
    // Only the controller feedback goes to already shows what it sent; another
    // device (a fixed host elsewhere) still needs the change.
    if (!lastDestination_ || lastDestination_->ip != senderIp) return;
    feedback_.markKnown(target.canonicalPath, feedbackArg(target.target->kind, sentValue));
}

}  // namespace quadmapper::control
