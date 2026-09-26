#include "ControlDispatcher.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace quadmapper::control {

namespace {

void pushRecent(std::deque<std::string>& list, std::string entry, size_t limit) {
    list.push_front(std::move(entry));
    while (list.size() > limit) list.pop_back();
}

}  // namespace

const std::string& ControlDispatcher::learnTarget() const {
    static const std::string kNone;
    return learnTarget_ ? *learnTarget_ : kNone;
}

double ControlDispatcher::lastActivity(const std::string& canonicalPath) const {
    auto it = activity_.find(canonicalPath);
    return it == activity_.end() ? -1.0 : it->second;
}

MidiMode ControlDispatcher::defaultMode(MidiControlType type, TargetKind kind) {
    switch (kind) {
        case TargetKind::Bool:
            return MidiMode::Toggle;
        case TargetKind::Float:
            if (type == MidiControlType::CC) return MidiMode::Value;
            if (type == MidiControlType::Note) return MidiMode::Momentary;
            return MidiMode::Trigger;
        case TargetKind::Trigger:
        case TargetKind::Index:
            return MidiMode::Trigger;
    }
    return MidiMode::Trigger;
}

std::string ControlDispatcher::describe(const OscMessage& message) {
    std::string text = message.address;
    for (const auto& arg : message.args) {
        text += ' ';
        char number[32];
        switch (arg.type) {
            case OscArg::Type::Int:
                text += std::to_string(arg.i);
                break;
            case OscArg::Type::Float:
                std::snprintf(number, sizeof(number), "%g", static_cast<double>(arg.f));
                text += number;
                break;
            case OscArg::Type::String:
                text += '"' + arg.s.substr(0, 64) + '"';
                break;
            case OscArg::Type::True:  text += "T"; break;
            case OscArg::Type::False: text += "F"; break;
            case OscArg::Type::Unsupported:
                text += '(';
                text += arg.tag;
                text += ')';
                break;
        }
    }
    return text;
}

std::string ControlDispatcher::describe(const MidiMessage& message, const std::string& device) {
    char text[160];
    const char* kind = "Note on";
    switch (message.type) {
        case MidiType::NoteOn:        kind = "Note on"; break;
        case MidiType::NoteOff:       kind = "Note off"; break;
        case MidiType::ControlChange: kind = "CC"; break;
        case MidiType::ProgramChange: kind = "Program"; break;
    }
    if (message.type == MidiType::ProgramChange) {
        std::snprintf(text, sizeof(text), "%s ch%d %d (%s)", kind, message.channel, message.number,
                      device.c_str());
    } else {
        std::snprintf(text, sizeof(text), "%s ch%d %d = %d (%s)", kind, message.channel,
                      message.number, message.value, device.c_str());
    }
    return text;
}

void ControlDispatcher::dispatch(const ControlEvent& event, std::vector<MidiMapping>& mappings,
                                 double now) {
    if (event.source == ControlEvent::Source::Osc) {
        dispatchOsc(event.osc, now);
    } else {
        // device is NUL-terminated by construction; bound the scan regardless.
        const auto end = std::find(event.device.begin(), event.device.end(), '\0');
        dispatchMidi(event.midi, std::string(event.device.begin(), end), mappings, now);
    }
}

// ---- Target operations --------------------------------------------------------

void ControlDispatcher::fire(const ControlRegistry::Resolved& r) {
    const ControlTarget& t = *r.target;
    switch (t.kind) {
        case TargetKind::Trigger: if (t.apply) t.apply(r.entityId, 1.0f); break;
        case TargetKind::Bool:    toggle(r); break;
        case TargetKind::Float:   if (t.apply) t.apply(r.entityId, t.max); break;
        case TargetKind::Index:   break;  // needs a number
    }
}

bool ControlDispatcher::setValue(const ControlRegistry::Resolved& r, float value) {
    if (!std::isfinite(value)) return false;
    const ControlTarget& t = *r.target;
    switch (t.kind) {
        case TargetKind::Trigger:
            if (value >= 0.5f && t.apply) t.apply(r.entityId, 1.0f);
            return true;
        case TargetKind::Bool:
            if (t.apply) t.apply(r.entityId, value >= 0.5f ? 1.0f : 0.0f);
            return true;
        case TargetKind::Float:
            if (t.apply) t.apply(r.entityId, std::clamp(value, t.min, t.max));
            return true;
        case TargetKind::Index: {
            if (value < t.min - 0.5f || value > 1.0e6f) return false;
            const float index = std::round(value);
            if (t.apply) t.apply(r.entityId, index);
            return true;
        }
    }
    return false;
}

