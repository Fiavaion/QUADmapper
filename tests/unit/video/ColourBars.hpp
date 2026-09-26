#pragma once

// Reference colour bars and the forward R'G'B' -> Y'CbCr transform (ADR-055 colour
// gate). Tests build YUV pictures from these with the matrix and range under test and
// expect the GPU conversion to give the reference RGB back within +-2 per channel.

#include "core/YuvPicture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace quadmapper::colourbars {

struct Rgb {
    int r, g, b;
};

// White, 75% bars (yellow, cyan, green, magenta, red, blue), black, and a mid grey:
// saturated primaries and secondaries exercise every matrix coefficient.
constexpr std::array<Rgb, 9> BARS = {{
    {255, 255, 255}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191},
    {191, 0, 0},     {0, 0, 191},   {0, 0, 0},     {128, 128, 128},
}};

struct Yuv {
    int y, cb, cr;
};

// Integer Y'CbCr code values of an 8-bit R'G'B' colour at `bits` (8 or 10).
inline Yuv toYuv(const Rgb& c, YuvMatrix matrix, bool fullRange, int bits = 8) {
    double kr = 0.2126, kb = 0.0722;
    if (matrix == YuvMatrix::BT601) { kr = 0.299; kb = 0.114; }
    if (matrix == YuvMatrix::BT2020) { kr = 0.2627; kb = 0.0593; }
    const double kg = 1.0 - kr - kb;
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double y = kr * r + kg * g + kb * b;
    const double cb = (b - y) / (2.0 * (1.0 - kb));
    const double cr = (r - y) / (2.0 * (1.0 - kr));
    const double step = std::ldexp(1.0, bits - 8);  // one 8-bit code value
    const double maxValue = std::ldexp(1.0, bits) - 1.0;
    auto code = [maxValue](double v) {
        return static_cast<int>(std::clamp(std::lround(v), 0L, static_cast<long>(maxValue)));
    };
    if (fullRange) {
        return {code(y * maxValue), code(128.0 * step + cb * maxValue), code(128.0 * step + cr * maxValue)};
    }
    return {code(16.0 * step + 219.0 * step * y), code(128.0 * step + 224.0 * step * cb),
            code(128.0 * step + 224.0 * step * cr)};
}

inline bool within(const Rgb& a, const Rgb& b, int tolerance) {
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance &&
           std::abs(a.b - b.b) <= tolerance;
}

}  // namespace quadmapper::colourbars
