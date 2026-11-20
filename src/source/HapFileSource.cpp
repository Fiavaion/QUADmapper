#include "HapFileSource.hpp"
#include "hap/HapFrameParser.hpp"
#include "video/DecodeBackoff.hpp"
#include "video/FrameSelection.hpp"
#include "video/PlaybackTransport.hpp"
#include "video/VideoDecoder.hpp"  // DecodePlan
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/MediaLimits.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <exception>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#ifdef QUADMAPPER_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}
#include "video/LocalMediaInput.hpp"

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

// Seeks further back tried when a reverse step lands on or after its bound.
constexpr int MAX_REVERSE_RETRIES = 20;

} // namespace

// A HAP file's container, opened and read up to its stream facts; no thread, no decoder.
struct HapFileSource::OpenedFile::Data {
    AVFormatContext* formatCtx = nullptr;
    int videoStreamIndex = -1;
    int width = 0;
    int height = 0;
    double timeBase = 0.0;
    int64_t startPts = 0;      // stream start_time, in time_base units
    double duration = 0.0;
    double frameSeconds = 1.0 / 30.0;

    ~Data() {
        if (formatCtx) avformat_close_input(&formatCtx);
    }

    // Reads the container's header and stream info (file I/O, up to tens of ms under
    // load: off the main thread, TD-35). Null, logged, on failure.
    static std::unique_ptr<Data> open(const std::filesystem::path& path) {
        auto d = std::make_unique<Data>();
        // FFmpeg takes UTF-8 file names on Windows; path::string() is the ANSI code page.
        const std::string pathStr = utf8FromPath(path);
        if (openLocalMediaInput(&d->formatCtx, pathStr.c_str()) < 0) {
            d->formatCtx = nullptr;
            Logger::error("HapFileSource: avformat_open_input failed for '{}'", pathStr);
            return nullptr;
        }
        if (avformat_find_stream_info(d->formatCtx, nullptr) < 0) {
            Logger::error("HapFileSource: avformat_find_stream_info failed for '{}'", pathStr);
            return nullptr;
        }

        // Find the HAP video stream (do NOT open a decoder).
        for (unsigned int i = 0; i < d->formatCtx->nb_streams; ++i) {
            AVCodecParameters* cp = d->formatCtx->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_HAP) {
                d->videoStreamIndex = static_cast<int>(i);
                break;
            }
        }
        if (d->videoStreamIndex < 0) {
            Logger::error("HapFileSource: no HAP video stream in '{}'", pathStr);
            return nullptr;
        }

        AVStream* stream = d->formatCtx->streams[d->videoStreamIndex];
        d->width = stream->codecpar->width;
        d->height = stream->codecpar->height;
        if (const std::string tooLarge = pictureSizeProblem(d->width, d->height);
            !tooLarge.empty()) {
            Logger::error("HapFileSource: '{}': {}", pathStr, tooLarge);
            return nullptr;
        }
        d->timeBase = av_q2d(stream->time_base);
        // Frame times are relative to the stream start, so pts 0 is the first frame.
        d->startPts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

        // The video stream's own duration, when recorded: the clock loops at it, and a
        // container duration padded by audio would freeze the last frame each pass.
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            d->duration = static_cast<double>(stream->duration) * d->timeBase;
        } else if (d->formatCtx->duration != AV_NOPTS_VALUE) {
            d->duration = static_cast<double>(d->formatCtx->duration) / AV_TIME_BASE;
        }
        const AVRational rate = av_guess_frame_rate(d->formatCtx, stream, nullptr);
        d->frameSeconds = rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 1.0 / 30.0;

        if (d->width <= 0 || d->height <= 0) {
            Logger::error("HapFileSource: invalid dimensions {}x{} in '{}'", d->width, d->height, pathStr);
            return nullptr;
        }
        return d;
    }
};

