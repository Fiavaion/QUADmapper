#include "UUID.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace quadmapper {

namespace {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
}

UUID::UUID() : high_(0), low_(0) {}

UUID::UUID(uint64_t high, uint64_t low) : high_(high), low_(low) {}

UUID UUID::generate() {
    return UUID(dis(gen), dis(gen));
}

UUID UUID::fromString(const std::string& str) {
    // Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
    if (str.length() != 36) {
        throw std::invalid_argument("Invalid UUID string length");
    }

    // Remove dashes and parse as two 64-bit values
    std::string cleaned;
    for (char c : str) {
        if (c != '-') {
            cleaned += c;
        }
    }

    if (cleaned.length() != 32) {
        throw std::invalid_argument("Invalid UUID format");
    }

    uint64_t high = std::stoull(cleaned.substr(0, 16), nullptr, 16);
    uint64_t low = std::stoull(cleaned.substr(16, 16), nullptr, 16);

    return UUID(high, low);
}

UUID UUID::null() {
    return UUID(0, 0);
}

bool UUID::isNull() const {
    return high_ == 0 && low_ == 0;
}

std::string UUID::toString() const {
    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    // First 8 hex chars from high (bits 63-32)
    oss << std::setw(8) << ((high_ >> 32) & 0xFFFFFFFF);
    oss << '-';
    // Next 4 hex chars (bits 31-16)
    oss << std::setw(4) << ((high_ >> 16) & 0xFFFF);
    oss << '-';
    // Next 4 hex chars (bits 15-0)
    oss << std::setw(4) << (high_ & 0xFFFF);
    oss << '-';
    // Next 4 hex chars from low (bits 63-48)
    oss << std::setw(4) << ((low_ >> 48) & 0xFFFF);
    oss << '-';
    // Last 12 hex chars (bits 47-0)
    oss << std::setw(12) << (low_ & 0xFFFFFFFFFFFF);

    return oss.str();
}

} // namespace quadmapper
