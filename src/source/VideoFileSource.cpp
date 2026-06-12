#include "VideoFileSource.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

VideoFileSource::VideoFileSource(const std::filesystem::path& path)
    : path_(path)
    , name_(path.filename().string())
    , player_(std::make_unique<VideoPlayer>())
{
}

VideoFileSource::~VideoFileSource() {
    shutdown();
}

bool VideoFileSource::initialize() {
    if (ready_) {
        return true;
    }

    if (!player_->load(path_)) {
        Logger::error("VideoFileSource: Failed to load '{}'", path_.string());
        return false;
    }

    // Pre-allocate frame buffer based on video dimensions
    const VideoInfo& info = player_->getVideoInfo();
    frame_.allocate(info.width, info.height, 3);  // RGB24

    ready_ = true;

    Logger::debug("VideoFileSource: Loaded '{}' ({}x{}, {:.2f}s, {:.1f}fps)",
                  name_, info.width, info.height, info.duration, info.frameRate);

    return true;
}

void VideoFileSource::shutdown() {
    if (player_) {
        player_->unload();
    }
    frame_.clear();
    ready_ = false;
    hasNewFrame_ = false;
}

bool VideoFileSource::update(double deltaTime) {
    if (!ready_ || !player_) {
        return false;
    }

    // Update the video player (advances playback)
    bool newFrame = player_->update(deltaTime);

    if (newFrame && player_->hasNewFrame()) {
        // Convert VideoFrame to FrameData
        const VideoFrame& vf = player_->getCurrentFrame();
        if (vf.isValid()) {
            convertVideoFrame(vf);
            player_->consumeFrame();
            hasNewFrame_ = true;
            return true;
        }
    }

    return false;
}

const FrameData* VideoFileSource::getCurrentFrame() const {
    if (!ready_ || !frame_.isValid()) {
        return nullptr;
    }
    return &frame_;
}

void VideoFileSource::markFrameConsumed() {
    hasNewFrame_ = false;
}

SourceCapabilities VideoFileSource::getCapabilities() const {
    SourceCapabilities caps;
    caps.isStatic = false;          // Changes over time
    caps.hasPlayback = true;        // Supports play/pause/seek
    caps.hasParameters = false;     // No adjustable parameters
    caps.supportsAsync = true;      // Background decoding
    caps.isGPUNative = false;       // CPU-based source
    return caps;
}

int VideoFileSource::getWidth() const {
    if (player_ && player_->isLoaded()) {
        return player_->getVideoInfo().width;
    }
    return 0;
}

int VideoFileSource::getHeight() const {
    if (player_ && player_->isLoaded()) {
        return player_->getVideoInfo().height;
    }
    return 0;
}

// --- PlayableSource Implementation ---

void VideoFileSource::play() {
    if (player_) {
        player_->play();
    }
}

void VideoFileSource::pause() {
    if (player_) {
        player_->pause();
    }
}

void VideoFileSource::stop() {
    if (player_) {
        player_->stop();
    }
}

void VideoFileSource::togglePlayPause() {
    if (player_) {
        player_->togglePlayPause();
    }
}

void VideoFileSource::seek(double timeSeconds) {
    if (player_) {
        player_->seek(timeSeconds);
    }
}

void VideoFileSource::seekProgress(float progress) {
    if (player_) {
        player_->seekProgress(progress);
    }
}

PlaybackState VideoFileSource::getPlaybackState() const {
    if (player_) {
        return player_->getState().state;
    }
    return PlaybackState::STOPPED;
}

double VideoFileSource::getCurrentTime() const {
    if (player_) {
        return player_->getState().position;
    }
    return 0.0;
}

double VideoFileSource::getDuration() const {
    if (player_) {
        return player_->getState().duration;
    }
    return 0.0;
}

float VideoFileSource::getProgress() const {
    if (player_) {
        return player_->getState().getProgress();
    }
    return 0.0f;
}

void VideoFileSource::setSpeed(float speed) {
    if (player_) {
        player_->setSpeed(speed);
    }
}

float VideoFileSource::getSpeed() const {
    if (player_) {
        return player_->getSpeed();
    }
    return 1.0f;
}

void VideoFileSource::setLoopMode(LoopMode mode) {
    if (player_) {
        player_->setLoopMode(mode);
    }
}

LoopMode VideoFileSource::getLoopMode() const {
    if (player_) {
        return player_->getLoopMode();
    }
    return LoopMode::ONCE;
}

const VideoInfo& VideoFileSource::getVideoInfo() const {
    static VideoInfo empty;
    if (player_ && player_->isLoaded()) {
        return player_->getVideoInfo();
    }
    return empty;
}

void VideoFileSource::convertVideoFrame(const VideoFrame& videoFrame) {
    // Ensure frame buffer is correct size
    if (frame_.width != videoFrame.width ||
        frame_.height != videoFrame.height) {
        frame_.allocate(videoFrame.width, videoFrame.height, 3);
    }

    // Copy pixel data (both are RGB24)
    size_t dataSize = frame_.getDataSize();
    if (videoFrame.data.size() >= dataSize) {
        std::memcpy(frame_.pixels.data(), videoFrame.data.data(), dataSize);
    }

    frame_.timestamp = static_cast<uint64_t>(videoFrame.pts * 1000000.0);  // Convert to microseconds
    frame_.frameNumber = static_cast<uint64_t>(videoFrame.frameNumber);
}

} // namespace quadmapper
