#include "OscParser.hpp"

#include <cstdint>
#include <cstring>

namespace quadmapper::control {

namespace {

// Every reader takes the element's bounds and a cursor `pos` with pos <= size, and
// either advances it past what it read or returns false leaving it unchanged.

bool readUint32(const uint8_t* data, size_t size, size_t& pos, uint32_t& out) {
    if (size - pos < 4) return false;
    out = (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
          (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
    pos += 4;
    return true;
}

bool skipBytes(size_t size, size_t& pos, size_t count) {
    if (size - pos < count) return false;
    pos += count;
    return true;
}

// OSC-string: bytes up to a NUL, then NUL padding to a multiple of 4. One longer than
// maxLength is refused before it is copied.
bool readString(const uint8_t* data, size_t size, size_t& pos, std::string* out,
                size_t maxLength = SIZE_MAX) {
    const void* nul = std::memchr(data + pos, 0, size - pos);
    if (!nul) return false;
    const size_t length = static_cast<size_t>(static_cast<const uint8_t*>(nul) - (data + pos));
    if (length > maxLength) return false;
    const size_t padded = (length + 4) & ~static_cast<size_t>(3);  // length + NUL, rounded up
    if (size - pos < padded) return false;
    if (out) out->assign(reinterpret_cast<const char*>(data + pos), length);
    pos += padded;
    return true;
}

// OSC-blob: int32 byte count, the bytes, then padding to a multiple of 4.
bool skipBlob(const uint8_t* data, size_t size, size_t& pos) {
    size_t cursor = pos;
    uint32_t raw = 0;
    if (!readUint32(data, size, cursor, raw)) return false;
    if (raw > 0x7FFFFFFFu) return false;  // negative int32
    const size_t padded = (static_cast<size_t>(raw) + 3) & ~static_cast<size_t>(3);
    if (!skipBytes(size, cursor, padded)) return false;
    pos = cursor;
    return true;
}

bool parseMessage(const uint8_t* data, size_t size, OscMessage& message) {
    size_t pos = 0;
    if (!readString(data, size, pos, &message.address, kMaxStringBytes)) return false;
    if (message.address.empty() || message.address[0] != '/') return false;
    if (pos == size) return true;  // no type-tag string: no arguments (old senders)

    std::string tags;
    if (!readString(data, size, pos, &tags)) return false;
    if (tags.empty() || tags[0] != ',') return false;

    for (size_t t = 1; t < tags.size(); ++t) {
        OscArg arg;
        arg.tag = tags[t];
        uint32_t raw = 0;
        switch (arg.tag) {
            case 'i':
                if (!readUint32(data, size, pos, raw)) return false;
                arg.type = OscArg::Type::Int;
                arg.i = static_cast<int32_t>(raw);
                break;
            case 'f':
                if (!readUint32(data, size, pos, raw)) return false;
                arg.type = OscArg::Type::Float;
                std::memcpy(&arg.f, &raw, sizeof(arg.f));
                break;
            case 's':
                if (!readString(data, size, pos, &arg.s, kMaxStringBytes)) return false;
                arg.type = OscArg::Type::String;
                break;
            case 'T': arg.type = OscArg::Type::True; break;
            case 'F': arg.type = OscArg::Type::False; break;
            // Known sizes, not decoded (Unsupported keeps the argument's position).
            case 'S':
                if (!readString(data, size, pos, nullptr)) return false;
                break;
            case 'b':
                if (!skipBlob(data, size, pos)) return false;
                break;
            case 'h': case 't': case 'd':
                if (!skipBytes(size, pos, 8)) return false;
                break;
            case 'c': case 'r': case 'm':
                if (!skipBytes(size, pos, 4)) return false;
                break;
            case 'N': case 'I':
                break;
            case '[': case ']':
                continue;  // array delimiters carry no data and are not arguments
            default:
                return false;  // unknown size: the rest cannot be read safely
        }
        if (message.args.size() >= kMaxArgsPerMessage) return false;
        message.args.push_back(std::move(arg));
    }
    return true;
}

bool parseElement(const uint8_t* data, size_t size, int depth, std::vector<OscMessage>& out);

bool parseBundle(const uint8_t* data, size_t size, int depth, std::vector<OscMessage>& out) {
    if (depth >= kMaxBundleDepth) return false;
    size_t pos = 16;  // "#bundle\0" + 8-byte time tag (ignored: run on receipt)
    while (pos < size) {
        uint32_t elementSize = 0;
        if (!readUint32(data, size, pos, elementSize)) return false;
        if (elementSize == 0 || elementSize > size - pos) return false;
        if (!parseElement(data + pos, elementSize, depth + 1, out)) return false;
        pos += elementSize;
    }
    return true;
}

bool parseElement(const uint8_t* data, size_t size, int depth, std::vector<OscMessage>& out) {
    if (size == 0 || size % 4 != 0) return false;
    if (data[0] == '/') {
        if (out.size() >= kMaxMessagesPerPacket) return false;
        OscMessage message;
        if (!parseMessage(data, size, message)) return false;
        out.push_back(std::move(message));
        return true;
    }
    static const char kBundleTag[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    if (size >= 16 && std::memcmp(data, kBundleTag, sizeof(kBundleTag)) == 0) {
        return parseBundle(data, size, depth, out);
    }
    return false;
}

}  // namespace

bool parseOscPacket(const uint8_t* data, size_t size, std::vector<OscMessage>& out) {
    if (!data) return false;
    std::vector<OscMessage> parsed;
    if (!parseElement(data, size, 0, parsed)) return false;
    for (auto& message : parsed) out.push_back(std::move(message));
    return true;
}

}  // namespace quadmapper::control
