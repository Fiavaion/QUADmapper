#include "VideoDecoder.hpp"
#include "DecodeBackoff.hpp"
#include "FrameSelection.hpp"
#include "PlaybackClock.hpp"
#include "core/Logger.hpp"

#ifdef QUADMAPPER_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#ifdef QUADMAPPER_HAS_AVDEVICE
#include <libavdevice/avdevice.h>
#endif
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace quadmapper {

namespace {

constexpr size_t MEGABYTE = size_t(1) << 20;
// Seeks further back tried when a reverse step finds nothing before its bound.
constexpr int MAX_REVERSE_RETRIES = 20;
// Leg changes in a row that produced no frame: the plan cannot make progress.
constexpr int MAX_EMPTY_LEGS = 3;

size_t frameBytes(const AVFrame* frame) {
    size_t bytes = 0;
    for (const AVBufferRef* buf : frame->buf) {
        if (buf) bytes += buf->size;
    }
    return bytes;
}

std::string formatBytes(double bytes) {
    if (bytes >= static_cast<double>(MEGABYTE)) {
        return std::to_string(static_cast<int64_t>(bytes / MEGABYTE)) + " MB";
    }
    return std::to_string(static_cast<int64_t>(bytes / 1024.0)) + " KB";
}

std::string reverseAdvice(size_t cacheBytes) {
    return "more than the " + formatBytes(static_cast<double>(cacheBytes)) +
           " reverse cache. Re-encode as HAP or all-intra for Ping-Pong.";
}

}  // namespace

class VideoDecoder::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    bool open(const std::filesystem::path& path) {
        close();

        std::string pathStr = path.string();

        // Open input file
        if (avformat_open_input(&formatCtx_, pathStr.c_str(), nullptr, nullptr) < 0) {
            LOG_ERROR("Failed to open video file: {}", pathStr);
            return false;
        }

        isLive_ = false;
        return setupDecoderFromFormat();
    }

    bool openCamera(const std::string& deviceName, int width, int height, double fps) {
        close();
#ifdef QUADMAPPER_HAS_AVDEVICE
        avdevice_register_all();
        const AVInputFormat* ifmt = av_find_input_format("dshow");
        if (!ifmt) {
            LOG_ERROR("dshow input format not available");
            return false;
        }

        AVDictionary* opts = nullptr;
        if (width > 0 && height > 0) {
            std::string sz = std::to_string(width) + "x" + std::to_string(height);
            av_dict_set(&opts, "video_size", sz.c_str(), 0);
        }
        if (fps > 0.0) {
            av_dict_set(&opts, "framerate", std::to_string(static_cast<int>(fps)).c_str(), 0);
        }
        // Generous real-time buffer so brief decode hitches don't drop frames
        av_dict_set(&opts, "rtbufsize", "100000000", 0);

        std::string url = "video=" + deviceName;
        int rc = avformat_open_input(&formatCtx_, url.c_str(), ifmt, &opts);
        av_dict_free(&opts);
        if (rc < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(rc, err, sizeof(err));
            LOG_ERROR("Failed to open camera '{}': {}", deviceName, err);
            return false;
        }

        isLive_ = true;
        if (!setupDecoderFromFormat()) {
            return false;
        }
        info_.duration = 0.0;   // live capture: no duration, no seek
        info_.totalFrames = 0;
        LOG_INFO("Opened camera '{}': {}x{}, {} fps", deviceName, info_.width, info_.height, info_.frameRate);
        return true;
#else
        (void)deviceName; (void)width; (void)height; (void)fps;
        LOG_ERROR("openCamera requires libavdevice (not compiled in)");
        return false;
#endif
    }

    // Shared decoder setup; assumes formatCtx_ is already open on an input.
    bool setupDecoderFromFormat() {
        // Find stream info
        if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
            LOG_ERROR("Failed to find stream info");
            close();
            return false;
        }

        // Find video stream
        videoStreamIndex_ = -1;
        audioStreamIndex_ = -1;
        for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
            if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex_ < 0) {
                videoStreamIndex_ = static_cast<int>(i);
            }
            if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex_ < 0) {
                audioStreamIndex_ = static_cast<int>(i);
            }
        }

        if (videoStreamIndex_ < 0) {
            LOG_ERROR("No video stream found");
            close();
            return false;
        }

        // Get codec parameters
        AVCodecParameters* codecPar = formatCtx_->streams[videoStreamIndex_]->codecpar;

        // Find decoder
        const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Unsupported codec");
            close();
            return false;
        }

        // Allocate codec context
        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_) {
            LOG_ERROR("Failed to allocate codec context");
            close();
            return false;
        }

        // Copy codec parameters
        if (avcodec_parameters_to_context(codecCtx_, codecPar) < 0) {
            LOG_ERROR("Failed to copy codec parameters");
            close();
            return false;
        }

        // Open codec
        if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            LOG_ERROR("Failed to open codec");
            close();
            return false;
        }

        // Allocate frame
        frame_ = av_frame_alloc();
        skippedFrame_ = av_frame_alloc();
        rgbFrame_ = av_frame_alloc();
        if (!frame_ || !skippedFrame_ || !rgbFrame_) {
            LOG_ERROR("Failed to allocate frames");
            close();
            return false;
        }

        // Setup RGB frame buffer
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codecCtx_->width, codecCtx_->height, 1);
        rgbBuffer_.resize(numBytes);
        av_image_fill_arrays(rgbFrame_->data, rgbFrame_->linesize, rgbBuffer_.data(),
                           AV_PIX_FMT_RGB24, codecCtx_->width, codecCtx_->height, 1);

        // Setup scaler
        swsCtx_ = sws_getContext(
            codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
            codecCtx_->width, codecCtx_->height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!swsCtx_) {
            LOG_ERROR("Failed to create scaler context");
            close();
            return false;
        }

        // Fill video info
        AVStream* videoStream = formatCtx_->streams[videoStreamIndex_];
        info_.width = codecCtx_->width;
        info_.height = codecCtx_->height;
        info_.codecName = codec->name;

        // Duration: the video stream's own, when the container records it, since the
        // playback clock loops at it (a longer audio track would add a frozen tail);
        // else the container's.
        if (videoStream->duration != AV_NOPTS_VALUE && videoStream->duration > 0) {
            info_.duration = static_cast<double>(videoStream->duration) * av_q2d(videoStream->time_base);
        } else if (formatCtx_->duration != AV_NOPTS_VALUE) {
            info_.duration = static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
        }

        // Calculate frame rate
        AVRational frameRate = av_guess_frame_rate(formatCtx_, videoStream, nullptr);
        if (frameRate.num > 0 && frameRate.den > 0) {
            info_.frameRate = static_cast<double>(frameRate.num) / frameRate.den;
        }

        // Calculate total frames
        if (videoStream->nb_frames > 0) {
            info_.totalFrames = videoStream->nb_frames;
        } else if (info_.duration > 0 && info_.frameRate > 0) {
            info_.totalFrames = static_cast<int64_t>(info_.duration * info_.frameRate);
        }

        // Audio info
        if (audioStreamIndex_ >= 0) {
            AVCodecParameters* audioPar = formatCtx_->streams[audioStreamIndex_]->codecpar;
            info_.hasAudio = true;
            info_.audioChannels = audioPar->ch_layout.nb_channels;
            info_.audioSampleRate = audioPar->sample_rate;
        }

        timeBase_ = av_q2d(videoStream->time_base);
        // Frame times are reported relative to the stream start, so pts 0 is the
        // first frame even in containers whose timestamps begin elsewhere (MPEG-TS).
        startPts_ = videoStream->start_time != AV_NOPTS_VALUE ? videoStream->start_time : 0;
        frameTicks_ = info_.frameRate > 0.0
            ? std::max<int64_t>(1, std::llround(1.0 / (info_.frameRate * timeBase_)))
            : 1;
        frameSeconds_ = info_.frameRate > 0.0 ? 1.0 / info_.frameRate : 0.04;
        const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codecCtx_->codec_id);
        intraOnly_ = descriptor && (descriptor->props & AV_CODEC_PROP_INTRA_ONLY);
        const bool probeEnd = !isLive_ &&
            (videoStream->duration == AV_NOPTS_VALUE || videoStream->duration <= 0);
        if (probeEnd) {
            // No video duration recorded: measure where the video really ends, since the
            // playback clock loops there (Matroska declares its end time, start offset
            // included; MPEG-TS only has an estimate).
            const double end = probeVideoEnd(info_.duration);
            if (end > 0.0) info_.duration = end;
        }
        seekTargetPts_ = AV_NOPTS_VALUE;
        checkFirstPacket_ = false;
        checkFirstFrame_ = false;
        keepPreroll_ = false;
        isOpen_ = true;
        currentFrame_ = 0;
        currentTime_ = 0.0;
        atEnd_ = false;
        observedEnd_ = 0.0;
        endSeen_ = false;
        if (probeEnd) seekExact(0.0);  // back to the first frame after reading the tail

        LOG_INFO("Opened video: {}x{}, {} fps, {:.2f}s", info_.width, info_.height, info_.frameRate, info_.duration);
        return true;
    }

    void close() {
        stopBackgroundDecoding();
        releaseReverse();
        releaseTail();

        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        if (frame_) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (skippedFrame_) {
            av_frame_free(&skippedFrame_);
            skippedFrame_ = nullptr;
        }
        if (rgbFrame_) {
            av_frame_free(&rgbFrame_);
            rgbFrame_ = nullptr;
        }
        if (codecCtx_) {
            avcodec_free_context(&codecCtx_);
            codecCtx_ = nullptr;
        }
        if (formatCtx_) {
            avformat_close_input(&formatCtx_);
            formatCtx_ = nullptr;
        }

        rgbBuffer_.clear();
        info_ = VideoInfo{};
        videoStreamIndex_ = -1;
        audioStreamIndex_ = -1;
        isOpen_ = false;
        isLive_ = false;
        planned_ = false;
        currentFrame_ = 0;
        currentTime_ = 0.0;
        atEnd_ = false;
        observedEnd_ = 0.0;
        endSeen_ = false;
        reversePeakBytes_ = 0;
    }

    bool isOpen() const { return isOpen_; }
    bool isLive() const { return isLive_; }
    const VideoInfo& getInfo() const { return info_; }
    double getCurrentTime() const { return currentTime_; }
    int64_t getCurrentFrame() const { return currentFrame_; }
    bool isAtEnd() const { return atEnd_; }
    double getObservedEnd() const { return endSeen_ ? observedEnd_.load() : 0.0; }
    size_t getReverseCachePeakBytes() const { return reversePeakBytes_; }

    bool seek(double timeSeconds) {
        if (!isOpen_) return false;
        if (!seekExact(timeSeconds)) return false;
        planned_ = false;  // back to plain forward decoding

        // Clear background buffer on seek
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            std::queue<VideoFrame> empty;
            std::swap(frameBuffer_, empty);
        }
        bufferCondition_.notify_all();

        return true;
    }

    bool seekFrame(int64_t frameNumber) {
        if (!isOpen_ || info_.frameRate <= 0) return false;
        double timeSeconds = static_cast<double>(frameNumber) / info_.frameRate;
        return seek(timeSeconds);
    }

    VideoFrame decodeNextFrame() {
        if (!isOpen_ || !decodeNextRaw()) return VideoFrame{};
        return convertFrame(frame_);
    }

    // --- Planned playback (ADR-050) ---

    void startPlayback(const DecodePlan& plan, size_t bufferSize) {
        stopBackgroundDecoding();  // also empties the frame buffer
        if (!isOpen_) return;
        releaseReverse();
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            reverseError_.clear();
        }
        plan_ = plan;
        planned_ = true;
        planStalled_ = false;
        emptyLegs_ = 0;
        releaseTail();
        tailOverflow_ = false;

        leg_ = legAt(plan.timeline, plan.duration, plan.loopMode);
        const ClipPosition clip = mapTimeline(plan.timeline, plan.duration, plan.loopMode);
        if (leg_.direction > 0) {
            beginForward(clip.position);
        } else {
            // Mid reverse leg: first show the frame holding the position, then earlier ones.
            beginReverse(clip.position + FRAME_DUE_EPSILON);
        }
        startBackgroundDecoding(bufferSize);
    }

    std::string checkReverse(size_t cacheBytes) const {
        if (!isOpen_ || isLive_) return "the source cannot seek.";
        if (intraOnly_) return {};  // every frame is a keyframe

        AVStream* stream = formatCtx_->streams[videoStreamIndex_];
        const int entries = avformat_index_get_entries_count(stream);
        double previousKey = std::numeric_limits<double>::quiet_NaN();
        double widestGap = 0.0;
        for (int i = 0; i < entries; ++i) {
            const AVIndexEntry* entry = avformat_index_get_entry(stream, i);
            if (!entry || !(entry->flags & AVINDEX_KEYFRAME)) continue;
            const double t = static_cast<double>(entry->timestamp - startPts_) * timeBase_;
            if (!std::isnan(previousKey)) widestGap = std::max(widestGap, t - previousKey);
            previousKey = t;
        }
        if (std::isnan(previousKey)) return {};  // no index: checked while playing
        widestGap = std::max(widestGap, info_.duration - previousKey);  // the last group

        // One group of pictures, plus the reordering delay's frames decoded past it.
        const int64_t frames = static_cast<int64_t>(std::ceil(widestGap / frameSeconds_ - 1e-6)) +
                               std::max(codecCtx_->has_b_frames, 0) + 1;
        const int perFrame = av_image_get_buffer_size(codecCtx_->pix_fmt, info_.width, info_.height, 1);
        if (perFrame <= 0) return {};
        const double needed = static_cast<double>(frames) * perFrame;
        if (needed <= static_cast<double>(cacheBytes)) return {};
        return "its keyframes are " + std::to_string(frames - 1) + " frames apart; at " +
               std::to_string(info_.width) + "x" + std::to_string(info_.height) +
               " one group of pictures needs " + formatBytes(needed) + ", " + reverseAdvice(cacheBytes);
    }

    std::string getReverseError() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return reverseError_;
    }

    // Background decoding
    void startBackgroundDecoding(size_t bufferSize) {
        if (backgroundThread_.joinable()) return;

        maxBufferSize_ = bufferSize;
        stopDecoding_ = false;
        backgroundThread_ = std::thread(&Impl::backgroundDecodingLoop, this);
    }

    void stopBackgroundDecoding() {
        if (!backgroundThread_.joinable()) return;

        // Set the flag under the mutex the decode thread waits on: set outside it,
        // the notify can land between the thread's predicate check and its wait,
        // and join() then hangs on a thread that never wakes.
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            stopDecoding_ = true;
        }
        bufferCondition_.notify_all();
        backgroundThread_.join();

        // Clear buffer
        std::lock_guard<std::mutex> lock(bufferMutex_);
        std::queue<VideoFrame> empty;
        std::swap(frameBuffer_, empty);
        lastError_.clear();
    }

    VideoFrame getBufferedFrame() {
        std::unique_lock<std::mutex> lock(bufferMutex_);
        if (frameBuffer_.empty()) {
            return VideoFrame{};
        }

        VideoFrame frame = std::move(frameBuffer_.front());
        frameBuffer_.pop();
        bufferCondition_.notify_one();
        return frame;
    }

    bool peekBufferedDue(double& due) const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (frameBuffer_.empty()) return false;
        due = frameBuffer_.front().due;
        return true;
    }

    bool hasBufferedFrames() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return !frameBuffer_.empty();
    }

    size_t getBufferedFrameCount() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return frameBuffer_.size();
    }

    std::string getLastError() const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        return lastError_;
    }

