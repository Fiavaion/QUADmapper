#pragma once

#include <filesystem>
#include <cstdint>

namespace quadmapper {

/**
 * @brief Encode a tightly-packed RGB24 buffer to a PNG file (via FFmpeg).
 *
 * @param rgb    Pointer to width*height*3 bytes, row-major, no padding.
 * @param width  Image width in pixels.
 * @param height Image height in pixels.
 * @param path   Output file path.
 * @return true on success.
 *
 * Used by QM-055 to write a captured camera frame for the AI sidecar. Takes raw
 * primitives (not FrameData) so the video library keeps no dependency on src/source.
 */
bool writeRgbPng(const uint8_t* rgb, int width, int height, const std::filesystem::path& path);

} // namespace quadmapper
