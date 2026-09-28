#include "OscWriter.hpp"

#include <cstring>
#include <string>

namespace quadmapper::control {

namespace {

void appendUint32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

// OSC-string: the bytes, a NUL, then NUL padding to a multiple of 4 counted from
// the packet's first byte at `start`.
void appendString(std::vector<uint8_t>& out, size_t start, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
    while ((out.size() - start) % 4 != 0) out.push_back(0);
}

bool hasNul(const std::string& text) {
    return text.find('\0') != std::string::npos;
}

}  // namespace

bool writeOscMessage(const OscMessage& message, std::vector<uint8_t>& out) {
    if (message.address.empty() || message.address[0] != '/' || hasNul(message.address)) return false;

    std::string tags = ",";
    for (const OscArg& arg : message.args) {
        switch (arg.type) {
            case OscArg::Type::Int:    tags += 'i'; break;
            case OscArg::Type::Float:  tags += 'f'; break;
            case OscArg::Type::String:
                if (hasNul(arg.s)) return false;
                tags += 's';
                break;
            case OscArg::Type::True:   tags += 'T'; break;
            case OscArg::Type::False:  tags += 'F'; break;
            case OscArg::Type::Unsupported:
                return false;
        }
    }

    const size_t start = out.size();
    appendString(out, start, message.address);
    appendString(out, start, tags);
    for (const OscArg& arg : message.args) {
        switch (arg.type) {
            case OscArg::Type::Int:
                appendUint32(out, static_cast<uint32_t>(arg.i));
                break;
            case OscArg::Type::Float: {
                uint32_t bits = 0;
                std::memcpy(&bits, &arg.f, sizeof(bits));
                appendUint32(out, bits);
                break;
            }
            case OscArg::Type::String:
                appendString(out, start, arg.s);
                break;
            case OscArg::Type::True:
            case OscArg::Type::False:
            case OscArg::Type::Unsupported:
                break;  // T and F carry no data; Unsupported was rejected above
        }
    }
    return true;
}

}  // namespace quadmapper::control
