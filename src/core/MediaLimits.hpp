#pragma once

#include <string>

namespace quadmapper {

// The largest picture an image or video file may declare, checked before QUADmapper
// allocates for it. 32768 is the largest GL_MAX_TEXTURE_SIZE in use (NVIDIA);
// Texture::create checks the card's own. 134 MP still allows a 19200 x 2160
// five-projector panorama.
constexpr long long MAX_PICTURE_SIDE = 32768;
constexpr long long MAX_PICTURE_PIXELS = 1LL << 27;

// Empty when a picture of this size may be opened, else the reason it may not.
inline std::string pictureSizeProblem(long long width, long long height) {
    if (width <= MAX_PICTURE_SIDE && height <= MAX_PICTURE_SIDE &&
        width * height <= MAX_PICTURE_PIXELS) {
        return {};
    }
    return std::to_string(width) + " x " + std::to_string(height) +
           " is too large (at most 32768 pixels a side and 134 megapixels)";
}

}  // namespace quadmapper
