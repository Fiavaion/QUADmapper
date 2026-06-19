#include "HapFileSource.hpp"
#include "hap/HapFrameParser.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
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
        duration_ = 0.0;
        position_ = 0.0;
        isOpen_ = false;
        atEnd_ = false;
    }

    bool isOpen() const { return isOpen_; }
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

    void setLoopMode(LoopMode mode) { loopMode_ = mode; }
    LoopMode loopMode() const { return loopMode_; }

    void seek(double timeSeconds) {
        if (!isOpen_) return;
        timeSeconds = std::clamp(timeSeconds, 0.0, duration_ > 0.0 ? duration_ : timeSeconds);

        bool wasRunning = threadRunning();
        stopThread();

        if (timeBase_ > 0.0) {
            int64_t ts = static_cast<int64_t>(timeSeconds / timeBase_);
            if (av_seek_frame(formatCtx_, videoStreamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
                Logger::warning("HapFileSource: seek to {}s failed", timeSeconds);
            }
        }

        clearQueue();
        position_ = timeSeconds;
        atEnd_ = false;
        if (state_ == PlaybackState::FINISHED) {
            state_ = PlaybackState::PAUSED;
        }
        if (wasRunning && state_ == PlaybackState::PLAYING) {
            startThread();
        }
    }

    // Advance the playback clock and pull the freshest staged frame at or before
    // the target time. Returns the frame to display (or nullptr if none new).
    // Mirrors VideoPlayer::update.
    bool update(double deltaTime, FrameData& out) {
        if (!isOpen_ || state_ != PlaybackState::PLAYING) {
            return false;
        }

        position_ += deltaTime * static_cast<double>(speed_);
        double target = position_;

        bool got = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            while (!frameQueue_.empty()) {
                StagedFrame& front = frameQueue_.front();
                // Take the frame; if it is still ahead of target, leave it for next time.
                if (front.pts > target && got) {
                    break;
                }
                out = std::move(front.data);
                double pts = front.pts;
                frameQueue_.pop();
                got = true;
                // Stop once we've reached a frame at/after the target PTS.
                if (pts >= target) {
                    break;
                }
            }
        }
        queueCond_.notify_one();

        // End-of-stream handling once the demux thread is drained. Check emptiness
        // under the lock but call handleEndOfVideo() WITHOUT it — that path joins the
        // demux thread (via seek), which itself blocks on queueMutex_ (deadlock if held).
        if (atEnd_ && !got) {
            bool drained;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                drained = frameQueue_.empty();
            }
            if (drained) {
                handleEndOfVideo();
            }
        }

        return got;
    }

private:
    struct StagedFrame {
        double pts = 0.0;
        FrameData data;
    };

    void handleEndOfVideo() {
        switch (loopMode_) {
            case LoopMode::ONCE:
                state_ = PlaybackState::FINISHED;
                break;
            case LoopMode::LOOP:
            case LoopMode::PING_PONG:  // backward playback not yet supported; loop forward
                // Re-arm from the start. seek() restarts the demux thread.
                position_ = 0.0;
                seek(0.0);
                play();
                break;
        }
    }

    bool threadRunning() const { return thread_.joinable(); }

    void startThread() {
        if (thread_.joinable()) return;
        stopFlag_ = false;
        thread_ = std::thread(&Impl::demuxLoop, this);
    }

    void stopThread() {
        if (!thread_.joinable()) return;
        stopFlag_ = true;
        queueCond_.notify_all();
        thread_.join();
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
                    atEnd_ = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
                pts = static_cast<double>(packet->pts) * timeBase_;
            } else if (packet->dts != AV_NOPTS_VALUE && timeBase_ > 0.0) {
                pts = static_cast<double>(packet->dts) * timeBase_;
            }

            av_packet_unref(packet);

            if (!decoded.valid()) {
                Logger::warning("HapFileSource: parseHapFrame produced an invalid frame; skipping");
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

            {
                std::lock_guard<std::mutex> lock(queueMutex_);
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
    double duration_ = 0.0;
    bool isOpen_ = false;

    // Playback clock (main thread)
    double position_ = 0.0;
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

} // namespace quadmapper
