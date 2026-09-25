#include "VideoDecoder.hpp"
#include "DecodeBackoff.hpp"
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
#include <string>

namespace quadmapper {

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

        // Calculate duration
        if (formatCtx_->duration != AV_NOPTS_VALUE) {
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
        seekTargetPts_ = AV_NOPTS_VALUE;
        checkFirstPacket_ = false;
        checkFirstFrame_ = false;
        isOpen_ = true;
        currentFrame_ = 0;
        currentTime_ = 0.0;
        atEnd_ = false;

        LOG_INFO("Opened video: {}x{}, {} fps, {:.2f}s", info_.width, info_.height, info_.frameRate, info_.duration);
        return true;
    }

    void close() {
        stopBackgroundDecoding();

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
        currentFrame_ = 0;
        currentTime_ = 0.0;
        atEnd_ = false;
    }

    bool isOpen() const { return isOpen_; }
    bool isLive() const { return isLive_; }
    const VideoInfo& getInfo() const { return info_; }
    double getCurrentTime() const { return currentTime_; }
    int64_t getCurrentFrame() const { return currentFrame_; }
    bool isAtEnd() const { return atEnd_; }

    bool seek(double timeSeconds) {
        if (!isOpen_) return false;

        const int64_t target = startPts_ + std::llround(timeSeconds / timeBase_);
        if (!seekDemuxer(target)) {
            LOG_WARNING("Seek failed");
            return false;
        }

        seekTargetPts_ = target;
        atEnd_ = false;
        currentTime_ = timeSeconds;
        currentFrame_ = static_cast<int64_t>(timeSeconds * info_.frameRate);

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
        if (!isOpen_) return VideoFrame{};

        AVPacket* packet = av_packet_alloc();
        if (!packet) return VideoFrame{};

        VideoFrame result;
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

                // Convert to RGB
                sws_scale(swsCtx_, frame_->data, frame_->linesize, 0,
                         codecCtx_->height, rgbFrame_->data, rgbFrame_->linesize);

                // Create output frame
                result.width = info_.width;
                result.height = info_.height;
                result.frameNumber = currentFrame_++;

                // Calculate PTS
                if (frame_->pts != AV_NOPTS_VALUE) {
                    result.pts = static_cast<double>(frame_->pts - startPts_) * timeBase_;
                    currentTime_ = result.pts;
                } else {
                    result.pts = currentTime_;
                    // std::atomic<double> has no operator+= in C++17; single writer
                    // (this thread), so load+store is correct.
                    currentTime_.store(currentTime_.load() + 1.0 / info_.frameRate);
                }

                // Copy pixel data
                int rowBytes = info_.width * 3;
                result.data.resize(rowBytes * info_.height);
                for (int y = 0; y < info_.height; y++) {
                    memcpy(result.data.data() + y * rowBytes,
                           rgbFrame_->data[0] + y * rgbFrame_->linesize[0],
                           rowBytes);
                }

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
                checkFirstPacket_ = false;
                if (!landedOffKey || !retrySeekEarlier()) {
                    avcodec_send_packet(codecCtx_, packet);  // a corrupt packet is skipped
                }
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        return result;
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

    bool peekBufferedPts(double& pts) const {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (frameBuffer_.empty()) return false;
        pts = frameBuffer_.front().pts;
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
    // are skipped, so the seek is frame-exact.
    bool reachedSeekTarget() {
        if (seekTargetPts_ == AV_NOPTS_VALUE || frame_->pts == AV_NOPTS_VALUE) return true;

        if (checkFirstFrame_) {
            checkFirstFrame_ = false;
            if (frame_->pts > seekTargetPts_ && retrySeekEarlier()) return false;
        }

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

    void backgroundDecodingLoop() {
        DecodeBackoff backoff;
        while (!stopDecoding_) {
            {
                std::unique_lock<std::mutex> lock(bufferMutex_);
                bufferCondition_.wait(lock, [this]() {
                    return stopDecoding_ || frameBuffer_.size() < maxBufferSize_;
                });
            }

            if (stopDecoding_) break;

            VideoFrame frame = decodeNextFrame();
            if (stopDecoding_) break;  // interrupted mid-skip: not a decode failure
            if (!frame.isValid()) {
                std::chrono::milliseconds delay{10};  // at end of stream: idle until seek/stop
                if (!atEnd_) {
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
    // Decode thread only (seek() runs while the thread is stopped).
    int64_t seekTargetPts_ = AV_NOPTS_VALUE;  // skip frames before this; NOPTS = no seek pending
    int64_t seekFromPts_ = 0;   // where the demuxer was last asked to seek
    bool checkFirstPacket_ = false;  // next video packet is the first after a demuxer seek
    bool checkFirstFrame_ = false;   // next decoded frame is the first after a demuxer seek
    int lastReadError_ = 0;     // last non-EOF av_read_frame error (decode thread only)
    bool isOpen_ = false;
    bool isLive_ = false;   // live capture device (no seek/duration)
    // Written by the background decode thread, read on the main thread (via the getters /
    // VideoPlayer::update). Atomic to avoid a data race / UB. Single-writer-at-a-time (seek
    // joins the decode thread first), so plain load/store ordering is sufficient.
    std::atomic<int64_t> currentFrame_{0};
    std::atomic<double> currentTime_{0.0};
    std::atomic<bool> atEnd_{false};

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
    void stopBackgroundDecoding() {}
    VideoFrame getBufferedFrame() { return VideoFrame{}; }
    bool peekBufferedPts(double&) const { return false; }
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
void VideoDecoder::stopBackgroundDecoding() { impl_->stopBackgroundDecoding(); }
VideoFrame VideoDecoder::getBufferedFrame() { return impl_->getBufferedFrame(); }
bool VideoDecoder::peekBufferedPts(double& pts) const { return impl_->peekBufferedPts(pts); }
bool VideoDecoder::hasBufferedFrames() const { return impl_->hasBufferedFrames(); }
size_t VideoDecoder::getBufferedFrameCount() const { return impl_->getBufferedFrameCount(); }
std::string VideoDecoder::getLastError() const { return impl_->getLastError(); }

} // namespace quadmapper