void ControlDispatcher::setOnOff(const ControlRegistry::Resolved& r, bool on) {
    const ControlTarget& t = *r.target;
    switch (t.kind) {
        case TargetKind::Trigger: if (on && t.apply) t.apply(r.entityId, 1.0f); break;
        case TargetKind::Bool:    if (t.apply) t.apply(r.entityId, on ? 1.0f : 0.0f); break;
        case TargetKind::Float:   if (t.apply) t.apply(r.entityId, on ? t.max : t.min); break;
        case TargetKind::Index:   break;
    }
}

void ControlDispatcher::toggle(const ControlRegistry::Resolved& r) {
    const ControlTarget& t = *r.target;
    if (t.kind == TargetKind::Trigger) {
        if (t.apply) t.apply(r.entityId, 1.0f);
        return;
    }
    if (t.kind == TargetKind::Index) return;
    const std::optional<float> current = t.read ? t.read(r.entityId) : std::nullopt;
    const float middle = t.kind == TargetKind::Bool ? 0.5f : 0.5f * (t.min + t.max);
    const bool isOn = current && *current >= middle;
    setOnOff(r, !isOn);
}

void ControlDispatcher::noteActivity(const ControlRegistry::Resolved& r, double now) {
    activity_[r.canonicalPath] = now;
}

// ---- OSC ------------------------------------------------------------------------

void ControlDispatcher::dispatchOsc(const OscMessage& message, double now) {
    ++oscMessages_;
    lastOscTime_ = now;
    pushRecent(recentOsc_, describe(message), kRecentMessages);

    const auto resolved = registry_.resolve(message.address);
    if (!resolved) {
        ++unknownAddresses_;
        auto it = std::find(recentUnknown_.begin(), recentUnknown_.end(), message.address);
        if (it != recentUnknown_.end()) recentUnknown_.erase(it);
        pushRecent(recentUnknown_, message.address, kRecentUnknown);
        return;
    }

    const TargetKind kind = resolved->target->kind;
    if (message.args.empty()) {
        if (kind == TargetKind::Float || kind == TargetKind::Index) {
            ++badArguments_;  // a value target needs its value
            return;
        }
        fire(*resolved);
        noteActivity(*resolved, now);
        return;
    }

    const OscArg& arg = message.args.front();
    float value = 0.0f;
    switch (arg.type) {
        case OscArg::Type::Int:   value = static_cast<float>(arg.i); break;
        case OscArg::Type::Float: value = arg.f; break;
        case OscArg::Type::True:  value = 1.0f; break;
        case OscArg::Type::False: value = 0.0f; break;
        case OscArg::Type::String:
        case OscArg::Type::Unsupported:
            ++badArguments_;
            return;
    }
    if (!setValue(*resolved, value)) {
        ++badArguments_;
        return;
    }
    noteActivity(*resolved, now);
}

// ---- MIDI -----------------------------------------------------------------------

void ControlDispatcher::dispatchMidi(const MidiMessage& message, const std::string& device,
                                     std::vector<MidiMapping>& mappings, double now) {
    ++midiMessages_;
    lastMidiTime_ = now;
    lastMidi_ = describe(message, device);

    if (learnTarget_) {
        if (learn(message, device, mappings)) {
            if (lastLearnedIndex_ >= 0) {
                MidiMapping& m = mappings[static_cast<size_t>(lastLearnedIndex_)];
                m.lastActivity = now;
                m.lastValue = message.value;
                m.ccHigh = message.type == MidiType::ControlChange && message.value >= 64;
            }
            return;  // the binding control does not also drive its target
        }
    }

    for (auto& mapping : mappings) {
        if (!ControlRegistry::equalsIgnoreCase(mapping.device, "any") &&
            mapping.device != device) {
            continue;
        }
        if (mapping.channel != 0 && mapping.channel != message.channel) continue;
        if (mapping.number != message.number) continue;
        const bool typeMatches =
            (mapping.type == MidiControlType::Note &&
             (message.type == MidiType::NoteOn || message.type == MidiType::NoteOff)) ||
            (mapping.type == MidiControlType::CC && message.type == MidiType::ControlChange) ||
            (mapping.type == MidiControlType::Program && message.type == MidiType::ProgramChange);
        if (!typeMatches) continue;
        applyMapping(mapping, message, now);
    }
}