/**
 * @brief FFmpeg demux + HAP parse behind the pimpl (ADR-011, ADR-044).
 *
 * The HAP stream is DEMUXED only — never sent to the FFmpeg HAP decoder. Each
 * demuxed packet's bytes ARE one HAP frame, parsed straight to BCn via
 * hap::parseHapFrame on a background thread and staged as a compressed FrameData
 * for the main thread to upload (no GL here — rendering rule).
 *
 * Playback follows a PlaybackTransport (ADR-050). The demux thread walks the clock's
 * legs: forward legs read packets in order, Ping-Pong's reverse legs seek to each
 * earlier frame in turn. HAP is intra-only, so reverse playback is frame-exact.
 */
class HapFileSource::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    // Unload; from now until adopt() or failOpen() the transport is taken (its clock
    // runs) but nothing is demuxed.
    void beginOpen() {
        close();
        opening_ = true;
    }

    // The container opened (off the main thread): start demuxing at the clock's position.
    // Play, seek, loop, speed and lock given while it opened carry over.
    void adopt(OpenedFile::Data& file) {
        opening_ = false;
        formatCtx_ = std::exchange(file.formatCtx, nullptr);
        videoStreamIndex_ = file.videoStreamIndex;
        width_ = file.width;
        height_ = file.height;
        timeBase_ = file.timeBase;
        startPts_ = file.startPts;
        duration_ = file.duration;
        frameSeconds_ = file.frameSeconds;

        isOpen_ = true;
        atEnd_ = false;
        observedEnd_ = 0.0;
        endSeen_ = false;
        durationChecked_ = false;
        transport_.setDuration(duration_);
        // Demux from the clock's position (bounded by the staging queue) so the first
        // frame is on screen even before play.
        applyTransport();
        Logger::info("HapFileSource: demuxing HAP ({}x{}, {}s)", width_, height_, duration_);
    }

    // The open failed: the source stays empty with the transport it was given.
    void failOpen() { opening_ = false; }

    bool isOpening() const { return opening_; }

    void close() {
        stopThread();
        if (formatCtx_) {
            avformat_close_input(&formatCtx_);
            formatCtx_ = nullptr;
        }
        clearQueue();
        buffers_.clear();
        videoStreamIndex_ = -1;
        width_ = 0;
        height_ = 0;
        timeBase_ = 0.0;
        startPts_ = 0;
        duration_ = 0.0;
        isOpen_ = false;
        opening_ = false;
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
    double duration() const { return transport_.clock().duration(); }
    double position() const { return position_; }

    // --- Playback control (the clock lives in the transport; main thread) ---

    void play() { if (takesTransport()) { transport_.play(); applyTransport(); } }
    void pause() { if (takesTransport()) { transport_.pause(); applyTransport(); } }
    void stop() { if (takesTransport()) { transport_.stop(); applyTransport(); } }
    void togglePlayPause() {
        if (transport_.state() == PlaybackState::PLAYING) pause(); else play();
    }
    void seek(double timeSeconds) { if (takesTransport()) { transport_.seek(timeSeconds); applyTransport(); } }

    PlaybackState state() const { return transport_.state(); }

    void setSpeed(float speed) { transport_.setSpeed(speed); }
    float speed() const { return transport_.speed(); }

    // HAP frames are all keyframes: Ping-Pong always runs backwards exactly.
    void setLoopMode(LoopMode mode) {
        transport_.setLoopMode(mode);
        if (takesTransport()) applyTransport();
    }
    LoopMode loopMode() const { return transport_.loopMode(); }
    std::string loopModeNotice() const { return transport_.loopModeNotice(); }

    void setTimelineLocked(bool locked, const PlaybackClock& master) {
        transport_.setTimelineLocked(locked, master);
        if (takesTransport()) applyTransport();
    }
    bool timelineLocked() const { return transport_.timelineLocked(); }
    void followTimeline(const PlaybackClock& master) { transport_.followTimeline(master); }

    const PlaybackClock& clock() const { return transport_.clock(); }
    PlaybackTimePoint now() const { return transport_.now(); }
    void setTimeSource(PlaybackTransport::TimeSource source) { transport_.setTimeSource(std::move(source)); }

    // Take the staged frames that are due on the clock; the newest due one is returned
    // in out. Same rule as VideoPlayer::update (FrameSelection.hpp): never ahead of the
    // clock, late frames dropped.
    bool update(FrameData& out) {
        if (!isOpen_) {
            return false;
        }
        checkDuration();
        if (transport_.takeReposition()) reposition();

        const PlaybackTimePoint now = transport_.now();
        const PlaybackClock& clock = transport_.clock();
        const double timeline = clock.timeline(now);
        const ClipPosition clip = clock.sample(now);
        const bool playing = transport_.state() == PlaybackState::PLAYING;
        position_ = clip.position;

        size_t taken = 0;
        // A paused source holds its frame (see VideoPlayer::update).
        if (playing || !clock.running() || refreshPending_) {
            // Once: at the end take whatever is left.
            const double target = playing && clip.ended ? std::numeric_limits<double>::infinity()
                                                        : timeline;
            std::lock_guard<std::mutex> lock(queueMutex_);
            taken = popDueFrames(
                target, !refreshPending_,
                [this](double& due) {
                    if (frameQueue_.empty()) return false;
                    due = frameQueue_.front().due;
                    return true;
                },
                [this, &out]() {
                    shownDue_ = frameQueue_.front().due;
                    out = std::move(frameQueue_.front().data);
                    frameQueue_.pop();
                });
            // The upload starts copying the next frame early (ADR-044 amendment).
            if (taken > 0 && !frameQueue_.empty()) out.nextCompressed = frameQueue_.front().data.compressed;
        }
        if (taken > 0) {
            refreshPending_ = false;
            queueCond_.notify_one();
        }

        // The clock jumped ahead since the last update (a stall) and nothing staged is
        // near it: resume demuxing at the clock (see VideoPlayer::update).
        const bool clockJumped = timeline - lastTimeline_ > RESYNC_LATENESS;
        lastTimeline_ = timeline;
        if (playing && clockJumped && !refreshPending_ && shownDue_ < timeline - RESYNC_LATENESS &&
            !atEnd_ && staleQueue(timeline)) {
            reposition();
        }

        if (playing && !transport_.timelineLocked()) handleEndOfClip(clip);
        return taken > 0;
    }

private:
    struct StagedFrame {
        double due = 0.0;  // timeline time at which the frame becomes current
        FrameData data;
    };

    enum class Phase { Forward, Reverse, Done };

    // What one demux step produced.
    enum class Step { Frame, Continue, Idle, Failed };

    bool takesTransport() const { return opening_ || isOpen_; }

    // While the file opens the request waits: adopt() starts demuxing wherever the clock
    // is then.
    void applyTransport() {
        if (isOpen_ && transport_.takeReposition()) reposition();
        position_ = transport_.clock().position(transport_.now());
    }

    // Nothing staged, or only frames already too late to catch up with.
    bool staleQueue(double timeline) const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return frameQueue_.empty() || frameQueue_.front().due < timeline - RESYNC_LATENESS;
    }

    // Restart the demux thread at the clock's position.
    void reposition() {
        stopThread();
        clearQueue();
        plan_.timeline = transport_.clock().timeline(transport_.now());
        lastTimeline_ = plan_.timeline;
        plan_.duration = transport_.clock().duration();
        plan_.loopMode = transport_.clock().loopMode();
        leg_ = legAt(plan_.timeline, plan_.duration, plan_.loopMode);
        const ClipPosition clip = mapTimeline(plan_.timeline, plan_.duration, plan_.loopMode);
        if (leg_.direction > 0) {
            beginForward(clip.position);
        } else {
            beginReverse(clip.position + FRAME_DUE_EPSILON, /*knownEnd=*/false);
        }
        refreshPending_ = true;
        startThread();
    }

    // Only Once reaches an end; Loop and Ping-Pong demux forever.
    void handleEndOfClip(const ClipPosition& clip) {
        if (!clip.ended || !atEnd_) return;
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (frameQueue_.empty()) transport_.finish();
    }

    // Once a forward pass reached the end, correct a missing or wrong duration so the
    // clock wraps where the video really ends (see VideoPlayer::checkDuration).
    void checkDuration() {
        if (durationChecked_ || !endSeen_) return;
        durationChecked_ = true;
        const double end = observedEnd_;
        if (std::abs(end - transport_.clock().duration()) > 0.5 * frameSeconds_) {
            Logger::info("HapFileSource: video ends at {} s, not the declared {} s", end,
                         transport_.clock().duration());
            transport_.setDuration(end);
        }
    }

    bool planLoops() const {
        return plan_.loopMode != LoopMode::ONCE && plan_.duration >= MIN_WRAP_DURATION;
    }

    int64_t toTicks(double seconds) const {
        return startPts_ + static_cast<int64_t>(std::llround(seconds / timeBase_));
    }

    double packetPts(const AVPacket* packet) const {
        if (timeBase_ <= 0.0) return 0.0;
        if (packet->pts != AV_NOPTS_VALUE) return static_cast<double>(packet->pts - startPts_) * timeBase_;
        if (packet->dts != AV_NOPTS_VALUE) return static_cast<double>(packet->dts - startPts_) * timeBase_;
        return 0.0;
    }

    // Demuxer at the frame on screen at `position` (every HAP frame is a keyframe).
    void beginForward(double position) {
        phase_ = Phase::Forward;
        atEnd_ = false;
        if (timeBase_ > 0.0 &&
            av_seek_frame(formatCtx_, videoStreamIndex_, toTicks(position), AVSEEK_FLAG_BACKWARD) < 0) {
            Logger::warning("HapFileSource: seek to {}s failed", position);
        }
    }

    // Reverse from the frame before `bound`. knownEnd: the first frame shown ends at
    // the clip's end (a leg starting there); otherwise its end is read from the file.
    void beginReverse(double bound, bool knownEnd) {
        phase_ = Phase::Reverse;
        atEnd_ = false;
        reverseBound_ = std::min(bound, plan_.duration);
        reverseEnd_ = knownEnd ? plan_.duration : std::numeric_limits<double>::quiet_NaN();
    }

    void advanceLeg() {
        leg_ = nextLeg(leg_, plan_.duration, plan_.loopMode);
        if (leg_.direction > 0) {
            beginForward(0.0);
        } else {
            beginReverse(plan_.duration, /*knownEnd=*/true);
        }
    }

    // Read up to the next packet of the video stream. Returns the av_read_frame code.
    int readVideoPacket(AVPacket* packet) {
        for (;;) {
            const int rc = av_read_frame(formatCtx_, packet);
            if (rc < 0 || packet->stream_index == videoStreamIndex_) return rc;
            av_packet_unref(packet);
        }
    }

    // A frame buffer no one else holds (not queued, shown or being uploaded), or a new
    // one. Reusing them keeps the 1-8 MB allocations and their release off both threads:
    // freeing them on the main thread cost up to a millisecond a frame (TD-31).
    std::shared_ptr<std::vector<uint8_t>> takeBuffer() {
        for (const auto& buffer : buffers_) {
            if (buffer.use_count() == 1) {
                // The last other holder has let go: its reads come before our writes.
                std::atomic_thread_fence(std::memory_order_acquire);
                return buffer;
            }
        }
        auto buffer = std::make_shared<std::vector<uint8_t>>();
        if (buffers_.size() < kMaxBuffers) buffers_.push_back(buffer);
        return buffer;
    }

    // Parse a HAP packet into a staged frame; false (and a logged reason) if unusable.
    bool stage(const AVPacket* packet, double pts, StagedFrame& staged, std::string& failure) {
        // The packet bytes ARE the HAP frame — parse straight to BCn, into a reused buffer.
        std::shared_ptr<std::vector<uint8_t>> buffer = takeBuffer();
        hap::HapDecodedFrame& decoded = decoded_;
        decoded.textureData.swap(*buffer);
        hap::parseHapFrame(packet->data, static_cast<size_t>(packet->size), decoded);
        buffer->swap(decoded.textureData);
        if (buffer->empty() || decoded.format == hap::HapTextureFormat::None) {
            Logger::warning("HapFileSource: parseHapFrame produced an invalid frame; skipping");
            failure = "HAP frames could not be decoded";
            return false;
        }
        GpuCompressedFormat cf = toCompressedFormat(decoded.format);
        if (cf == GpuCompressedFormat::None) {
            Logger::warning("HapFileSource: unsupported HAP texture format; skipping");
            return false;
        }
        if (!decoded.alphaPlane.empty() && !loggedAlphaPlane_) {
            Logger::warning("HapFileSource: HapQ-Alpha second plane present but deferred (v1) — primary plane only");
            loggedAlphaPlane_ = true;
        }
        staged.data.width = width_;
        staged.data.height = height_;
        staged.data.compressedFormat = cf;
        staged.data.compressed = std::move(buffer);
        staged.data.timestamp = static_cast<uint64_t>(std::max(pts, 0.0) * 1000000.0);
        staged.data.frameNumber = frameCounter_++;
        return true;
    }

    Step stepForward(AVPacket* packet, StagedFrame& staged, std::string& failure) {
        const int rc = readVideoPacket(packet);
        if (rc == AVERROR_EOF) {
            endSeen_ = true;  // observedEnd_ now includes the last frame
            if (leg_.final) {
                phase_ = Phase::Done;
                atEnd_ = true;  // idle until a reposition or stop
                return Step::Idle;
            }
            advanceLeg();
            return Step::Continue;
        }
        if (rc < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(rc, err, sizeof(err));
            failure = std::string("HAP read failed: ") + err;
            return Step::Failed;
        }
        const double pts = packetPts(packet);
        observedEnd_ = std::max(observedEnd_.load(), pts + frameSeconds_);
        // A frame stamped past the clip's end would show after the next pass began.
        if (planLoops() && pts >= plan_.duration - FRAME_DUE_EPSILON) {
            av_packet_unref(packet);
            return Step::Continue;
        }
        const bool ok = stage(packet, pts, staged, failure);
        av_packet_unref(packet);
        if (!ok) return failure.empty() ? Step::Continue : Step::Failed;
        staged.due = frameDueTime(leg_, plan_.duration, pts, pts + frameSeconds_);
        return Step::Frame;
    }

    // The frame before reverseBound_: seek to it, read it, stage it newest-first.
    Step stepReverse(AVPacket* packet, StagedFrame& staged, std::string& failure) {
        if (reverseBound_ <= FRAME_DUE_EPSILON) {  // the first frame was shown
            advanceLeg();
            return Step::Continue;
        }
        int64_t target = toTicks(reverseBound_) - 1;
        const int64_t step = std::max<int64_t>(1, std::llround(frameSeconds_ / timeBase_));
        for (int attempt = 0; attempt <= MAX_REVERSE_RETRIES; ++attempt) {
            if (stopFlag_) return Step::Idle;
            if (target < startPts_ ||
                av_seek_frame(formatCtx_, videoStreamIndex_, target, AVSEEK_FLAG_BACKWARD) < 0) {
                break;  // nothing earlier
            }
            const int rc = readVideoPacket(packet);
            if (rc < 0) break;
            const double pts = packetPts(packet);
            if (pts >= reverseBound_ - FRAME_DUE_EPSILON) {
                av_packet_unref(packet);  // the demuxer landed late: step further back
                target -= step << std::min(attempt, 16);
                continue;
            }
            const bool ok = stage(packet, pts, staged, failure);
            av_packet_unref(packet);
            double end = reverseEnd_;
            if (std::isnan(end)) {
                // Starting mid-leg: this frame lasts until the next one in the file.
                end = plan_.duration;
                if (readVideoPacket(packet) >= 0) {
                    end = std::min(packetPts(packet), plan_.duration);
                    av_packet_unref(packet);
                }
            }
            reverseBound_ = pts;
            reverseEnd_ = pts;
            if (!ok) return failure.empty() ? Step::Continue : Step::Failed;
            staged.due = frameDueTime(leg_, plan_.duration, pts, std::max(end, pts));
            return Step::Frame;
        }
        reverseBound_ = 0.0;  // no earlier frame found: the leg is over
        return Step::Continue;
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

    // Background thread entry: nothing may escape a std::thread (that terminates the app).
    void demuxLoop() {
        try {
            runDemux();
        } catch (const std::exception& e) {
            failDemux(e.what());
        } catch (...) {
            failDemux("unknown error");
        }
    }

    void failDemux(const char* what) {
        Logger::error("HapFileSource: demux thread stopped: {}", what);
        std::lock_guard<std::mutex> lock(queueMutex_);
        lastError_ = std::string("Playback stopped: ") + what;
    }

    // Produce staged BCn frames in timeline order.
    void runDemux() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            Logger::error("HapFileSource: av_packet_alloc failed on demux thread");
            return;
        }

        DecodeBackoff backoff;
        int emptyLegs = 0;
        while (!stopFlag_) {
            // Backpressure: wait until there's room in the staging queue.
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCond_.wait(lock, [this]() {
                    return stopFlag_ || frameQueue_.size() < kMaxQueued;
                });
            }
            if (stopFlag_) break;

            StagedFrame staged;
            std::string failure;
            const Phase phaseBefore = phase_;
            const int64_t legBefore = leg_.index;
            Step step = Step::Idle;
            switch (phase_) {
                case Phase::Forward: step = stepForward(packet, staged, failure); break;
                case Phase::Reverse: step = stepReverse(packet, staged, failure); break;
                case Phase::Done:    step = Step::Idle; break;
            }

            if (step == Step::Continue) {
                // Guard against a clip whose legs never produce a frame.
                if (leg_.index != legBefore || phase_ != phaseBefore) {
                    if (++emptyLegs > 3) {
                        onDemuxFailure(backoff, "HAP clip has no frames to show");
                        emptyLegs = 0;
                    }
                }
                continue;
            }
            if (step == Step::Idle) {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCond_.wait_for(lock, std::chrono::milliseconds(10),
                                    [this]() { return stopFlag_.load(); });
                continue;
            }
            if (step == Step::Failed) {
                onDemuxFailure(backoff, failure);
                continue;
            }

            emptyLegs = 0;
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
    double frameSeconds_ = 1.0 / 30.0;
    bool isOpen_ = false;
    bool opening_ = false;  // beginOpen() ran; adopt() or failOpen() has not

    // Playback (main thread)
    PlaybackTransport transport_;
    double position_ = 0.0;        // clip position at the last update
    bool refreshPending_ = false;  // an open/seek made the shown frame stale
    double shownDue_ = 0.0;        // due time of the frame last taken
    double lastTimeline_ = 0.0;    // clock timeline at the previous update
    uint64_t frameCounter_ = 0;
    bool loggedAlphaPlane_ = false;

    // Demux plan (demux thread; reposition() sets it up with the thread stopped)
    DecodePlan plan_;
    PlaybackLeg leg_;
    Phase phase_ = Phase::Done;
    double reverseBound_ = 0.0;  // reverse: the next frame shown is the one before this
    double reverseEnd_ = 0.0;    // reverse: where that frame's interval ends (NaN: read it)
    std::atomic<double> observedEnd_{0.0};  // latest frame end read going forward
    std::atomic<bool> endSeen_{false};      // a forward pass reached the end of the file
    bool durationChecked_ = false;          // main thread

    // Staging queue (background -> main)
    static constexpr size_t kMaxQueued = 8;
    // Reused frame buffers (demux thread): the queue, the frame shown, the one the upload
    // is copying, and slack. Past this a frame gets a buffer of its own.
    static constexpr size_t kMaxBuffers = kMaxQueued + 4;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> buffers_;
    hap::HapDecodedFrame decoded_;  // demux thread; its buffers are reused too
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
struct HapFileSource::OpenedFile::Data {
    static std::unique_ptr<Data> open(const std::filesystem::path&) { return nullptr; }
};

