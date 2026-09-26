#pragma once

// Builds OSC 1.0 packets byte by byte for the parser and server tests.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace quadmapper::control::test {

class OscPacket {
public:
    OscPacket& string(const std::string& s) {
        bytes_.insert(bytes_.end(), s.begin(), s.end());
        bytes_.push_back(0);
        pad();
        return *this;
    }
    OscPacket& int32(int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        bytes_.push_back(static_cast<uint8_t>(u >> 24));
        bytes_.push_back(static_cast<uint8_t>(u >> 16));
        bytes_.push_back(static_cast<uint8_t>(u >> 8));
        bytes_.push_back(static_cast<uint8_t>(u));
        return *this;
    }
    OscPacket& float32(float f) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return int32(static_cast<int32_t>(u));
    }
    OscPacket& raw(const std::vector<uint8_t>& b) {
        bytes_.insert(bytes_.end(), b.begin(), b.end());
        return *this;
    }
    // A bundle element: int32 size, then the element's bytes.
    OscPacket& element(const OscPacket& inner) {
        int32(static_cast<int32_t>(inner.bytes_.size()));
        return raw(inner.bytes_);
    }
    OscPacket& bundleHeader() {
        string("#bundle");
        int32(0);
        return int32(1);  // time tag "immediately"
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    void pad() {
        while (bytes_.size() % 4 != 0) bytes_.push_back(0);
    }
    std::vector<uint8_t> bytes_;
};

// /address ,<tags> args... for the common cases.
inline OscPacket oscMessage(const std::string& address) {
    OscPacket p;
    p.string(address);
    return p;
}

}  // namespace quadmapper::control::test
