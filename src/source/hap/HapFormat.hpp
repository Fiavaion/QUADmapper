#pragma once

// HAP frame format types — no GL, no FFmpeg headers.
// Spec: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
//
// Section-type byte low-nibble (texture format):
//   0xB = RGB  DXT1/BC1       (Hap)
//   0xE = RGBA DXT5/BC3       (Hap Alpha)
//   0xF = scaled-YCoCg DXT5  (Hap Q)
//   0xC = RGBA BC7            (Hap R)
//   0x1 = Alpha RGTC1/BC4    (Hap Q Alpha plane)
//
// Section-type byte high-nibble (second-stage compressor):
//   0xA = None (raw BCn bytes)
//   0xB = Snappy
//   0xC = Complex / Decode Instructions Container

#include <cstdint>
#include <vector>

namespace quadmapper {
namespace hap {

/// Texture format carried inside the HAP section-type byte (low nibble).
/// Matches the spec table; value = low nibble of section-type byte.
enum class HapTextureFormat : uint8_t {
    None         = 0x00,  ///< Not set / invalid
    RGB_DXT1     = 0x0B,  ///< Hap       — BC1 (DXT1), RGB
    RGBA_DXT5    = 0x0E,  ///< Hap Alpha — BC3 (DXT5), RGBA
    YCoCg_DXT5   = 0x0F,  ///< Hap Q     — BC3 (DXT5), scaled YCoCg (needs shader conversion)
    Alpha_RGTC1  = 0x01,  ///< Hap Q Alpha (secondary alpha plane) — BC4
    RGBA_BPTC    = 0x0C,  ///< Hap R     — BC7 (BPTC)
};

/// Decoded result from parseHapFrame(). Contains raw BCn bytes ready for
/// glCompressedTexSubImage2D. No GL headers are included here.
struct HapDecodedFrame {
    HapTextureFormat format = HapTextureFormat::None;

    /// Raw BCn texture bytes — concatenated from all chunks.
    /// For HapQ Alpha frames the primary YCoCg plane is here; the alpha plane
    /// is deferred (see alphaPlane). Format RGBA_DXT5 applies to both planes.
    std::vector<uint8_t> textureData;

    /// Secondary alpha plane for Hap Q Alpha (BC4 bytes). Empty if not present.
    /// The caller uploads textureData with format YCoCg_DXT5 and alphaPlane
    /// with format Alpha_RGTC1 to two separate texture objects.
    std::vector<uint8_t> alphaPlane;

    bool valid() const {
        return format != HapTextureFormat::None && !textureData.empty();
    }
};

} // namespace hap
} // namespace quadmapper