class HapFileSource::Impl {
public:
    void beginOpen() { opening_ = true; }
    void adopt(OpenedFile::Data&) { opening_ = false; }
    void failOpen() { opening_ = false; }
    bool isOpening() const { return opening_; }
    void close() { opening_ = false; }
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
    std::string loopModeNotice() const { return {}; }
    void setTimelineLocked(bool, const PlaybackClock&) {}
    bool timelineLocked() const { return false; }
    void followTimeline(const PlaybackClock&) {}
    const PlaybackClock& clock() const { return clock_; }
    PlaybackTimePoint now() const { return std::chrono::steady_clock::now(); }
    void setTimeSource(PlaybackTransport::TimeSource) {}
    void seek(double) {}
    bool update(FrameData&) { return false; }
    std::string lastError() const { return {}; }
private:
    PlaybackClock clock_;
    bool opening_ = false;
};

#endif // QUADMAPPER_HAS_FFMPEG

// --- Public interface ---

HapFileSource::HapFileSource(const std::filesystem::path& path)
    : name_(utf8FromPath(path.filename()))
    , path_(path)
    , impl_(std::make_unique<Impl>())
{
}

HapFileSource::~HapFileSource() {
    shutdown();
}

HapFileSource::OpenedFile::OpenedFile(std::unique_ptr<Data> data) : data_(std::move(data)) {}
HapFileSource::OpenedFile::~OpenedFile() = default;

