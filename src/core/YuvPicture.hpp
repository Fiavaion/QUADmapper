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
 * @brief A decoded picture left on the GPU (ADR-056)
 *
 * An NV12 or P010 D3D11 texture on the decoders' D3D11 device, shared with a keyed mutex
 * (key 0) so the renderer opens it on its own device. Plain handles, so no D3D header
 * reaches this one.
 */
struct GpuSurface {
    void* texture = nullptr;         // ID3D11Texture2D* on `device`, alive while YuvPicture::owner is
    void* shared = nullptr;          // its shared HANDLE, for OpenSharedResource
    uint64_t serial = 0;             // names the texture for the life of the process
    void* device = nullptr;          // the decoders' ID3D11Device* (kept until the process exits)
};

/**
 * @brief A decoded picture kept as YUV planes for the GPU conversion pass (ADR-055)
 *
 * Nothing is converted or copied on the CPU: the planes point into the decoder's
 * buffers, which `owner` keeps alive. Copying a YuvPicture shares those buffers, so a
 * frame moves from the decode thread to the upload by handle. A picture left on the GPU
 * (ADR-056) has no planes in memory; `gpu` names its texture instead.
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
    GpuSurface gpu;              // set instead of the planes for a picture on the GPU

    bool onGpu() const { return gpu.texture != nullptr; }
    bool isValid() const {
        return layout != YuvLayout::None && width > 0 && height > 0 && (planes[0] != nullptr || onGpu());
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
