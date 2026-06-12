#include "VideoPlayer.hpp"
#include "core/Logger.hpp"
#include <algorithm>

namespace quadmapper {

VideoPlayer::VideoPlayer()
    : decoder_(std::make_unique<VideoDecoder>())
{
}

VideoPlayer::~VideoPlayer() {
    unload();
}

VideoPlayer::VideoPlayer(VideoPlayer&&) noexcept = default;
VideoPlayer& VideoPlayer::operator=(VideoPlayer&&) noexcept = default;

bool VideoPlayer::load(const std::filesystem::path& path) {
    unload();

    if (!decoder_->open(path)) {
        return false;
    }

    path_ = path;
    const auto& info = decoder_->getInfo();
    state_.duration = info.duration;
    state_.totalFrames = info.totalFrames;
    state_.state = PlaybackState::STOPPED;
    state_.position = 0.0;
    state_.currentFrame = 0;

    // Start background decoding
    decoder_->startBackgroundDecoding(8);

    LOG_INFO("Loaded video: {}", path.string());
    return true;
}

void VideoPlayer::unload() {
    if (decoder_) {
        decoder_->stopBackgroundDecoding();
        decoder_->close();
    }

    state_.reset();
    currentFrame_ = VideoFrame{};
    path_.clear();
    hasNewFrame_ = false;
    needsSeek_ = false;
}

bool VideoPlayer::isLoaded() const {
    return decoder_ && decoder_->isOpen();
}

void VideoPlayer::play() {
    if (!isLoaded()) return;

    if (state_.state == PlaybackState::FINISHED) {
        // Restart from beginning
        seek(0.0);
    }

    state_.state = PlaybackState::PLAYING;
    state_.lastUpdateTime = std::chrono::steady_clock::now();
}

void VideoPlayer::pause() {
    if (!isLoaded()) return;

    if (state_.state == PlaybackState::PLAYING) {
        state_.state = PlaybackState::PAUSED;
    }
}

void VideoPlayer::stop() {
    if (!isLoaded()) return;

    state_.state = PlaybackState::STOPPED;
    seek(0.0);
}

void VideoPlayer::togglePlayPause() {
    if (state_.isPlaying()) {
        pause();
    } else {
        play();
    }
}

void VideoPlayer::seek(double timeSeconds) {
    if (!isLoaded()) return;

    // Clamp to valid range
    timeSeconds = std::clamp(timeSeconds, 0.0, state_.duration);

    needsSeek_ = true;
    seekTarget_ = timeSeconds;
    state_.position = timeSeconds;
    state_.currentFrame = static_cast<int64_t>(timeSeconds * decoder_->getInfo().frameRate);

    // Clear any buffered frames
    decoder_->stopBackgroundDecoding();

    if (decoder_->seek(timeSeconds)) {
        // Restart background decoding after seek
        decoder_->startBackgroundDecoding(8);
        hasNewFrame_ = false;
    }

    needsSeek_ = false;

    // If we were finished, go back to paused
    if (state_.state == PlaybackState::FINISHED) {
        state_.state = PlaybackState::PAUSED;
    }
}

void VideoPlayer::seekProgress(float progress) {
    seek(static_cast<double>(progress) * state_.duration);
}

void VideoPlayer::skip(double seconds) {
    seek(state_.position + seconds);
}

void VideoPlayer::setSpeed(float speed) {
    state_.speed = std::clamp(speed, 0.1f, 4.0f);
}

float VideoPlayer::getSpeed() const {
    return state_.speed;
}

void VideoPlayer::setLoopMode(LoopMode mode) {
    state_.loopMode = mode;
}

LoopMode VideoPlayer::getLoopMode() const {
    return state_.loopMode;
}

const VideoPlaybackState& VideoPlayer::getState() const {
    return state_;
}

const VideoInfo& VideoPlayer::getVideoInfo() const {
    static VideoInfo empty;
    return decoder_ ? decoder_->getInfo() : empty;
}

const std::filesystem::path& VideoPlayer::getPath() const {
    return path_;
}

bool VideoPlayer::update(double deltaTime) {
    if (!isLoaded()) return false;

    // Only advance time when playing
    if (!state_.isPlaying()) {
        return false;
    }

    // Calculate target PTS based on playback position
    updatePlaybackPosition(deltaTime);

    // Check if we need a new frame
    double targetPts = state_.position;

    // Get frames from decoder until we have one at or past target PTS
    while (decoder_->hasBufferedFrames()) {
        VideoFrame frame = decoder_->getBufferedFrame();
        if (!frame.isValid()) {
            break;
        }

        // Store this frame
        currentFrame_ = std::move(frame);
        hasNewFrame_ = true;

        // If frame PTS is at or past target, we're done
        if (currentFrame_.pts >= targetPts) {
            break;
        }

        // For fast playback, skip ahead to catch up
        if (state_.speed > 1.5f) {
            // Keep skipping frames
            continue;
        }
    }

    // Check for end of video
    if (decoder_->isAtEnd() && !decoder_->hasBufferedFrames()) {
        handleEndOfVideo();
    }

    return hasNewFrame_;
}

void VideoPlayer::updatePlaybackPosition(double deltaTime) {
    // Advance playback position based on speed
    double advance = deltaTime * static_cast<double>(state_.speed);
    state_.position += advance;

    // Update frame number
    const auto& info = decoder_->getInfo();
    if (info.frameRate > 0) {
        state_.currentFrame = static_cast<int64_t>(state_.position * info.frameRate);
    }

    // Clamp to duration
    if (state_.position >= state_.duration) {
        state_.position = state_.duration;
    }
}

void VideoPlayer::handleEndOfVideo() {
    switch (state_.loopMode) {
        case LoopMode::ONCE:
            state_.state = PlaybackState::FINISHED;
            if (endCallback_) {
                endCallback_();
            }
            break;

        case LoopMode::LOOP:
            seek(0.0);
            play();
            state_.isLooping = true;
            break;

        case LoopMode::PING_PONG:
            // For ping-pong, we'd need to play backward
            // For now, just loop normally
            seek(0.0);
            play();
            state_.isLooping = true;
            break;
    }
}

const VideoFrame& VideoPlayer::getCurrentFrame() const {
    return currentFrame_;
}

bool VideoPlayer::hasNewFrame() const {
    return hasNewFrame_;
}

void VideoPlayer::consumeFrame() {
    hasNewFrame_ = false;
}

void VideoPlayer::setEndCallback(EndCallback callback) {
    endCallback_ = std::move(callback);
}

} // namespace quadmapper
