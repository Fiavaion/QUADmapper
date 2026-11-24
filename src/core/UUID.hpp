#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <array>

namespace quadmapper {

// UUID class for unique identifiers (128-bit)
class UUID {
public:
    UUID();
    explicit UUID(uint64_t high, uint64_t low);

    static UUID generate();

    // Parse from string
    static UUID fromString(const std::string& str);

    // Null UUID
    static UUID null();

    bool isNull() const;

    uint64_t high() const { return high_; }
    uint64_t low() const { return low_; }

    // Convert to string (format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    std::string toString() const;

    // Comparison operators
    bool operator==(const UUID& other) const {
        return high_ == other.high_ && low_ == other.low_;
    }
    bool operator!=(const UUID& other) const {
        return !(*this == other);
    }
    bool operator<(const UUID& other) const {
        if (high_ != other.high_) return high_ < other.high_;
        return low_ < other.low_;
    }

private:
    uint64_t high_;
    uint64_t low_;
};

} // namespace quadmapper

// Hash support for std::unordered_map
namespace std {
template<>
struct hash<quadmapper::UUID> {
    size_t operator()(const quadmapper::UUID& uuid) const {
        size_t h1 = hash<uint64_t>()(uuid.high());
        size_t h2 = hash<uint64_t>()(uuid.low());
        return h1 ^ (h2 << 1);
    }
};
} // namespace std