bool ControlDispatcher::learn(const MidiMessage& message, const std::string& device,
                              std::vector<MidiMapping>& mappings) {
    MidiControlType type;
    switch (message.type) {
        case MidiType::NoteOn:        type = MidiControlType::Note; break;
        case MidiType::ControlChange: type = MidiControlType::CC; break;
        case MidiType::ProgramChange: type = MidiControlType::Program; break;
        case MidiType::NoteOff:
        default:
            return false;  // keep waiting: a note-off is the tail of an earlier press
    }

    const std::string target = *learnTarget_;
    learnTarget_.reset();
    const auto resolved = registry_.resolve(target);
    if (!resolved) {
        Logger::warning("Control: MIDI learn target {} no longer exists", target);
        lastLearnedIndex_ = -1;
        return true;
    }

    MidiMapping mapping;
    mapping.device = device.empty() ? std::string("any") : device;
    mapping.type = type;
    mapping.channel = message.channel;
    mapping.number = message.number;
    mapping.target = resolved->canonicalPath;
    mapping.mode = defaultMode(type, resolved->target->kind);

    for (size_t i = 0; i < mappings.size(); ++i) {
        const MidiMapping& m = mappings[i];
        if (m.device == mapping.device && m.type == mapping.type && m.channel == mapping.channel &&
            m.number == mapping.number && m.target == mapping.target) {
            lastLearnedIndex_ = static_cast<int>(i);  // already bound: keep it as it is
            return true;
        }
    }
    mappings.push_back(mapping);
    lastLearnedIndex_ = static_cast<int>(mappings.size() - 1);
    Logger::info("Control: learned {} {} ch{} -> {} ({})", toString(type), mapping.number,
                 mapping.channel, mapping.target, toString(mapping.mode));
    return true;
}

void ControlDispatcher::applyMapping(MidiMapping& mapping, const MidiMessage& message, double now) {
    bool press = false;
    bool release = false;
    int value7 = 0;
    switch (message.type) {
        case MidiType::NoteOn:
            press = true;
            value7 = message.value;
            break;
        case MidiType::NoteOff:
            release = true;
            value7 = 0;
            break;
        case MidiType::ControlChange: {
            const bool high = message.value >= 64;
            press = high && !mapping.ccHigh;
            release = !high && mapping.ccHigh;
            mapping.ccHigh = high;
            value7 = message.value;
            break;
        }
        case MidiType::ProgramChange:
            press = true;
            value7 = 127;
            break;
    }
    mapping.lastActivity = now;
    mapping.lastValue = message.value;

    const auto resolved = registry_.resolve(mapping.target);
    if (!resolved) return;  // unresolved (deleted quad/scene): kept, does nothing
    const TargetKind kind = resolved->target->kind;

    bool acted = false;
    switch (mapping.mode) {
        case MidiMode::Trigger:
            if (press) { fire(*resolved); acted = true; }
            break;
        case MidiMode::Toggle:
            if (press) { toggle(*resolved); acted = true; }
            break;
        case MidiMode::Momentary:
            if (press || release) { setOnOff(*resolved, press); acted = true; }
            break;
        case MidiMode::Value:
            if (kind == TargetKind::Trigger) {
                if (press) { fire(*resolved); acted = true; }
            } else if (kind == TargetKind::Bool) {
                const bool on = mapping.type == MidiControlType::CC ? value7 >= 64 : !release;
                setOnOff(*resolved, on);
                acted = true;
            } else if (kind == TargetKind::Float) {
                const ControlTarget& t = *resolved->target;
                acted = setValue(*resolved, t.min + (static_cast<float>(value7) / 127.0f) * (t.max - t.min));
            }
            break;
    }
    if (acted) noteActivity(*resolved, now);
}

}  // namespace quadmapper::control
