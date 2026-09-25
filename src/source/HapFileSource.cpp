#include "HapFileSource.hpp"
#include "hap/HapFrameParser.hpp"
#include "video/DecodeBackoff.hpp"
#include "video/FrameSelection.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#ifdef QUADMAPPER_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace quadmapper {

namespace {

// Map the parser's HAP texture format to the GPU-compressed staging format.
// Alpha_RGTC1 is the deferred HapQ-Alpha second plane (v1: ignored).
GpuCompressedFormat toCompressedFormat(hap::HapTextureFormat fmt) {
    switch (fmt) {
        case hap::HapTextureFormat::RGB_DXT1:   return GpuCompressedFormat::BC1_RGB;
        case hap::HapTextureFormat::RGBA_DXT5:  return GpuCompressedFormat::BC3_RGBA;
        case hap::HapTextureFormat::YCoCg_DXT5: return GpuCompressedFormat::BC3_YCoCg;
        case hap::HapTextureFormat::RGBA_BPTC:  return GpuCompressedFormat::BC7_RGBA;
        default:                                return GpuCompressedFormat::None;
    }
}

} // namespace

/**
 * @brief FFmpeg demux + HAP parse behind the pimpl (ADR-011, ADR-044).
 *
 * The HAP stream is DEMUXED only — never sent to the FFmpeg HAP decoder. Each
 * demuxed packet's bytes ARE one HAP frame, parsed straight to BCn via
 * hap::parseHapFrame on a background thread and staged as a compressed FrameData
 * for the main thread to upload (no GL here — rendering rule).
 */
class HapFileSource::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    bool open(const std::filesystem::path& path) {
        close();

        std::string pathStr = path.string();
        if (avformat_open_input(&formatCtx_, pathStr.c_str(), nullptr, nullptr) < 0) {
            Logger::error("HapFileSource: avformat_open_input failed for '{}'", pathStr);
            return false;
        }

