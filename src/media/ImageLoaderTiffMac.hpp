#pragma once

#ifdef __APPLE__

#include "ImageLoader.hpp"

namespace quadmapper {

// The macOS half of the TIFF reader (ImageLoaderTiff_mac.cpp, ADR-062): the first frame,
// decoded by ImageIO, as straight-alpha RGBA with the top row first. An error carries
// the reason only; ImageLoader adds the file's name.
Result<ImageData, Error> macDecodeTiff(const std::filesystem::path& path);

}  // namespace quadmapper

#endif
