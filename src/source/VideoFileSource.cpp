#include "VideoFileSource.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

VideoFileSource::VideoFileSource(const std::filesystem::path& path, const DecodeOptions& options)
    : name_(path.filename().string())
    , path_(path)
    , options_(options)
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

    if (!player_->load(path_, options_)) {
        Logger::error("VideoFileSource: Failed to load '{}'", path_.string());
        return false;
    }

    const VideoInfo& info = player_->getVideoInfo();
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

    // Takes the frames due on the player's clock (deltaTime is not used for timing)
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

std::string VideoFileSource::getLoopModeNotice() const {
    return player_ ? player_->getLoopModeNotice() : std::string();
}

void VideoFileSource::setTimelineLocked(bool locked, const PlaybackClock& master) {
    if (player_) player_->setTimelineLocked(locked, master);
}

bool VideoFileSource::isTimelineLocked() const {
    return player_ && player_->isTimelineLocked();
}

void VideoFileSource::followTimeline(const PlaybackClock& master) {
    if (player_) player_->followTimeline(master);
}

const PlaybackClock& VideoFileSource::getClock() const {
    return player_->getClock();
}

PlaybackTimePoint VideoFileSource::getClockNow() const {
    return player_->now();
}

std::string VideoFileSource::getErrorMessage() const {
    return player_ ? player_->getErrorMessage() : std::string();
}

bool VideoFileSource::isHardwareDecoding() const {
    return player_ && player_->isHardwareDecoding();
}

std::string VideoFileSource::getDecodeNote() const {
    return player_ ? player_->getDecodeNote() : std::string();
}

const VideoInfo& VideoFileSource::getVideoInfo() const {
    static VideoInfo empty;
    if (player_ && player_->isLoaded()) {
        return player_->getVideoInfo();
    }
    return empty;
}

void VideoFileSource::convertVideoFrame(const VideoFrame& videoFrame) {
    frame_.timestamp = static_cast<uint64_t>(videoFrame.pts * 1000000.0);  // Convert to microseconds
    frame_.frameNumber = static_cast<uint64_t>(videoFrame.frameNumber);

    if (videoFrame.yuv.isValid()) {
        // Share the decoder's planes: the frame moves by handle, not by copy (ADR-055).
        if (!frame_.pixels.empty()) std::vector<uint8_t>().swap(frame_.pixels);
        frame_.yuv = videoFrame.yuv;
        frame_.nextYuv = player_->peekNextPicture();
        frame_.width = videoFrame.width;
        frame_.height = videoFrame.height;
        frame_.channels = 4;  // uploaded into an RGBA texture
        return;
    }
    frame_.yuv = YuvPicture{};
    frame_.nextYuv = YuvPicture{};

    // RGB24 (formats the GPU conversion does not handle): copy into the frame buffer
    if (frame_.width != videoFrame.width || frame_.height != videoFrame.height ||
        frame_.channels != 3 || frame_.pixels.empty()) {
        frame_.allocate(videoFrame.width, videoFrame.height, 3);
    }

    // Copy pixel data (both are RGB24)
    size_t dataSize = frame_.getDataSize();
    if (videoFrame.data.size() >= dataSize) {
        std::memcpy(frame_.pixels.data(), videoFrame.data.data(), dataSize);
    }
}

} // namespace quadmapper
