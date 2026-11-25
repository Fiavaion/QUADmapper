#include "ControlDispatcher.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace quadmapper::control {

namespace {

constexpr size_t kMaxDescribedArgs = 16;

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

void ControlDispatcher::noteRefused(const std::string& address, const std::string& reason) {
    ++refusals_;
    lastRefused_ = address + ": " + reason;
    if (warnedRefused_.size() >= kMaxWarnedAddresses) {
        if (!warnedRefusedFull_) {
            warnedRefusedFull_ = true;
            Logger::warning("Control: too many distinct refused addresses; further refusals are not logged");
        }
        return;
    }
    if (warnedRefused_.insert(address).second) Logger::warning("Control: {} refused ({})", address, reason);
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
    size_t shown = 0;
    for (const auto& arg : message.args) {
        if (shown++ == kMaxDescribedArgs) {
            text += " ...";
            break;
        }
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
        dispatchOsc(event, now);
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

void ControlDispatcher::dispatchOsc(const ControlEvent& event, double now) {
    const OscMessage& message = event.osc;
    ++oscMessages_;
    pushRecent(recentOsc_, describe(message), kRecentMessages);

    // One target for a plain address, every match for a pattern (/qm/quad/*/opacity).
    std::vector<ControlRegistry::Resolved> targets = registry_.resolveAll(message.address);
    if (targets.empty()) {
        ++unknownAddresses_;
        auto it = std::find(recentUnknown_.begin(), recentUnknown_.end(), message.address);
        if (it != recentUnknown_.end()) recentUnknown_.erase(it);
        pushRecent(recentUnknown_, message.address, kRecentUnknown);
        return;
    }

    // A pattern naming several scene takes (exclusive targets) takes none of them; the
    // other targets it matched still act.
    const auto exclusiveCount = std::count_if(targets.begin(), targets.end(),
                                              [](const auto& r) { return r.target->exclusive; });
    if (exclusiveCount > 1) {
        noteRefused(message.address, "matches " + std::to_string(exclusiveCount) +
                                         " scene takes; address a single scene");
        targets.erase(std::remove_if(targets.begin(), targets.end(),
                                     [](const auto& r) { return r.target->exclusive; }),
                      targets.end());
        if (targets.empty()) return;
    }

    // A message some target rejected counts once as a bad argument; the targets that
    // accepted it still act.
    bool rejected = false;
    if (message.args.empty()) {
        for (const auto& r : targets) {
            const TargetKind kind = r.target->kind;
            if (kind == TargetKind::Float || kind == TargetKind::Index) {
                rejected = true;  // a value target needs its value
                continue;
            }
            fire(r);
            noteActivity(r, now);  // no hook: the sender does not know the new state
        }
        if (rejected) ++badArguments_;
        return;
    }

    const OscArg& arg = message.args.front();
    float value = 0.0f;
    bool numeric = true;
    switch (arg.type) {
        case OscArg::Type::Int:   value = static_cast<float>(arg.i); break;
        case OscArg::Type::Float: value = arg.f; break;
        case OscArg::Type::True:  value = 1.0f; break;
        case OscArg::Type::False: value = 0.0f; break;
        case OscArg::Type::String:
        case OscArg::Type::Unsupported:
            numeric = false;
            break;
    }
    for (const auto& r : targets) {
        const bool applied = r.target->applyOscArgs ? r.target->applyOscArgs(r.entityId, message.args)
                                                    : numeric && setValue(r, value);
        if (!applied) {
            rejected = true;
            continue;
        }
        noteActivity(r, now);
        if (oscApplied_) oscApplied_(r, value, event.oscSenderIp, event.oscSenderPort);
    }
    if (rejected) ++badArguments_;
}

// ---- MIDI -----------------------------------------------------------------------

void ControlDispatcher::dispatchMidi(const MidiMessage& message, const std::string& device,
                                     std::vector<MidiMapping>& mappings, double now) {
    ++midiMessages_;
    lastMidiTime_ = now;
    lastMidi_ = describe(message, device);

    // The second half of a 14-bit pair just learned completes it (and is not applied).
    if (pendingPair_ && completePair(message, device, mappings, now)) return;

    if (learnTarget_) {
        if (learn(message, device, mappings, now)) {
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
        if (mapping.is14Bit()) {
            if (message.type != MidiType::ControlChange) continue;
            if (message.number != mapping.number &&
                message.number != mapping.number + k14BitLsbOffset) {
                continue;
            }
            applyMapping(mapping, message, now);
            continue;
        }
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

bool ControlDispatcher::completePair(const MidiMessage& message, const std::string& device,
                                     std::vector<MidiMapping>& mappings, double now) {
    const PendingPair pair = *pendingPair_;
    if (now - pair.time > k14BitPairSeconds) {
        pendingPair_.reset();
        return false;
    }
    if (message.type != MidiType::ControlChange || message.channel != pair.channel) return false;
    if ((device.empty() ? std::string("any") : device) != pair.device) return false;

    int msbNumber = 0;
    int msbValue = 0;
    int lsbValue = 0;
    bool lsbLast = false;  // the LSB is the message completing the pair
    if (pair.number <= k14BitMaxMsbNumber && message.number == pair.number + k14BitLsbOffset) {
        msbNumber = pair.number;
        msbValue = pair.value;
        lsbValue = message.value;
        lsbLast = true;
    } else if (pair.number >= k14BitLsbOffset && pair.number <= k14BitMaxMsbNumber + k14BitLsbOffset &&
               message.number == pair.number - k14BitLsbOffset) {
        msbNumber = message.number;
        msbValue = message.value;
        lsbValue = pair.value;
    } else {
        return false;
    }
    pendingPair_.reset();

    // The mapping must still be the one learned (the panel or a project load may
    // have changed the list since); otherwise the message is handled normally.
    if (pair.index >= mappings.size()) return false;
    {
        const MidiMapping& m = mappings[pair.index];
        if (m.device != pair.device || m.type != MidiControlType::CC || m.channel != pair.channel ||
            m.number != pair.number || m.target != pair.target) {
            return false;
        }
    }

    size_t index = pair.index;
    mappings[index].number = msbNumber;
    mappings[index].bits = 14;
    // An LSB learned first, renumbered to its MSB, may now repeat an existing mapping
    // of that MSB to the same target: keep that one (as 14-bit) and drop this.
    for (size_t i = 0; i < mappings.size(); ++i) {
        const MidiMapping& m = mappings[i];
        if (i == index || m.device != pair.device || m.type != MidiControlType::CC ||
            m.channel != pair.channel || m.number != msbNumber || m.target != pair.target) {
            continue;
        }
        mappings[i].bits = 14;
        mappings.erase(mappings.begin() + static_cast<std::ptrdiff_t>(index));
        index = i > index ? i - 1 : i;
        break;
    }

    MidiMapping& m = mappings[index];
    m.msb = msbValue;
    m.lsb = lsbValue;
    m.lsbSinceMsb = lsbLast;
    m.lsbTime = lsbLast ? now : -1.0;
    m.ccHigh = msbValue >= 64;
    m.lastValue = (msbValue << 7) | lsbValue;
    m.lastActivity = now;
    lastLearnedIndex_ = static_cast<int>(index);
    Logger::info("Control: CC {} + {} ch{} is a 14-bit pair -> {}", msbNumber, msbNumber + k14BitLsbOffset,
                 m.channel, m.target);
    return true;
}

bool ControlDispatcher::learn(const MidiMessage& message, const std::string& device,
                              std::vector<MidiMapping>& mappings, double now) {
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

    // A CC that can be half of a 14-bit pair waits briefly for the other half.
    auto awaitPair = [&](size_t index) {
        if (type != MidiControlType::CC || message.number > k14BitMaxMsbNumber + k14BitLsbOffset) return;
        pendingPair_ = PendingPair{index, mapping.device, mapping.target, mapping.channel,
                                   mapping.number, message.value, now};
    };

    for (size_t i = 0; i < mappings.size(); ++i) {
        const MidiMapping& m = mappings[i];
        if (m.device == mapping.device && m.type == mapping.type && m.channel == mapping.channel &&
            m.number == mapping.number && m.target == mapping.target) {
            lastLearnedIndex_ = static_cast<int>(i);  // already bound: keep it as it is
            awaitPair(i);
            return true;
        }
    }
    mappings.push_back(mapping);
    lastLearnedIndex_ = static_cast<int>(mappings.size() - 1);
    awaitPair(mappings.size() - 1);
    Logger::info("Control: learned {} {} ch{} -> {} ({})", toString(type), mapping.number,
                 mapping.channel, mapping.target, toString(mapping.mode));
    return true;
}

void ControlDispatcher::applyMapping(MidiMapping& mapping, const MidiMessage& message, double now) {
    bool press = false;
    bool release = false;
    int value = 0;      // the control's value, 0..full
    int full = 127;     // 16383 for a 14-bit CC
    auto edges = [&](int msb) {
        const bool high = msb >= 64;
        press = high && !mapping.ccHigh;
        release = !high && mapping.ccHigh;
        mapping.ccHigh = high;
    };
    switch (message.type) {
        case MidiType::NoteOn:
            press = true;
            value = message.value;
            break;
        case MidiType::NoteOff:
            release = true;
            value = 0;
            break;
        case MidiType::ControlChange:
            if (!mapping.is14Bit()) {
                edges(message.value);
                value = message.value;
                break;
            }
            full = 16383;
            if (message.number == mapping.number) {  // MSB (see MidiMapping.hpp)
                const bool keepLsb = mapping.lsbSinceMsb && now - mapping.lsbTime <= k14BitPairSeconds;
                if (!keepLsb) mapping.lsb = 0;
                mapping.msb = message.value;
                mapping.lsbSinceMsb = false;
                edges(mapping.msb);
            } else {  // LSB
                mapping.lsb = message.value;
                mapping.lsbSinceMsb = true;
                mapping.lsbTime = now;
                if (mapping.msb < 0) {  // no MSB yet: nothing to combine it with
                    mapping.lastActivity = now;
                    return;
                }
            }
            value = (mapping.msb << 7) | mapping.lsb;
            break;
        case MidiType::ProgramChange:
            press = true;
            value = 127;
            break;
    }
    mapping.lastActivity = now;
    mapping.lastValue = mapping.is14Bit() ? value : message.value;

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
                const bool on = mapping.type == MidiControlType::CC ? value >= (full + 1) / 2 : !release;
                setOnOff(*resolved, on);
                acted = true;
            } else if (kind == TargetKind::Float) {
                const ControlTarget& t = *resolved->target;
                const float fraction = static_cast<float>(value) / static_cast<float>(full);
                acted = setValue(*resolved, t.min + fraction * (t.max - t.min));
            }
            break;
    }
    if (acted) noteActivity(*resolved, now);
}

}  // namespace quadmapper::control
