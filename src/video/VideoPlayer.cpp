#include "VideoPlayer.hpp"
#include "FrameSelection.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/MainThreadTimings.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace quadmapper {

namespace {
constexpr size_t FRAME_BUFFER_SIZE = 8;
}

VideoPlayer::VideoPlayer()
    : decoder_(std::make_unique<VideoDecoder>())
{
}

VideoPlayer::~VideoPlayer() {
    unload();
}

VideoPlayer::VideoPlayer(VideoPlayer&&) noexcept = default;
VideoPlayer& VideoPlayer::operator=(VideoPlayer&&) noexcept = default;

bool VideoPlayer::load(const std::filesystem::path& path, const DecodeOptions& options) {
    unload();

    if (!decoder_->open(path, options)) {
        return false;
    }

    path_ = path;
    transport_.load(decoder_->getInfo().duration);
    start();
    LOG_INFO("Loaded video: {}", utf8FromPath(path));
    return true;
}

void VideoPlayer::beginOpen(const std::filesystem::path& path) {
    unload();
    path_ = path;
    opening_ = true;
}

bool VideoPlayer::finishOpen(std::unique_ptr<VideoDecoder> decoder) {
    if (!opening_ || !decoder || !decoder->isOpen()) return false;
    opening_ = false;
    decoder_ = std::move(decoder);  // the unopened one it replaces holds no thread or file
    // Play, pause, seek, speed, loop and lock given while the file opened already moved
    // the clock: keep its position and state, now with the clip's duration.
    transport_.setDuration(decoder_->getInfo().duration);
    start();
    LOG_INFO("Loaded video: {}", utf8FromPath(path_));
    return true;
}

// The decoder just opened: playback state from its info, decoding at the clock's position.
void VideoPlayer::start() {
    const auto& info = decoder_->getInfo();
    state_.duration = info.duration;
    state_.totalFrames = info.totalFrames;
    if (transport_.loopMode() == LoopMode::PING_PONG) {
        transport_.setLoopMode(LoopMode::PING_PONG, decoder_->checkReverse(reverseCacheBytes_));
    }
    applyTransport();  // starts background decoding at the clock's position
}

void VideoPlayer::unload() {
    if (decoder_) {
        decoder_->stopBackgroundDecoding();
        decoder_->close();
    }

    state_.reset();
    currentFrame_ = VideoFrame{};
    path_.clear();
    opening_ = false;
    hasNewFrame_ = false;
    refreshPending_ = false;
    durationChecked_ = false;
}

bool VideoPlayer::isLoaded() const {
    return decoder_ && decoder_->isOpen();
}

bool VideoPlayer::takesTransport() const {
    return opening_ || isLoaded();
}

void VideoPlayer::play() {
    if (!takesTransport()) return;
    transport_.play();
    applyTransport();
}

void VideoPlayer::pause() {
    if (!takesTransport()) return;
    transport_.pause();
    applyTransport();
}

void VideoPlayer::stop() {
    if (!takesTransport()) return;
    transport_.stop();
    applyTransport();
}

void VideoPlayer::togglePlayPause() {
    if (state_.isPlaying()) {
        pause();
    } else {
        play();
    }
}

void VideoPlayer::seek(double timeSeconds) {
    if (!takesTransport()) return;
    transport_.seek(timeSeconds);
    applyTransport();
}

void VideoPlayer::seekProgress(float progress) {
    seek(static_cast<double>(progress) * state_.duration);
}

void VideoPlayer::skip(double seconds) {
    seek(state_.position + seconds);
}

void VideoPlayer::setSpeed(float speed) {
    transport_.setSpeed(speed);
    refreshState();
}

float VideoPlayer::getSpeed() const {
    return transport_.speed();
}

void VideoPlayer::setLoopMode(LoopMode mode) {
    const std::string reverseProblem = mode == LoopMode::PING_PONG && isLoaded()
        ? decoder_->checkReverse(reverseCacheBytes_) : std::string();
    transport_.setLoopMode(mode, reverseProblem);
    if (takesTransport()) applyTransport();
}

LoopMode VideoPlayer::getLoopMode() const {
    return transport_.loopMode();
}

const std::string& VideoPlayer::getLoopModeNotice() const {
    return transport_.loopModeNotice();
}

void VideoPlayer::setReverseCacheBytes(size_t bytes) {
    reverseCacheBytes_ = bytes;
}

void VideoPlayer::setTimelineLocked(bool locked, const PlaybackClock& master) {
    transport_.setTimelineLocked(locked, master);
    if (takesTransport()) applyTransport();
}

bool VideoPlayer::isTimelineLocked() const {
    return transport_.timelineLocked();
}

void VideoPlayer::followTimeline(const PlaybackClock& master) {
    transport_.followTimeline(master);
}

const VideoPlaybackState& VideoPlayer::getState() const {
    return state_;
}

const PlaybackClock& VideoPlayer::getClock() const {
    return transport_.clock();
}

PlaybackTimePoint VideoPlayer::now() const {
    return transport_.now();
}

