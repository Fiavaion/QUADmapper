#pragma once

#include <string>
#include <vector>

namespace quadmapper {

/**
 * @brief A discovered video capture device.
 *
 * `name` is the DirectShow FriendlyName, which is also the identifier
 * FFmpeg's dshow input expects (i.e. "video=<name>").
 */
struct CameraDevice {
    std::string name;
};

/**
 * @brief Enumerate available video capture devices (Windows DirectShow).
 *
 * Returns an empty vector on non-Windows platforms or when no device is
 * present. Names round-trip directly into VideoDecoder::openCamera(). See QM-055.
 */
std::vector<CameraDevice> enumerateCameraDevices();

} // namespace quadmapper