        if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
            Logger::error("HapFileSource: avformat_find_stream_info failed for '{}'", pathStr);
            close();
            return false;
        }

        // Find the HAP video stream (do NOT open a decoder).
        videoStreamIndex_ = -1;
        for (unsigned int i = 0; i < formatCtx_->nb_streams; ++i) {
            AVCodecParameters* cp = formatCtx_->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_HAP) {
                videoStreamIndex_ = static_cast<int>(i);
                break;
            }
        }

        if (videoStreamIndex_ < 0) {
            Logger::error("HapFileSource: no HAP video stream in '{}'", pathStr);
            close();
            return false;
        }

        AVStream* stream = formatCtx_->streams[videoStreamIndex_];
        width_ = stream->codecpar->width;
        height_ = stream->codecpar->height;
        timeBase_ = av_q2d(stream->time_base);
        // Frame times are relative to the stream start, so pts 0 is the first frame.
        startPts_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

        if (formatCtx_->duration != AV_NOPTS_VALUE) {
            duration_ = static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
        } else {
            duration_ = 0.0;
        }

        if (width_ <= 0 || height_ <= 0) {
            Logger::error("HapFileSource: invalid dimensions {}x{} in '{}'", width_, height_, pathStr);
            close();
            return false;
        }

        isOpen_ = true;
        position_ = 0.0;
        atEnd_ = false;
        refreshPending_ = true;
        // Demux from the start (bounded by the staging queue) so the first frame is
        // on screen even before play.
        startThread();
        Logger::info("HapFileSource: demuxing HAP '{}' ({}x{}, {}s)",
                     pathStr, width_, height_, duration_);
        return true;
    }

    void close() {
        stopThread();
        if (formatCtx_) {
            avformat_close_input(&formatCtx_);
            formatCtx_ = nullptr;
        }
        clearQueue();
        videoStreamIndex_ = -1;
        width_ = 0;
        height_ = 0;
        timeBase_ = 0.0;
        startPts_ = 0;
        duration_ = 0.0;
        position_ = 0.0;
        isOpen_ = false;
        atEnd_ = false;
        refreshPending_ = false;
    }

    bool isOpen() const { return isOpen_; }

    std::string lastError() const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return lastError_;
    }
    int width() const { return width_; }
    int height() const { return height_; }
    double duration() const { return duration_; }
    double position() const { return position_; }

    // --- Playback control (clock lives here; main thread drives update) ---

    void play() {
        if (!isOpen_) return;
        if (state_ == PlaybackState::FINISHED) {
            seek(0.0);
        }
        state_ = PlaybackState::PLAYING;
        startThread();
    }

    void pause() {
        if (state_ == PlaybackState::PLAYING) {
            state_ = PlaybackState::PAUSED;
        }
    }

    void stop() {
        state_ = PlaybackState::STOPPED;
        seek(0.0);
    }

    void togglePlayPause() {
        if (state_ == PlaybackState::PLAYING) pause(); else play();
    }

    PlaybackState state() const { return state_; }

    void setSpeed(float speed) { speed_ = std::clamp(speed, 0.1f, 4.0f); }
    float speed() const { return speed_; }

    // Ping-pong needs reverse playback, which forward demuxing cannot do; it was only
    // ever a forward loop, so files that saved it keep looping (QM-088).
    void setLoopMode(LoopMode mode) { loopMode_ = mode == LoopMode::PING_PONG ? LoopMode::LOOP : mode; }
    LoopMode loopMode() const { return loopMode_; }

    void seek(double timeSeconds) {
        if (!isOpen_) return;
        timeSeconds = std::clamp(timeSeconds, 0.0, duration_ > 0.0 ? duration_ : timeSeconds);

        stopThread();

        if (timeBase_ > 0.0) {
            int64_t ts = static_cast<int64_t>(timeSeconds / timeBase_) + startPts_;
            if (av_seek_frame(formatCtx_, videoStreamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
                Logger::warning("HapFileSource: seek to {}s failed", timeSeconds);
            }
        }

        clearQueue();
        position_ = timeSeconds;
        atEnd_ = false;
        refreshPending_ = true;
        if (state_ == PlaybackState::FINISHED) {
            state_ = PlaybackState::PAUSED;
        }
        startThread();
    }

    // Advance the playback clock (while playing) and take the staged frames that are
    // due; the newest due one is returned in out. Same rule as VideoPlayer::update
    // (FrameSelection.hpp): never ahead of the clock, late frames dropped.
    bool update(double deltaTime, FrameData& out) {
        if (!isOpen_) {
            return false;
        }

        const bool playing = state_ == PlaybackState::PLAYING;
        if (playing) {
            position_ += deltaTime * static_cast<double>(speed_);
            if (duration_ > 0.0 && position_ > duration_) {
                position_ = duration_;
            }
        }
        // At the clip's end take whatever is left, so it drains and loops.
        const bool clockAtEnd = playing && duration_ > 0.0 && position_ >= duration_;
        const double target = clockAtEnd ? std::numeric_limits<double>::infinity() : position_;

        size_t taken = 0;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            taken = popDueFrames(
                target, !refreshPending_,
                [this](double& pts) {
                    if (frameQueue_.empty()) return false;
                    pts = frameQueue_.front().pts;
                    return true;
                },
                [this, &out]() {
                    out = std::move(frameQueue_.front().data);
                    frameQueue_.pop();
                });
        }
        if (taken > 0) {
            refreshPending_ = false;
            queueCond_.notify_one();
        }

        // End-of-stream handling once the demux thread is drained. Check emptiness
        // under the lock but call handleEndOfVideo() WITHOUT it — that path joins the
        // demux thread (via seek), which itself blocks on queueMutex_ (deadlock if held).
        if (playing && atEnd_ && taken == 0) {
            bool drained;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                drained = frameQueue_.empty();
            }
            if (drained) {
                handleEndOfVideo();
            }
        }

        return taken > 0;
    }