bool HapFileSource::initialize() {
    if (ready_) {
        return true;
    }
    beginOpen();
    return finishOpen(openFile(path_));
}

std::unique_ptr<HapFileSource::OpenedFile> HapFileSource::openFile(const std::filesystem::path& path) {
    std::unique_ptr<OpenedFile::Data> data = OpenedFile::Data::open(path);
    if (!data) return nullptr;
    return std::make_unique<OpenedFile>(std::move(data));
}

void HapFileSource::beginOpen() {
    openFailed_ = false;
    ready_ = false;
    impl_->beginOpen();
}

bool HapFileSource::finishOpen(std::unique_ptr<OpenedFile> file) {
    if (!impl_->isOpening()) return false;
    if (!file || !file->data_) {
        impl_->failOpen();
        Logger::error("HapFileSource: Failed to load '{}'", utf8FromPath(path_));
        openFailed_ = true;  // the transport it was given is kept, for the project to save
        return false;
    }
    impl_->adopt(*file->data_);
    ready_ = true;
    return true;
}

bool HapFileSource::isOpening() const {
    return impl_ && impl_->isOpening();
}

void HapFileSource::shutdown() {
    if (impl_) {
        impl_->close();
    }
    frame_.clear();
    ready_ = false;
    hasNewFrame_ = false;
}

bool HapFileSource::update(double /*deltaTime: the playback clock reads the wall clock*/) {
    if (!ready_) {
        return false;
    }
    FrameData staged;
    if (impl_->update(staged) && staged.isValid()) {
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
std::string HapFileSource::getLoopModeNotice() const { return impl_ ? impl_->loopModeNotice() : std::string(); }

void HapFileSource::setTimelineLocked(bool locked, const PlaybackClock& master) {
    if (impl_) impl_->setTimelineLocked(locked, master);
}
bool HapFileSource::isTimelineLocked() const { return impl_ && impl_->timelineLocked(); }
void HapFileSource::followTimeline(const PlaybackClock& master) {
    if (impl_) impl_->followTimeline(master);
}

const PlaybackClock& HapFileSource::getClock() const { return impl_->clock(); }
PlaybackTimePoint HapFileSource::getClockNow() const { return impl_->now(); }
void HapFileSource::setTimeSource(std::function<PlaybackTimePoint()> source) {
    impl_->setTimeSource(std::move(source));
}

std::string HapFileSource::getErrorMessage() const {
    if (openFailed_) return "The video file could not be opened.";
    return impl_ ? impl_->lastError() : std::string();
}

} // namespace quadmapper