void VideoPlayer::setTimeSource(PlaybackTransport::TimeSource source) {
    transport_.setTimeSource(std::move(source));
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

size_t VideoPlayer::getReverseCachePeakBytes() const {
    return decoder_ ? decoder_->getReverseCachePeakBytes() : 0;
}

bool VideoPlayer::isHardwareDecoding() const {
    return decoder_ && decoder_->isHardwareDecoding();
}

std::string VideoPlayer::getDecodeNote() const {
    return decoder_ ? decoder_->getDecodeNote() : std::string();
}

bool VideoPlayer::isZeroCopy() const {
    return decoder_ && decoder_->isZeroCopy();
}

void VideoPlayer::stopZeroCopy(const std::string& reason) {
    if (decoder_) decoder_->stopZeroCopy(reason);
}

std::string VideoPlayer::getZeroCopyNote() const {
    return decoder_ ? decoder_->getZeroCopyNote() : std::string();
}

void VideoPlayer::applyTransport() {
    // While the file opens the request waits: finishOpen() starts decoding wherever the
    // clock is then.
    if (isLoaded() && transport_.takeReposition()) reposition();
    refreshState();
}

void VideoPlayer::reposition() {
    ScopedMainThreadTiming timing("reposition");
    const PlaybackClock& clock = transport_.clock();
    DecodePlan plan;
    plan.timeline = clock.timeline(transport_.now());
    plan.duration = clock.duration();
    plan.loopMode = clock.loopMode();
    plan.reverseCacheBytes = reverseCacheBytes_;
    decoder_->startPlayback(plan, FRAME_BUFFER_SIZE);
    lastTimeline_ = plan.timeline;
    // The frame on screen stays until the first frame of the new position arrives.
    hasNewFrame_ = false;
    refreshPending_ = true;
}

bool VideoPlayer::update(double /*deltaTime*/) {
    if (!isLoaded()) return false;

    // Ping-Pong met a group of pictures too big for the reverse cache: play as Loop.
    if (transport_.effectiveLoopMode() == LoopMode::PING_PONG) {
        transport_.reverseUnavailable(decoder_->getReverseError());
    }
    checkDuration();
    if (transport_.takeReposition()) reposition();

    const PlaybackTimePoint now = transport_.now();
    const PlaybackClock& clock = transport_.clock();
    const double timeline = clock.timeline(now);
    const ClipPosition clip = clock.sample(now);
    const bool playing = transport_.state() == PlaybackState::PLAYING;

    // A paused source holds its frame. Its own clock is stopped, so frames up to the
    // playhead are still taken (a clip loaded or seeked while paused shows the frame
    // there); a paused locked source's clock keeps the master's pace, so it only takes
    // the frame a reposition asked for.
    if (playing || !clock.running() || refreshPending_) {
        // Once: at the end take whatever is left, so a last frame stamped past the
        // declared duration still shows.
        const double target = playing && clip.ended ? std::numeric_limits<double>::infinity()
                                                    : timeline;
        // After a load or seek the shown frame is stale: take the next one even if early.
        const bool haveFrame = currentFrame_.isValid() && !refreshPending_;
        if (takeDueFrames(target, haveFrame) > 0) {
            refreshPending_ = false;
        }
    }

    // The clock jumped ahead since the last update (a stall) and nothing decoded is
    // near it: resume decoding at the clock instead of decoding everything between.
    const bool clockJumped = timeline - lastTimeline_ > RESYNC_LATENESS;
    lastTimeline_ = timeline;
    double nextDue = 0.0;
    if (playing && clockJumped && !refreshPending_ && currentFrame_.isValid() &&
        currentFrame_.due < timeline - RESYNC_LATENESS && !decoder_->isAtEnd() &&
        (!decoder_->peekBufferedDue(nextDue) || nextDue < timeline - RESYNC_LATENESS)) {
        reposition();
    }

    if (playing && !transport_.timelineLocked()) handleEndOfClip(clip);
    refreshState();
    return hasNewFrame_;
}

// Only Once reaches an end; the decoder follows Loop and Ping-Pong forever.
void VideoPlayer::handleEndOfClip(const ClipPosition& clip) {
    if (!clip.ended || !decoder_->isAtEnd() || decoder_->hasBufferedFrames()) return;
    transport_.finish();
    if (endCallback_) endCallback_();
}

void VideoPlayer::checkDuration() {
    if (durationChecked_) return;
    const double end = decoder_->getObservedEnd();
    if (end <= 0.0) return;
    durationChecked_ = true;
    // The container's duration was missing or wrong: wrap where the video really ends.
    const double fps = decoder_->getInfo().frameRate;
    const double halfFrame = fps > 0.0 ? 0.5 / fps : 0.02;
    if (std::abs(end - transport_.clock().duration()) > halfFrame) {
        LOG_INFO("Video {} ends at {} s, not the declared {} s", utf8FromPath(path_.filename()), end,
                 transport_.clock().duration());
        state_.duration = end;
        transport_.setDuration(end);
    }
}

void VideoPlayer::refreshState() {
    const ClipPosition clip = transport_.clock().sample(transport_.now());
    state_.state = transport_.state();
    state_.position = clip.position;
    state_.direction = clip.direction;
    state_.speed = transport_.speed();
    state_.loopMode = transport_.loopMode();
    const double fps = decoder_ ? decoder_->getInfo().frameRate : 0.0;
    state_.currentFrame = fps > 0.0 ? static_cast<int64_t>(clip.position * fps) : 0;
}

size_t VideoPlayer::takeDueFrames(double target, bool haveFrame) {
    return popDueFrames(
        target, haveFrame,
        [this](double& due) { return decoder_->peekBufferedDue(due); },
        [this]() {
            VideoFrame frame = decoder_->getBufferedFrame();
            if (frame.isValid()) {
                currentFrame_ = std::move(frame);
                hasNewFrame_ = true;
            }
        });
}

const VideoFrame& VideoPlayer::getCurrentFrame() const {
    return currentFrame_;
}

YuvPicture VideoPlayer::peekNextPicture() const {
    return decoder_ ? decoder_->peekBufferedPicture() : YuvPicture{};
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