private:
    struct StagedFrame {
        double pts = 0.0;
        FrameData data;
    };

    void handleEndOfVideo() {
        if (loopMode_ == LoopMode::ONCE) {
            state_ = PlaybackState::FINISHED;
            return;
        }
        // Re-arm from the start. seek() restarts the demux thread.
        seek(0.0);
        play();
    }

    void startThread() {
        if (thread_.joinable()) return;
        stopFlag_ = false;
        thread_ = std::thread(&Impl::demuxLoop, this);
    }

    void stopThread() {
        if (!thread_.joinable()) return;
        // Set the flag under the mutex the demux thread waits on: set outside it,
        // the notify can land between the thread's predicate check and its wait,
        // and join() then hangs on a thread that never wakes.
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopFlag_ = true;
            lastError_.clear();
        }
        queueCond_.notify_all();
        thread_.join();
    }

    // One failed read or parse on the demux thread; waits out the backoff delay
    // (interruptible by stop) and surfaces the error once past the threshold.
    void onDemuxFailure(DecodeBackoff& backoff, const std::string& reason) {
        const auto delay = backoff.onFailure();
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (backoff.failing()) {
            if (lastError_.empty()) {
                Logger::error("HapFileSource: {} (retrying with backoff)", reason);
            }
            lastError_ = reason;
        }
        if (delay.count() > 0) {
            queueCond_.wait_for(lock, delay, [this]() { return stopFlag_.load(); });
        }
    }

    void clearQueue() {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::queue<StagedFrame> empty;
        std::swap(frameQueue_, empty);
    }

    // Background thread: demux packets, parse HAP -> BCn, stage compressed frames.
    void demuxLoop() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            Logger::error("HapFileSource: av_packet_alloc failed on demux thread");
            return;
        }

        DecodeBackoff backoff;
        while (!stopFlag_) {
            // Backpressure: wait until there's room in the staging queue.
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCond_.wait(lock, [this]() {
                    return stopFlag_ || frameQueue_.size() < kMaxQueued;
                });
            }
            if (stopFlag_) break;

            int rc = av_read_frame(formatCtx_, packet);
            if (rc < 0) {
                if (rc == AVERROR_EOF) {
                    // Idle until a seek or stop restarts the thread.
                    atEnd_ = true;
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    queueCond_.wait_for(lock, std::chrono::milliseconds(10),
                                        [this]() { return stopFlag_.load(); });
                } else {
                    char err[AV_ERROR_MAX_STRING_SIZE] = {0};
                    av_strerror(rc, err, sizeof(err));
                    onDemuxFailure(backoff, std::string("HAP read failed: ") + err);
                }
                continue;
            }

            if (packet->stream_index != videoStreamIndex_) {
                av_packet_unref(packet);
                continue;
            }

            // The packet bytes ARE the HAP frame — parse straight to BCn.
            hap::HapDecodedFrame decoded =
                hap::parseHapFrame(packet->data, static_cast<size_t>(packet->size));

            double pts = 0.0;
            if (packet->pts != AV_NOPTS_VALUE && timeBase_ > 0.0) {
                pts = static_cast<double>(packet->pts - startPts_) * timeBase_;
            } else if (packet->dts != AV_NOPTS_VALUE && timeBase_ > 0.0) {
                pts = static_cast<double>(packet->dts - startPts_) * timeBase_;
            }

            av_packet_unref(packet);

            if (!decoded.valid()) {
                Logger::warning("HapFileSource: parseHapFrame produced an invalid frame; skipping");
                onDemuxFailure(backoff, "HAP frames could not be decoded");
                continue;
            }

            GpuCompressedFormat cf = toCompressedFormat(decoded.format);
            if (cf == GpuCompressedFormat::None) {
                Logger::warning("HapFileSource: unsupported HAP texture format; skipping");
                continue;
            }

            if (!decoded.alphaPlane.empty() && !loggedAlphaPlane_) {
                Logger::warning("HapFileSource: HapQ-Alpha second plane present but deferred (v1) — primary plane only");
                loggedAlphaPlane_ = true;
            }

            StagedFrame staged;
            staged.pts = pts;
            staged.data.width = width_;
            staged.data.height = height_;
            staged.data.compressedFormat = cf;
            staged.data.pixels = std::move(decoded.textureData);
            staged.data.timestamp = static_cast<uint64_t>(pts * 1000000.0);
            staged.data.frameNumber = frameCounter_++;

            backoff.onSuccess();
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                lastError_.clear();
                frameQueue_.push(std::move(staged));
            }
        }

        av_packet_free(&packet);
    }

    // FFmpeg
    AVFormatContext* formatCtx_ = nullptr;
    int videoStreamIndex_ = -1;

    // Stream info
    int width_ = 0;
    int height_ = 0;
    double timeBase_ = 0.0;
    int64_t startPts_ = 0;  // stream start_time, in time_base units
    double duration_ = 0.0;
    bool isOpen_ = false;

    // Playback clock (main thread)
    double position_ = 0.0;
    bool refreshPending_ = false;  // an open/seek made the shown frame stale
    float speed_ = 1.0f;
    PlaybackState state_ = PlaybackState::STOPPED;
    LoopMode loopMode_ = LoopMode::LOOP;
    uint64_t frameCounter_ = 0;
    bool loggedAlphaPlane_ = false;

    // Staging queue (background -> main)
    static constexpr size_t kMaxQueued = 8;
    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> atEnd_{false};
    mutable std::mutex queueMutex_;
    std::condition_variable queueCond_;
    std::queue<StagedFrame> frameQueue_;
    std::string lastError_;  // guarded by queueMutex_
};

