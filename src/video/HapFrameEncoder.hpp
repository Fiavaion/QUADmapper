#pragma once

// HAP frame encoder (QM-082) — pure CPU, no GL, no FFmpeg.
// Spec: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
//
// The inverse of source/hap/HapFrameParser: an RGBA image becomes one HAP frame
// (a section header followed by BC1 or scaled-YCoCg BC3 texture data, Snappy-
// compressed when that makes it smaller). The linked FFmpeg is built without
// libsnappy, so it has no HAP encoder; this is ours.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quadmapper {
namespace hap {

enum class HapVariant {
    Hap,   // BC1 (DXT1), RGB, 4 bits per pixel. Codec tag 'Hap1'.
    HapQ,  // scaled YCoCg in BC3 (DXT5), 8 bits per pixel, closer to the source. 'HapY'.
};

/// Bytes of BCn texture data a frame holds: the image padded to whole 4x4 blocks.
size_t hapTextureSize(int width, int height, HapVariant variant);

/// Encode one RGBA image (stride bytes per row; alpha ignored) as a HAP frame.
/// Block compression is spread over `threads` threads (at least one).
/// Returns an empty vector for an empty image.
std::vector<uint8_t> encodeHapFrame(const uint8_t* rgba, int width, int height, size_t stride,
                                    HapVariant variant, int threads);

/// Snappy raw block format (no framing), as HAP stores it: a varint of the
/// uncompressed length, then literals and back-references within 64 KiB blocks.
std::vector<uint8_t> snappyCompress(const uint8_t* data, size_t size);

} // namespace hap
} // namespace quadmapper
