#include "UUID.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace quadmapper {

namespace {
// One generator per thread, so two threads never share its state, seeded with 256 bits:
// a single 32-bit seed allows only four billion different id sequences.
std::mt19937_64& generator() {
    thread_local std::mt19937_64 gen = [] {
        std::random_device rd;
        std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seed);
    }();
    return gen;
}
}

UUID::UUID() : high_(0), low_(0) {}

UUID::UUID(uint64_t high, uint64_t low) : high_(high), low_(low) {}

UUID UUID::generate() {
    std::mt19937_64& gen = generator();
    const uint64_t high = gen();
    return UUID(high, gen());
}

UUID UUID::fromString(const std::string& str) {
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx: hex digits, with the dashes in those four
    // places only. Anything looser would let two different strings name one id.
    if (str.length() != 36) {
        throw std::invalid_argument("Invalid UUID string length");
    }

    uint64_t halves[2] = {0, 0};
    int digits = 0;
    for (size_t i = 0; i < str.length(); ++i) {
        const char c = str[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') throw std::invalid_argument("Invalid UUID format");
            continue;
        }
        uint64_t value = 0;
        if (c >= '0' && c <= '9') value = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = static_cast<uint64_t>(c - 'A' + 10);
        else throw std::invalid_argument("Invalid UUID format");
        halves[digits / 16] = (halves[digits / 16] << 4) | value;
        ++digits;
    }

    return UUID(halves[0], halves[1]);
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