private:
    // One group of pictures decoded for the reverse leg, oldest frame first.
    struct ReverseChunk {
        std::vector<AVFrame*> frames;
        size_t bytes = 0;
        double bound = 0.0;    // holds the frames with pts < bound
        double target = 0.0;   // clip time the demuxer was sent to (keyframe at or before)
        double stopPts = std::numeric_limits<double>::quiet_NaN();  // first frame at the bound
        bool started = false;
        bool complete = false;
        bool hitEnd = false;   // decoding reached the end of the stream
        bool legEnd = false;   // nothing precedes the bound: the reverse leg is done
        int attempts = 0;
    };

    enum class Phase { Forward, Reverse, Done };

    // Decode until frame_ holds the next frame to show. False at the end of the stream
    // (atEnd_), on a read error, or when asked to stop.
    bool decodeNextRaw() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) return false;

        bool got = false;
        for (;;) {
            // Skipping to a seek target can decode seconds of video. VideoPlayer joins
            // this thread on the UI thread before every seek (a scrub seeks each frame),
            // so give up promptly when asked to stop; the next seek() resets the skip
            // and a plain resume carries on from where it stopped.
            if (stopDecoding_) break;

            int receiveResult = avcodec_receive_frame(codecCtx_, frame_);
            if (receiveResult == AVERROR_EOF && checkFirstFrame_ && retrySeekEarlier()) {
                continue;  // the seek landed after the last keyframe: nothing decoded
            }
            if (receiveResult == AVERROR_EOF && seekTargetPts_ != AV_NOPTS_VALUE &&
                skippedFrame_->buf[0]) {
                // The seek target lies past the last frame, so that frame is the one to show.
                av_frame_move_ref(frame_, skippedFrame_);
                seekTargetPts_ = AV_NOPTS_VALUE;
                receiveResult = 0;
            }
            if (receiveResult == AVERROR_EOF) {
                // Fully drained: every frame of the stream has been returned.
                atEnd_ = true;
                break;
            }
            if (receiveResult >= 0) {
                if (!reachedSeekTarget()) continue;
                got = true;
                break;
            }

            // The decoder wants more input (EAGAIN; a decode error also moves on).
            int readResult = av_read_frame(formatCtx_, packet);
            if (readResult == AVERROR_EOF) {
                // Enter draining mode so the frames still held for B-frame reordering
                // come out. Already draining (only after a decode error) means done.
                if (avcodec_send_packet(codecCtx_, nullptr) < 0) {
                    atEnd_ = true;
                    break;
                }
                continue;
            }
            if (readResult < 0) {
                lastReadError_ = readResult;
                break;
            }

            if (packet->stream_index == videoStreamIndex_) {
                // A seek that lands on a non-key packet resumes at the next keyframe.
                const bool landedOffKey = checkFirstPacket_ && !(packet->flags & AV_PKT_FLAG_KEY);
                if (checkFirstPacket_ && !landedOffKey) {
                    landedKeyPts_ = packet->pts != AV_NOPTS_VALUE
                        ? static_cast<double>(packet->pts - startPts_) * timeBase_
                        : -std::numeric_limits<double>::infinity();
                }
                checkFirstPacket_ = false;
                if (!landedOffKey || !retrySeekEarlier()) {
                    avcodec_send_packet(codecCtx_, packet);  // a corrupt packet is skipped
                }
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        return got;
    }

    double framePts(const AVFrame* frame) const {
        return frame->pts != AV_NOPTS_VALUE
            ? static_cast<double>(frame->pts - startPts_) * timeBase_
            : currentTime_.load();
    }

    VideoFrame convertFrame(const AVFrame* frame) {
        VideoFrame result;
        sws_scale(swsCtx_, frame->data, frame->linesize, 0,
                  codecCtx_->height, rgbFrame_->data, rgbFrame_->linesize);

        result.width = info_.width;
        result.height = info_.height;
        result.frameNumber = currentFrame_++;

        if (frame->pts != AV_NOPTS_VALUE) {
            result.pts = framePts(frame);
            currentTime_ = result.pts;
        } else {
            result.pts = currentTime_;
            // std::atomic<double> has no operator+= in C++17; single writer
            // (this thread), so load+store is correct.
            currentTime_.store(currentTime_.load() + frameSeconds_);
        }
        result.due = result.pts;

        const int rowBytes = info_.width * 3;
        result.data.resize(static_cast<size_t>(rowBytes) * info_.height);
        for (int y = 0; y < info_.height; y++) {
            memcpy(result.data.data() + static_cast<size_t>(y) * rowBytes,
                   rgbFrame_->data[0] + y * rgbFrame_->linesize[0],
                   rowBytes);
        }
        return result;
    }

    // End of the last video packet (pts + duration, relative to the stream start),
    // read from the packets after the keyframe before `declared` seconds; 0 if unknown.
    // Reads packets only (no decoding); the caller seeks back to the start.
    double probeVideoEnd(double declared) {
        const double target = std::max(0.0, declared - 1.0);
        if (av_seek_frame(formatCtx_, videoStreamIndex_, startPts_ + std::llround(target / timeBase_),
                          AVSEEK_FLAG_BACKWARD) < 0) {
            return 0.0;
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet) return 0.0;
        double end = 0.0;
        for (int i = 0; i < 100000 && av_read_frame(formatCtx_, packet) >= 0; ++i) {
            if (packet->stream_index == videoStreamIndex_ && packet->pts != AV_NOPTS_VALUE) {
                const double length = packet->duration > 0 ? packet->duration * timeBase_ : frameSeconds_;
                end = std::max(end, static_cast<double>(packet->pts - startPts_) * timeBase_ + length);
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        return end;
    }

    // Frame-exact seek (QM-091): the next decoded frame is the one on screen at t.
    bool seekExact(double timeSeconds) {
        const int64_t target = startPts_ + std::llround(timeSeconds / timeBase_);
        if (!seekDemuxer(target)) {
            LOG_WARNING("Seek failed");
            return false;
        }
        seekTargetPts_ = target;
        keepPreroll_ = false;
        atEnd_ = false;
        currentTime_ = timeSeconds;
        currentFrame_ = static_cast<int64_t>(timeSeconds * info_.frameRate);
        return true;
    }

    // Position at the keyframe at or before t and decode every frame from there.
    bool seekKeyframe(double timeSeconds) {
        const int64_t target = startPts_ + std::llround(timeSeconds / timeBase_);
        if (!seekDemuxer(target)) return false;
        seekTargetPts_ = target;
        keepPreroll_ = true;
        atEnd_ = false;
        return true;
    }

    // Position the demuxer at a keyframe at or before `pts` (stream time base) and
    // reset the decoder, which also clears its drained state after an EOF.
    bool seekDemuxer(int64_t pts) {
        if (av_seek_frame(formatCtx_, videoStreamIndex_, pts, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codecCtx_);
        av_frame_unref(skippedFrame_);
        seekFromPts_ = pts;
        checkFirstPacket_ = true;
        checkFirstFrame_ = true;
        landedKeyPts_ = -std::numeric_limits<double>::infinity();
        return true;
    }

    // Demuxer seeks are not always keyframe-exact: MPEG-TS bisects on packet
    // timestamps and can stop on a non-key packet, so decoding would resume at the
    // next keyframe, past the target (or reach EOF with no frame when none follows).
    // Seek back further, doubling the distance each time; false once the demuxer was
    // already positioned before the stream start, where nothing earlier exists.
    bool retrySeekEarlier() {
        checkFirstPacket_ = false;
        checkFirstFrame_ = false;
        if (seekFromPts_ < startPts_) return false;
        const int64_t oneSecond = std::llround(1.0 / timeBase_);
        const int64_t back = std::max(2 * (seekTargetPts_ - seekFromPts_), oneSecond);
        if (seekDemuxer(seekTargetPts_ - back)) return true;
        LOG_WARNING("Seek landed after the target and could not back off");
        return false;
    }

    // After a seek, whether frame_ is the one to show: a first frame past the target
    // means the seek landed late and is retried; frames that end before the target
    // are skipped, so the seek is frame-exact. A keyframe seek keeps them.
    bool reachedSeekTarget() {
        if (seekTargetPts_ == AV_NOPTS_VALUE || frame_->pts == AV_NOPTS_VALUE) return true;

        if (checkFirstFrame_) {
            checkFirstFrame_ = false;
            if (frame_->pts > seekTargetPts_ && retrySeekEarlier()) return false;
        }
        if (keepPreroll_) return true;

        if (frame_->pts + frameTicks_ <= seekTargetPts_) {
            // Held in case the stream ends before the target.
            av_frame_unref(skippedFrame_);
            av_frame_move_ref(skippedFrame_, frame_);
            return false;
        }
        av_frame_unref(skippedFrame_);
        seekTargetPts_ = AV_NOPTS_VALUE;
        return true;
    }

    // --- Planned playback: legs (decode thread, or startPlayback with it stopped) ---

    bool planLoops() const {
        return plan_.loopMode != LoopMode::ONCE && plan_.duration >= MIN_WRAP_DURATION;
    }

    void beginForward(double position) {
        phase_ = Phase::Forward;
        seekExact(position);
    }

    void beginReverse(double bound) {
        phase_ = Phase::Reverse;
        releaseReverse();
        building_.bound = std::min(bound, plan_.duration);
    }

    void advanceLeg() {
        leg_ = nextLeg(leg_, plan_.duration, plan_.loopMode);
        if (leg_.direction > 0) {
            beginForward(0.0);
        } else if (!tail_.empty()) {
            // The forward pass just decoded the last frames: show them backwards at
            // once instead of decoding them again (no pause at the reflection).
            phase_ = Phase::Reverse;
            releaseReverse();
            ready_ = std::move(tail_);
            readyBytes_ = tailBytes_;
            readyEnd_ = std::max(plan_.duration, framePts(ready_.back()));
            building_.bound = framePts(ready_.front());
            tail_.clear();
            tailBytes_ = 0;
        } else {
            beginReverse(plan_.duration);
        }
    }

    // Ping-Pong forward pass: keep the frames decoded since the last keyframe, so the
    // reverse leg that follows starts from them. Bounded by the reverse cache; a group
    // that does not fit is simply not kept.
    void keepTail(double pts) {
        if (plan_.loopMode != LoopMode::PING_PONG || !planLoops()) return;
        if (frame_->flags & AV_FRAME_FLAG_KEY) {
            releaseTail();
            tailOverflow_ = false;
        }
        if (tailOverflow_) return;
        const size_t bytes = frameBytes(frame_);
        AVFrame* copy = tailBytes_ + bytes <= plan_.reverseCacheBytes ? av_frame_clone(frame_) : nullptr;
        if (!copy || frame_->pts == AV_NOPTS_VALUE || (!tail_.empty() && pts <= framePts(tail_.back()))) {
            av_frame_free(&copy);
            releaseTail();
            tailOverflow_ = true;  // until the next keyframe
            return;
        }
        lastFrameBytes_ = std::max(lastFrameBytes_, bytes);
        tail_.push_back(copy);
        tailBytes_ += bytes;
        reversePeakBytes_ = std::max(reversePeakBytes_.load(), reverseBytes() + tailBytes_);
    }

    void releaseTail() {
        for (AVFrame*& frame : tail_) av_frame_free(&frame);
        tail_.clear();
        tailBytes_ = 0;
    }

    // The next frame in timeline order, or an invalid frame when there is none yet
    // (end of a Once clip, a read error, a stop request, or a stalled plan).
    VideoFrame producePlanned() {
        while (!stopDecoding_ && !planStalled_) {
            if (phase_ == Phase::Forward) {
                if (decodeNextRaw()) {
                    const double pts = framePts(frame_);
                    observedEnd_ = std::max(observedEnd_.load(), pts + frameSeconds_);
                    // A frame stamped past the clip's end would show after the next pass began.
                    if (planLoops() && pts >= plan_.duration - FRAME_DUE_EPSILON) continue;
                    keepTail(pts);
                    emptyLegs_ = 0;
                    VideoFrame frame = convertFrame(frame_);
                    frame.due = frameDueTime(leg_, plan_.duration, pts, pts + frameSeconds_);
                    return frame;
                }
                if (!atEnd_) return VideoFrame{};  // read error or stop
                endSeen_ = true;  // observedEnd_ now includes the last frame
                if (leg_.final) {
                    phase_ = Phase::Done;          // atEnd_ stays set: the clip is over
                    return VideoFrame{};
                }
            } else if (phase_ == Phase::Reverse) {
                if (!ready_.empty() || buildChunk()) {
                    emptyLegs_ = 0;
                    return emitReverse();
                }
                if (planStalled_ || stopDecoding_ || !building_.legEnd) return VideoFrame{};
            } else {
                return VideoFrame{};
            }
            // This pass is over: go on to the next one.
            if (++emptyLegs_ > MAX_EMPTY_LEGS) {
                stallPlan("Playback found no frames to show in the clip.");
                return VideoFrame{};
            }
            advanceLeg();
        }
        return VideoFrame{};
    }

    // --- Planned playback: the reverse leg ---

    size_t reverseBytes() const { return readyBytes_ + building_.bytes; }

    // Whether the next group can be decoded while the frame buffer is full, so the
    // reverse leg does not pause at each group boundary.
    bool canPrebuild() const {
        if (!planned_ || phase_ != Phase::Reverse || planStalled_) return false;
        if (building_.complete || ready_.empty()) return false;
        return reverseBytes() + lastFrameBytes_ <= plan_.reverseCacheBytes;
    }

    // Decode one more frame of the group being built.
    void stepBuild() {
        ReverseChunk& chunk = building_;
        if (!chunk.started) {
            if (chunk.bound <= FRAME_DUE_EPSILON) {  // nothing precedes the first frame
                chunk.complete = chunk.legEnd = true;
                return;
            }
            // A few frames early: indexes that seek by decode time would otherwise find
            // the keyframe at the bound itself (decoded before it is shown) and retry a
            // whole second back. Landing a group early costs only a trimmed decode.
            const double reorder = intraOnly_ ? 0.5 : std::max(codecCtx_->has_b_frames, 2) + 0.5;
            chunk.target = std::max(0.0, chunk.bound - reorder * frameSeconds_);
            chunk.started = true;
            if (!seekKeyframe(chunk.target)) {
                chunk.complete = chunk.legEnd = true;
                LOG_WARNING("Reverse playback: seek to {} s failed", chunk.target);
                return;
            }
        }

        if (decodeNextRaw()) {
            const double pts = framePts(frame_);
            if (frame_->pts == AV_NOPTS_VALUE) {
                stallPlan("the file has no frame timestamps to play it backwards.");
                return;
            }
            if (pts < chunk.bound - FRAME_DUE_EPSILON && (frame_->flags & AV_FRAME_FLAG_KEY) &&
                pts > landedKeyPts_ + FRAME_DUE_EPSILON) {
                // A later keyframe still before the bound: the seek landed a group early
                // (a retried seek backs off by a second or more). Only the last group is
                // needed; the frames before it belong to the next chunk.
                for (AVFrame*& old : chunk.frames) av_frame_free(&old);
                chunk.frames.clear();
                chunk.bytes = 0;
                landedKeyPts_ = pts;
            }
            if (pts < landedKeyPts_ - FRAME_DUE_EPSILON) return;  // an open GOP's leading frame
            if (pts >= chunk.bound - FRAME_DUE_EPSILON) {
                chunk.stopPts = pts;
                chunk.complete = true;
            } else {
                const size_t bytes = frameBytes(frame_);
                if (chunk.bytes + bytes > plan_.reverseCacheBytes) {
                    char at[32];
                    std::snprintf(at, sizeof(at), "%.1f s", chunk.bound);
                    stallPlan(std::string("the group of pictures before ") + at + " needs " +
                              reverseAdvice(plan_.reverseCacheBytes));
                    return;
                }
                AVFrame* copy = av_frame_clone(frame_);
                if (!copy) {
                    stallPlan("out of memory for the reverse cache.");
                    return;
                }
                // Frames of one stream share a size, so this sizes the next one too.
                lastFrameBytes_ = std::max(lastFrameBytes_, bytes);
                chunk.frames.push_back(copy);
                chunk.bytes += bytes;
                reversePeakBytes_ = std::max(reversePeakBytes_.load(), reverseBytes());
            }
        } else {
            if (stopDecoding_) return;
            chunk.hitEnd = atEnd_;
            if (!atEnd_) LOG_WARNING("Reverse playback: decode stopped early before {} s", chunk.bound);
            atEnd_ = false;  // the end of the file is not the end of a looping clip
            chunk.complete = true;
        }

        if (chunk.complete && chunk.frames.empty() && !chunk.legEnd) {
            // Nothing before the bound decoded from this keyframe: seek further back.
            if (chunk.target <= 0.0 || ++chunk.attempts > MAX_REVERSE_RETRIES) {
                chunk.legEnd = true;
                return;
            }
            const double back = std::max(1.0, 2.0 * (chunk.bound - chunk.target));
            chunk.target = std::max(0.0, chunk.bound - back);
            chunk.complete = false;
            chunk.stopPts = std::numeric_limits<double>::quiet_NaN();
            if (!seekKeyframe(chunk.target)) chunk.complete = chunk.legEnd = true;
        }
    }

    // Finish the group being built and make it the one emitted. False when the leg is
    // over (building_.legEnd), the plan stalled, or a stop was requested.
    bool buildChunk() {
        while (!building_.complete && !planStalled_ && !stopDecoding_) stepBuild();
        if (!building_.complete || building_.frames.empty()) return false;

        ReverseChunk& chunk = building_;
        double end = plan_.duration;
        if (!std::isnan(chunk.stopPts)) {
            end = std::min(chunk.stopPts, plan_.duration);
        } else if (!chunk.hitEnd) {
            end = std::min(chunk.bound, plan_.duration);
        }
        ready_ = std::move(chunk.frames);
        readyBytes_ = chunk.bytes;
        readyEnd_ = std::max(end, framePts(ready_.back()));

        const double nextBound = framePts(ready_.front());
        chunk = ReverseChunk{};
        chunk.bound = nextBound;
        return true;
    }

    VideoFrame emitReverse() {
        AVFrame* frame = ready_.back();
        ready_.pop_back();
        const double pts = framePts(frame);
        readyBytes_ -= std::min(readyBytes_, frameBytes(frame));

        VideoFrame result = convertFrame(frame);
        result.due = frameDueTime(leg_, plan_.duration, pts, readyEnd_);
        readyEnd_ = pts;
        av_frame_free(&frame);
        return result;
    }

    void releaseReverse() {
        for (AVFrame*& frame : ready_) av_frame_free(&frame);
        ready_.clear();
        readyBytes_ = 0;
        for (AVFrame*& frame : building_.frames) av_frame_free(&frame);
        building_ = ReverseChunk{};
    }

    void stallPlan(const std::string& reason) {
        planStalled_ = true;
        releaseReverse();
        releaseTail();
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (reverseError_.empty()) LOG_WARNING("Ping-Pong cannot play backwards: {}", reason);
        reverseError_ = reason;
    }

    void backgroundDecodingLoop() {
        DecodeBackoff backoff;
        while (!stopDecoding_) {
            bool room = true;
            {
                std::unique_lock<std::mutex> lock(bufferMutex_);
                if (canPrebuild()) {
                    room = frameBuffer_.size() < maxBufferSize_;
                } else {
                    bufferCondition_.wait(lock, [this]() {
                        return stopDecoding_ || frameBuffer_.size() < maxBufferSize_;
                    });
                }
            }

            if (stopDecoding_) break;
            if (!room) {
                stepBuild();  // the buffer is full: decode the next reverse group meanwhile
                continue;
            }

            VideoFrame frame = planned_ ? producePlanned() : decodeNextFrame();
            if (stopDecoding_) break;  // interrupted mid-skip: not a decode failure
            if (!frame.isValid()) {
                std::chrono::milliseconds delay{10};  // at end of stream: idle until seek/stop
                if (!atEnd_ && !planStalled_) {
                    delay = backoff.onFailure();
                    if (backoff.failing()) recordFailure();
                }
                std::unique_lock<std::mutex> lock(bufferMutex_);
                bufferCondition_.wait_for(lock, delay, [this]() { return stopDecoding_.load(); });
                continue;
            }

            backoff.onSuccess();
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                lastError_.clear();
                frameBuffer_.push(std::move(frame));
            }
        }
    }

    void recordFailure() {
        std::string message = "Video decoding failed";
        if (lastReadError_ < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(lastReadError_, err, sizeof(err));
            message += std::string(": ") + err;
        }
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (lastError_.empty()) LOG_ERROR("{} (retrying with backoff)", message);
        lastError_ = std::move(message);
    }

    // FFmpeg objects
    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* skippedFrame_ = nullptr;  // last frame skipped on the way to a seek target
    AVFrame* rgbFrame_ = nullptr;
    std::vector<uint8_t> rgbBuffer_;

    // Stream indices
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;

    // State
    VideoInfo info_;
    double timeBase_ = 0.0;
    int64_t startPts_ = 0;      // stream start_time, in time_base units
    int64_t frameTicks_ = 1;    // one frame's duration, in time_base units
    double frameSeconds_ = 0.04;  // one frame's duration
    bool intraOnly_ = false;      // every frame is a keyframe (ProRes, MJPEG, ...)
    // Decode thread only (seek() runs while the thread is stopped).
    int64_t seekTargetPts_ = AV_NOPTS_VALUE;  // skip frames before this; NOPTS = no seek pending
    int64_t seekFromPts_ = 0;   // where the demuxer was last asked to seek
    bool checkFirstPacket_ = false;  // next video packet is the first after a demuxer seek
    bool checkFirstFrame_ = false;   // next decoded frame is the first after a demuxer seek
    bool keepPreroll_ = false;       // a keyframe seek: keep the frames before the target
    double landedKeyPts_ = 0.0;      // pts of the keyframe the last demuxer seek landed on
    int lastReadError_ = 0;     // last non-EOF av_read_frame error (decode thread only)
    bool isOpen_ = false;
    bool isLive_ = false;   // live capture device (no seek/duration)
    // Written by the background decode thread, read on the main thread (via the getters /
    // VideoPlayer::update). Atomic to avoid a data race / UB. Single-writer-at-a-time (seek
    // joins the decode thread first), so plain load/store ordering is sufficient.
    std::atomic<int64_t> currentFrame_{0};
    std::atomic<double> currentTime_{0.0};
    std::atomic<bool> atEnd_{false};
    std::atomic<double> observedEnd_{0.0};  // latest frame end decoded going forward
    std::atomic<bool> endSeen_{false};      // a forward pass reached the end of the stream
    std::atomic<size_t> reversePeakBytes_{0};

    // Planned playback (decode thread; startPlayback sets it up with the thread stopped)
    bool planned_ = false;
    DecodePlan plan_;
    PlaybackLeg leg_;
    Phase phase_ = Phase::Done;
    std::atomic<bool> planStalled_{false};
    int emptyLegs_ = 0;
    ReverseChunk building_;          // group being decoded
    std::vector<AVFrame*> ready_;    // group being emitted, oldest first (emitted from the back)
    size_t readyBytes_ = 0;
    double readyEnd_ = 0.0;          // where the interval of ready_.back() ends
    size_t lastFrameBytes_ = 0;
    std::vector<AVFrame*> tail_;     // Ping-Pong forward pass: frames since the last keyframe
    size_t tailBytes_ = 0;
    bool tailOverflow_ = false;      // this group did not fit; kept again from the next keyframe
    std::string reverseError_;       // guarded by bufferMutex_

    // Background decoding
    std::thread backgroundThread_;
    mutable std::mutex bufferMutex_;
    std::condition_variable bufferCondition_;
    std::queue<VideoFrame> frameBuffer_;
    std::string lastError_;     // guarded by bufferMutex_
    std::atomic<bool> stopDecoding_{false};
    size_t maxBufferSize_ = 8;
};

#else // !QUADMAPPER_HAS_FFMPEG

// Stub implementation when FFmpeg is not available
namespace quadmapper {

class VideoDecoder::Impl {
public:
    bool open(const std::filesystem::path&) { return false; }
    bool openCamera(const std::string&, int, int, double) { return false; }
    void close() {}
    bool isOpen() const { return false; }
    bool isLive() const { return false; }
    const VideoInfo& getInfo() const { return info_; }
    bool seek(double) { return false; }
    bool seekFrame(int64_t) { return false; }
    VideoFrame decodeNextFrame() { return VideoFrame{}; }
    double getCurrentTime() const { return 0.0; }
    int64_t getCurrentFrame() const { return 0; }
    bool isAtEnd() const { return true; }
    void startBackgroundDecoding(size_t) {}
    void startPlayback(const DecodePlan&, size_t) {}
    std::string checkReverse(size_t) const { return {}; }
    std::string getReverseError() const { return {}; }
    size_t getReverseCachePeakBytes() const { return 0; }
    double getObservedEnd() const { return 0.0; }
    void stopBackgroundDecoding() {}
    VideoFrame getBufferedFrame() { return VideoFrame{}; }
    bool peekBufferedDue(double&) const { return false; }
    bool hasBufferedFrames() const { return false; }
    size_t getBufferedFrameCount() const { return 0; }
    std::string getLastError() const { return {}; }
private:
    VideoInfo info_;
};

#endif // QUADMAPPER_HAS_FFMPEG

// Public interface implementation
VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;

bool VideoDecoder::open(const std::filesystem::path& path) { return impl_->open(path); }
bool VideoDecoder::openCamera(const std::string& deviceName, int width, int height, double fps) {
    return impl_->openCamera(deviceName, width, height, fps);
}
void VideoDecoder::close() { impl_->close(); }
bool VideoDecoder::isOpen() const { return impl_->isOpen(); }
bool VideoDecoder::isLive() const { return impl_->isLive(); }
const VideoInfo& VideoDecoder::getInfo() const { return impl_->getInfo(); }
bool VideoDecoder::seek(double timeSeconds) { return impl_->seek(timeSeconds); }
bool VideoDecoder::seekFrame(int64_t frameNumber) { return impl_->seekFrame(frameNumber); }
VideoFrame VideoDecoder::decodeNextFrame() { return impl_->decodeNextFrame(); }
double VideoDecoder::getCurrentTime() const { return impl_->getCurrentTime(); }
int64_t VideoDecoder::getCurrentFrame() const { return impl_->getCurrentFrame(); }
bool VideoDecoder::isAtEnd() const { return impl_->isAtEnd(); }
void VideoDecoder::startBackgroundDecoding(size_t bufferSize) { impl_->startBackgroundDecoding(bufferSize); }
void VideoDecoder::startPlayback(const DecodePlan& plan, size_t bufferSize) {
    impl_->startPlayback(plan, bufferSize);
}
std::string VideoDecoder::checkReverse(size_t cacheBytes) const { return impl_->checkReverse(cacheBytes); }
std::string VideoDecoder::getReverseError() const { return impl_->getReverseError(); }
size_t VideoDecoder::getReverseCachePeakBytes() const { return impl_->getReverseCachePeakBytes(); }
double VideoDecoder::getObservedEnd() const { return impl_->getObservedEnd(); }
void VideoDecoder::stopBackgroundDecoding() { impl_->stopBackgroundDecoding(); }
VideoFrame VideoDecoder::getBufferedFrame() { return impl_->getBufferedFrame(); }
bool VideoDecoder::peekBufferedDue(double& due) const { return impl_->peekBufferedDue(due); }
bool VideoDecoder::hasBufferedFrames() const { return impl_->hasBufferedFrames(); }
size_t VideoDecoder::getBufferedFrameCount() const { return impl_->getBufferedFrameCount(); }
std::string VideoDecoder::getLastError() const { return impl_->getLastError(); }

}  // namespace quadmapper
