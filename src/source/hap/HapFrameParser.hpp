#pragma once

// HAP frame parser — converts a single demuxed AVPacket byte buffer into raw
// BCn texture bytes. No GL, no FFmpeg headers included here.
//
// Spec: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
//
// Section-type byte values (spec table, Top-Level Section Types):
//   0xAB  RGB  DXT1/BC1  None             (Hap, uncompressed)
//   0xBB  RGB  DXT1/BC1  Snappy           (Hap, Snappy)
//   0xCB  RGB  DXT1/BC1  Decode Instruct. (Hap, chunked)
//   0xAE  RGBA DXT5/BC3  None             (Hap Alpha, uncompressed)
//   0xBE  RGBA DXT5/BC3  Snappy           (Hap Alpha, Snappy)
//   0xCE  RGBA DXT5/BC3  Decode Instruct. (Hap Alpha, chunked)
//   0xAF  YCoCg DXT5     None             (Hap Q, uncompressed)
//   0xBF  YCoCg DXT5     Snappy           (Hap Q, Snappy)
//   0xCF  YCoCg DXT5     Decode Instruct. (Hap Q, chunked)
//   0xAC  RGBA  BC7      None             (Hap R, uncompressed)
//   0xBC  RGBA  BC7      Snappy           (Hap R, Snappy)
//   0xCC  RGBA  BC7      Decode Instruct. (Hap R, chunked)
//   0xA1  Alpha RGTC1/BC4 None            (Hap Q Alpha plane, uncompressed)
//   0xB1  Alpha RGTC1/BC4 Snappy          (Hap Q Alpha plane, Snappy)
//   0xC1  Alpha RGTC1/BC4 Decode Instruct.(Hap Q Alpha plane, chunked)
//   0x0D  Multiple images — Hap Q Alpha (primary YCoCg + secondary BC4 alpha)
//
// Decode Instructions Container subsection types:
//   0x01  Decode Instructions Container
//   0x02  Chunk Second-Stage Compressor Table (one byte/chunk: 0x0A=raw, 0x0B=Snappy)
//   0x03  Chunk Size Table (4 bytes/chunk, LE uint32)
//   0x04  Chunk Offset Table (optional, 4 bytes/chunk, LE uint32)

#include "source/hap/HapFormat.hpp"

#include <cstdint>
#include <cstddef>

namespace quadmapper {
namespace hap {

/// Parse one HAP packet (the raw bytes of a single demuxed AVPacket).
///
/// Returns a valid HapDecodedFrame on success. Returns an invalid frame
/// (valid() == false) for any malformed, truncated, or unsupported input —
/// never throws, never crashes, never allocates based on unvalidated sizes.
HapDecodedFrame parseHapFrame(const uint8_t* data, size_t size);

/// The same, into `out`, whose buffers are cleared and reused rather than freed, so
/// playback decodes each frame into memory it already has. Returns out.valid().
bool parseHapFrame(const uint8_t* data, size_t size, HapDecodedFrame& out);

} // namespace hap
} // namespace quadmapper
