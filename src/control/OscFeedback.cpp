#include "OscFeedback.hpp"

#include <cstring>
#include <unordered_set>

namespace quadmapper::control {

bool OscFeedback::sameValue(const OscArg& a, const OscArg& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case OscArg::Type::Int:    return a.i == b.i;
        case OscArg::Type::Float:  return std::memcmp(&a.f, &b.f, sizeof(float)) == 0;  // bit-exact
        case OscArg::Type::String: return a.s == b.s;
        case OscArg::Type::True:
        case OscArg::Type::False:
        case OscArg::Type::Unsupported:
            return true;
    }
    return false;
}

bool OscFeedback::needsSend(const Entry& e) {
    return e.force || !e.everSent || !sameValue(e.value, e.sent);
}

void OscFeedback::setValues(const std::vector<Sample>& samples) {
    std::unordered_set<std::string> seen;
    seen.reserve(samples.size());
    for (const Sample& sample : samples) {
        seen.insert(sample.key);
        Entry& e = entries_[sample.key];
        if (e.address != sample.address) {
            e.address = sample.address;
            e.everSent = false;
        }
        e.value = sample.value;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (seen.count(it->first)) ++it;
        else it = entries_.erase(it);
    }
}

void OscFeedback::markKnown(const std::string& key, const OscArg& value) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    it->second.sent = value;
    it->second.everSent = true;
}

void OscFeedback::requestSendAll() {
    for (auto& [key, e] : entries_) e.force = true;
}

size_t OscFeedback::collect(double now, std::vector<OscMessage>& out, size_t maxMessages) {
    size_t added = 0;
    // Start where the last capped call stopped, so a busy head cannot starve the rest.
    auto it = resumeKey_.empty() ? entries_.begin() : entries_.lower_bound(resumeKey_);
    resumeKey_.clear();
    for (size_t visited = 0; visited < entries_.size(); ++visited, ++it) {
        if (it == entries_.end()) it = entries_.begin();
        Entry& e = it->second;
        if (added >= maxMessages) {
            resumeKey_ = it->first;
            break;
        }
        if (!needsSend(e)) continue;
        if (e.sentOnce && now - e.lastSendTime < minInterval_) continue;  // coalesce until due
        OscMessage message;
        message.address = e.address;
        message.args.push_back(e.value);
        out.push_back(std::move(message));
        e.sent = e.value;
        e.everSent = true;
        e.force = false;
        e.lastSendTime = now;
        e.sentOnce = true;
        ++added;
    }
    return added;
}

void OscFeedback::reset() {
    for (auto& [key, e] : entries_) {
        e.everSent = false;
        e.sentOnce = false;
        e.force = false;
    }
}

size_t OscFeedback::pendingCount() const {
    size_t n = 0;
    for (const auto& [key, e] : entries_) {
        if (needsSend(e)) ++n;
    }
    return n;
}

}  // namespace quadmapper::control
