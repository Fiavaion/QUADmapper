#pragma once

#include <cstdint>
#include <memory>

namespace quadmapper {

// How the planes of a decoded YUV picture are laid out (ADR-055). Chroma is at half
// resolution in both directions for every layout.
enum class YuvLayout {
    None,
    NV12,       // Y plane + interleaved UV plane, 8-bit samples
    P010,       // NV12 with 16-bit samples, the 10 significant bits in the high bits
    YUV420P,    // separate Y, U and V planes, 8-bit samples
    YUV420P10,  // YUV420P with 16-bit samples, the 10 significant bits in the low bits
};

// Which Y'CbCr -> R'G'B' matrix the picture was encoded with.
enum class YuvMatrix { BT601, BT709, BT2020 };

/**
 * @brief A decoded picture kept as YUV planes for the GPU conversion pass (ADR-055)
 *
 * Nothing is converted or copied on the CPU: the planes point into the decoder's
 * buffers, which `owner` keeps alive. Copying a YuvPicture shares those buffers, so a
 * frame moves from the decode thread to the upload by handle.
 */
struct YuvPicture {
    YuvLayout layout = YuvLayout::None;
    int width = 0;
    int height = 0;
    const uint8_t* planes[3] = {nullptr, nullptr, nullptr};
    int strides[3] = {0, 0, 0};  // bytes per row of each plane
    YuvMatrix matrix = YuvMatrix::BT709;
    bool fullRange = false;      // false: limited ("TV") range, 16-235 for 8-bit luma
    std::shared_ptr<const void> owner;

    bool isValid() const {
        return layout != YuvLayout::None && width > 0 && height > 0 && planes[0] != nullptr;
    }
    bool semiPlanar() const { return layout == YuvLayout::NV12 || layout == YuvLayout::P010; }
    int planeCount() const { return semiPlanar() ? 2 : 3; }
    int bytesPerSample() const {
        return (layout == YuvLayout::P010 || layout == YuvLayout::YUV420P10) ? 2 : 1;
    }
    int chromaWidth() const { return (width + 1) / 2; }
    int chromaHeight() const { return (height + 1) / 2; }
};

}  // namespace quadmapper
