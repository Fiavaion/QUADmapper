#include "VideoPlayer.hpp"
#include "FrameSelection.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <limits>

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
    refreshPending_ = true;

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
    refreshPending_ = false;
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
        refreshPending_ = true;
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
    // Ping-pong needs reverse playback, which a forward-only decoder cannot do; it was
    // only ever a forward loop, so files that saved it keep looping (QM-088).
    state_.loopMode = mode == LoopMode::PING_PONG ? LoopMode::LOOP : mode;
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

std::string VideoPlayer::getErrorMessage() const {
    return decoder_ ? decoder_->getLastError() : std::string();
}

bool VideoPlayer::update(double deltaTime) {
    if (!isLoaded()) return false;

    // The clock only runs while playing; paused or stopped it holds, but frames up to
    // the playhead are still taken, so a clip loaded or seeked while paused shows the
    // frame at the playhead instead of black or a stale picture.
    const bool playing = state_.isPlaying();
    if (playing) {
        updatePlaybackPosition(deltaTime);
    }

    // At the clip's end the clock is clamped to the duration; take whatever is left
    // so a last frame stamped past the container duration still drains and loops.
    const bool clockAtEnd = playing && state_.duration > 0.0 && state_.position >= state_.duration;
    const double targetPts = clockAtEnd ? std::numeric_limits<double>::infinity()
                                        : state_.position;
    // After a load or seek the shown frame is stale: take the next one even if early.
    const bool haveFrame = currentFrame_.isValid() && !refreshPending_;
    if (takeDueFrames(targetPts, haveFrame) > 0) {
        refreshPending_ = false;
    }

    // The clip ends once the last frame has had its full display time, so a loop does
    // not cut the final frame short.
    const double fps = decoder_->getInfo().frameRate;
    const double lastFrameEnd = currentFrame_.pts + (fps > 0.0 ? 1.0 / fps : 0.0);
    const bool lastFrameDone = clockAtEnd || state_.position + FRAME_DUE_EPSILON >= lastFrameEnd;
    if (playing && decoder_->isAtEnd() && !decoder_->hasBufferedFrames() && lastFrameDone) {
        handleEndOfVideo();
    }

    return hasNewFrame_;
}

size_t VideoPlayer::takeDueFrames(double targetPts, bool haveFrame) {
    return popDueFrames(
        targetPts, haveFrame,
        [this](double& pts) { return decoder_->peekBufferedPts(pts); },
        [this]() {
            VideoFrame frame = decoder_->getBufferedFrame();
            if (frame.isValid()) {
                currentFrame_ = std::move(frame);
                hasNewFrame_ = true;
            }
        });
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

    // Clamp to duration (unknown duration = 0: the decoder's end-of-file ends the clip)
    if (state_.duration > 0.0 && state_.position >= state_.duration) {
        state_.position = state_.duration;
    }
}

void VideoPlayer::handleEndOfVideo() {
    if (state_.loopMode == LoopMode::ONCE) {
        state_.state = PlaybackState::FINISHED;
        if (endCallback_) {
            endCallback_();
        }
        return;
    }
    seek(0.0);
    play();
    state_.isLooping = true;
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
