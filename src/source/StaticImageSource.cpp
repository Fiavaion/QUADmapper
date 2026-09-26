#include "StaticImageSource.hpp"
#include "media/ImageLoader.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

StaticImageSource::StaticImageSource(const std::filesystem::path& path)
    : path_(path)
    , name_(utf8FromPath(path.filename()))
{
}

StaticImageSource::~StaticImageSource() {
    shutdown();
}

bool StaticImageSource::initialize() {
    if (ready_) {
        return true;  // Already initialized
    }

    // Load image from disk
    auto result = ImageLoader::load(path_);
    if (!result.isSuccess()) {
        Logger::error("StaticImageSource: Failed to load '{}': {}",
                      utf8FromPath(path_), result.error().message);
        return false;
    }

    // Copy image data to frame
    const ImageData& imageData = result.value();
    frame_.width = imageData.width;
    frame_.height = imageData.height;
    frame_.channels = imageData.channels;
    frame_.pixels = std::move(result.value().pixels);
    frame_.timestamp = 0;
    frame_.frameNumber = 0;

    ready_ = true;
    hasNewFrame_ = true;
    firstFrame_ = true;

    Logger::debug("StaticImageSource: Loaded '{}' ({}x{}, {} ch)",
                  name_, frame_.width, frame_.height, frame_.channels);

    return true;
}

void StaticImageSource::shutdown() {
    frame_.clear();
    ready_ = false;
    hasNewFrame_ = false;
}

bool StaticImageSource::update(double /*deltaTime*/) {
    if (!ready_) {
        return false;
    }

    // Static images only report a new frame once (on first access)
    if (firstFrame_) {
        firstFrame_ = false;
        hasNewFrame_ = true;
        return true;
    }

    return false;  // No new frame (static content)
}

const FrameData* StaticImageSource::getCurrentFrame() const {
    if (!ready_ || !frame_.isValid()) {
        return nullptr;
    }
    return &frame_;
}

void StaticImageSource::markFrameConsumed() {
    hasNewFrame_ = false;
}

SourceCapabilities StaticImageSource::getCapabilities() const {
    SourceCapabilities caps;
    caps.isStatic = true;           // Never changes
    caps.hasPlayback = false;       // No play/pause
    caps.hasParameters = false;     // No adjustable parameters
    caps.supportsAsync = false;     // No background loading (could be added)
    caps.isGPUNative = false;       // CPU-based source
    return caps;
}

} // namespace quadmapper