#else // !QUADMAPPER_HAS_FFMPEG

namespace quadmapper {

// Stub when FFmpeg is unavailable — HAP playback simply never initializes.
class HapFileSource::Impl {
public:
    bool open(const std::filesystem::path&) { return false; }
    void close() {}
    bool isOpen() const { return false; }
    int width() const { return 0; }
    int height() const { return 0; }
    double duration() const { return 0.0; }
    double position() const { return 0.0; }
    void play() {}
    void pause() {}
    void stop() {}
    void togglePlayPause() {}
    PlaybackState state() const { return PlaybackState::STOPPED; }
    void setSpeed(float) {}
    float speed() const { return 1.0f; }
    void setLoopMode(LoopMode) {}
    LoopMode loopMode() const { return LoopMode::ONCE; }
    void seek(double) {}
    bool update(double, FrameData&) { return false; }
    std::string lastError() const { return {}; }
};

#endif // QUADMAPPER_HAS_FFMPEG

// --- Public interface ---

HapFileSource::HapFileSource(const std::filesystem::path& path)
    : name_(path.filename().string())
    , path_(path)
    , impl_(std::make_unique<Impl>())
{
}

HapFileSource::~HapFileSource() {
    shutdown();
}

bool HapFileSource::initialize() {
    if (ready_) {
        return true;
    }
    if (!impl_->open(path_)) {
        Logger::error("HapFileSource: Failed to load '{}'", path_.string());
        return false;
    }
    ready_ = true;
    return true;
}

void HapFileSource::shutdown() {
    if (impl_) {
        impl_->close();
    }
    frame_.clear();
    ready_ = false;
    hasNewFrame_ = false;
}

bool HapFileSource::update(double deltaTime) {
    if (!ready_) {
        return false;
    }
    FrameData staged;
    if (impl_->update(deltaTime, staged) && staged.isValid()) {
        frame_ = std::move(staged);
        hasNewFrame_ = true;
        return true;
    }
    return false;
}

const FrameData* HapFileSource::getCurrentFrame() const {
    if (!ready_ || !frame_.isValid()) {
        return nullptr;
    }
    return &frame_;
}

void HapFileSource::markFrameConsumed() {
    hasNewFrame_ = false;
}

SourceCapabilities HapFileSource::getCapabilities() const {
    SourceCapabilities caps;
    caps.isStatic = false;
    caps.hasPlayback = true;
    caps.hasParameters = false;
    caps.supportsAsync = true;
    caps.isGPUNative = false;   // CPU produces BCn bytes; main thread uploads
    return caps;
}

int HapFileSource::getWidth() const { return impl_ ? impl_->width() : 0; }
int HapFileSource::getHeight() const { return impl_ ? impl_->height() : 0; }

void HapFileSource::play() { if (impl_) impl_->play(); }
void HapFileSource::pause() { if (impl_) impl_->pause(); }
void HapFileSource::stop() { if (impl_) impl_->stop(); }
void HapFileSource::togglePlayPause() { if (impl_) impl_->togglePlayPause(); }

void HapFileSource::seek(double timeSeconds) { if (impl_) impl_->seek(timeSeconds); }

void HapFileSource::seekProgress(float progress) {
    if (impl_) {
        impl_->seek(static_cast<double>(progress) * impl_->duration());
    }
}

PlaybackState HapFileSource::getPlaybackState() const {
    return impl_ ? impl_->state() : PlaybackState::STOPPED;
}

double HapFileSource::getCurrentTime() const { return impl_ ? impl_->position() : 0.0; }
double HapFileSource::getDuration() const { return impl_ ? impl_->duration() : 0.0; }

float HapFileSource::getProgress() const {
    if (!impl_ || impl_->duration() <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(impl_->position() / impl_->duration());
}

void HapFileSource::setSpeed(float speed) { if (impl_) impl_->setSpeed(speed); }
float HapFileSource::getSpeed() const { return impl_ ? impl_->speed() : 1.0f; }

void HapFileSource::setLoopMode(LoopMode mode) { if (impl_) impl_->setLoopMode(mode); }
LoopMode HapFileSource::getLoopMode() const { return impl_ ? impl_->loopMode() : LoopMode::ONCE; }

std::string HapFileSource::getErrorMessage() const { return impl_ ? impl_->lastError() : std::string(); }

} // namespace quadmapper
