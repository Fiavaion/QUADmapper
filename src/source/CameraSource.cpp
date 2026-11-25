#include "CameraSource.hpp"
#include "CameraDevices.hpp"
#include "video/VideoDecoder.hpp"
#include "video/PngWriter.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace quadmapper {

CameraSource::CameraSource(std::string deviceName, int width, int height, double fps)
    : deviceName_(std::move(deviceName))
    , reqWidth_(width)
    , reqHeight_(height)
    , reqFps_(fps)
    , decoder_(std::make_unique<VideoDecoder>())
{
    name_ = deviceName_.empty() ? "Camera" : deviceName_;
}

CameraSource::~CameraSource() {
    shutdown();
}

bool CameraSource::initialize() {
    if (ready_) {
        return true;
    }

    // Only a device Windows lists is opened: the name can come from a project file,
    // and FFmpeg would read anything after a ':' in it as a second device to open.
    const auto devices = enumerateCameraDevices();
    std::string dev = deviceName_;
    if (dev.empty()) {
        if (devices.empty()) {
            Logger::error("CameraSource: no video capture devices found");
            return false;
        }
        dev = devices.front().name;
        deviceName_ = dev;
        name_ = dev;
    } else if (std::none_of(devices.begin(), devices.end(),
                            [&dev](const CameraDevice& d) { return d.name == dev; })) {
        Logger::error("CameraSource: '{}' is not a connected camera", dev);
        return false;
    }

    if (!decoder_->openCamera(dev, reqWidth_, reqHeight_, reqFps_)) {
        Logger::error("CameraSource: failed to open '{}'", dev);
        return false;
    }

    const VideoInfo& info = decoder_->getInfo();
    width_ = info.width;
    height_ = info.height;
    frame_.allocate(width_, height_, 3);  // RGB24

    decoder_->startBackgroundDecoding(8);
    ready_ = true;

    Logger::debug("CameraSource: opened '{}' ({}x{}, {} fps)",
                  dev, width_, height_, info.frameRate);
    return true;
}

void CameraSource::shutdown() {
    if (decoder_) {
        decoder_->stopBackgroundDecoding();
        decoder_->close();
    }
    frame_.clear();
    ready_ = false;
    hasNewFrame_ = false;
}

bool CameraSource::update(double /*deltaTime*/) {
    if (!ready_ || !decoder_) {
        return false;
    }

    // Live preview: drain the background buffer to the newest frame so we never
    // accumulate latency when the render loop is faster than the capture rate.
    VideoFrame latest;
    while (decoder_->hasBufferedFrames()) {
        VideoFrame f = decoder_->getBufferedFrame();
        if (f.isValid()) {
            latest = std::move(f);
        }
    }

    if (latest.isValid()) {
        convertFrame(latest);
        hasNewFrame_ = true;
        return true;
    }
    return false;
}

const FrameData* CameraSource::getCurrentFrame() const {
    if (!ready_ || !frame_.isValid()) {
        return nullptr;
    }
    return &frame_;
}

void CameraSource::markFrameConsumed() {
    hasNewFrame_ = false;
}

std::string CameraSource::getErrorMessage() const {
    return decoder_ ? decoder_->getLastError() : std::string();
}

SourceCapabilities CameraSource::getCapabilities() const {
    SourceCapabilities caps;
    caps.isStatic = false;
    caps.hasPlayback = false;   // live: no play/pause/seek
    caps.hasParameters = false;
    caps.supportsAsync = true;  // background decode
    caps.isGPUNative = false;
    return caps;
}

bool CameraSource::captureCurrentFrameToPng(const std::filesystem::path& path) const {
    if (!frame_.isValid()) {
        Logger::error("CameraSource: no frame available to capture");
        return false;
    }
    return writeRgbPng(frame_.pixels.data(), frame_.width, frame_.height, path);
}

void CameraSource::convertFrame(const VideoFrame& vf) {
    if (frame_.width != vf.width || frame_.height != vf.height) {
        frame_.allocate(vf.width, vf.height, 3);
        width_ = vf.width;
        height_ = vf.height;
    }
    const size_t dataSize = frame_.getDataSize();
    if (vf.data.size() >= dataSize) {
        std::memcpy(frame_.pixels.data(), vf.data.data(), dataSize);
    }
    frame_.timestamp = static_cast<uint64_t>(vf.pts * 1000000.0);
    frame_.frameNumber = static_cast<uint64_t>(vf.frameNumber);
}

} // namespace quadmapper
