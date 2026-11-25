#include "Mp4Writer.hpp"

#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/MediaLimits.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

#ifdef QUADMAPPER_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace fs = std::filesystem;

namespace quadmapper {

#ifdef QUADMAPPER_HAS_FFMPEG

namespace {

constexpr double AUTO_BITS_PER_PIXEL = 0.12;
constexpr int64_t MIN_AUTO_BIT_RATE = 2'000'000;
constexpr int64_t AAC_BIT_RATE_PER_CHANNEL = 96'000;
// An encoder that keeps answering "try again" (an asynchronous hardware MFT still
// busy) is given this long before the write counts as failed.
constexpr auto ENCODER_STALL_LIMIT = std::chrono::seconds(10);

std::string ffmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, text, sizeof(text));
    return text;
}

// One attempt at a video encoder: the codec and whether to ask for hardware.
struct EncoderAttempt {
    const AVCodec* codec = nullptr;
    bool hardware = false;
};

std::vector<EncoderAttempt> encoderAttempts() {
    std::vector<EncoderAttempt> attempts;
    if (const AVCodec* mf = avcodec_find_encoder_by_name("h264_mf")) {
        attempts.push_back({mf, true});
        attempts.push_back({mf, false});
    }
    if (const AVCodec* vt = avcodec_find_encoder_by_name("h264_videotoolbox")) attempts.push_back({vt, true});
    if (const AVCodec* openh264 = avcodec_find_encoder_by_name("libopenh264")) attempts.push_back({openh264, false});
    if (const AVCodec* mpeg4 = avcodec_find_encoder(AV_CODEC_ID_MPEG4)) attempts.push_back({mpeg4, false});
    return attempts;
}

// The first pixel format the encoder takes from system memory: VideoToolbox lists its
// GPU surface format first, and the frames here come from sws_scale.
AVPixelFormat firstPixelFormat(const AVCodec* codec) {
    const void* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats, &count) >= 0 &&
        formats) {
        for (int i = 0; i < count; ++i) {
            const AVPixelFormat format = static_cast<const AVPixelFormat*>(formats)[i];
            const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(format);
            if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return format;
        }
    }
    return AV_PIX_FMT_YUV420P;
}

}  // namespace

class Mp4Writer::Impl {
public:
    ~Impl() { abort(); }

    bool open(const fs::path& path, const Mp4WriterOptions& options, std::string& error) {
        abort();
        error_.clear();
        failed_ = false;
        if (!openStreams(path, options, error)) {
            abort();
            return false;
        }
        return true;
    }

    bool writeVideoFrame(const uint8_t* rgba, int stride, bool bottomUp) {
        if (!usable()) return false;
        if (!rgba || stride < width_ * 4) return fail("A video frame had no pixels or too short a row.");
        const int rc = av_frame_make_writable(frame_);
        if (rc < 0) return fail("Out of memory (" + ffmpegError(rc) + ").");
        const uint8_t* source = rgba;
        int sourceStride = stride;
        if (bottomUp) {
            // Start at the picture's top row (stored last) and walk up through memory.
            source = rgba + static_cast<ptrdiff_t>(sourceHeight_ - 1) * stride;
            sourceStride = -stride;
        }
        if (sws_scale(sws_, &source, &sourceStride, 0, height_, frame_->data, frame_->linesize) <= 0) {
            return fail("A video frame could not be converted.");
        }
        frame_->pts = frames_;
        if (!encode(video_, videoStream_, frame_)) return false;
        ++frames_;
        return true;
    }

    bool writeAudio(const float* interleaved, int frames) {
        if (!usable()) return false;
        if (!audio_) return fail("Sound was written to a file opened without a sound track.");
        if (frames <= 0) return true;
        if (!interleaved) return fail("Sound was written without samples.");
        const int channels = audio_->ch_layout.nb_channels;
        for (int ch = 0; ch < channels; ++ch) {
            planes_[ch].resize(static_cast<size_t>(frames));
            for (int i = 0; i < frames; ++i) planes_[ch][i] = interleaved[static_cast<size_t>(i) * channels + ch];
        }
        return queueAudio(frames);
    }

    bool finish(fs::path& written, std::string& error) {
        if (!fmt_) {
            error = failed_ ? error_ : "The video file is not open.";
            return false;
        }
        if (failed_) {
            error = error_;
            abort();
            return false;
        }
        bool ok = encode(video_, videoStream_, nullptr);
        if (ok && audio_) {
            // Pad the sound to the picture's length, so the track is never empty and
            // players do not stop early.
            const int64_t wanted = av_rescale(frames_, audio_->sample_rate, fps_);
            if (audioSamplesIn_ < wanted) {
                const int silence = static_cast<int>(wanted - audioSamplesIn_);
                for (auto& plane : planes_) plane.assign(static_cast<size_t>(silence), 0.0f);
                ok = queueAudio(silence);
            }
            ok = ok && encodeAudio(true) && encode(audio_, audioStream_, nullptr);
        }
        if (ok) {
            const int rc = av_write_trailer(fmt_);
            if (rc < 0) ok = fail("The MP4 file could not be completed (" + ffmpegError(rc) + ").");
        }
        if (!ok) {
            error = error_;
            abort();
            return false;
        }
        release();  // closes the file so it can be renamed
        std::error_code ec;
        fs::rename(partPath_, finalPath_, ec);
        if (ec) {
            error = "The finished video could not be renamed to " + utf8FromPath(finalPath_.filename()) + " (" +
                    ec.message() + ").";
            abort();
            return false;
        }
        LOG_INFO("Export video: wrote '{}' ({} frames, {})", utf8FromPath(finalPath_), frames_, encoderName_);
        partPath_.clear();
        written = finalPath_;
        return true;
    }

    void abort() {
        release();
        if (!partPath_.empty()) {
            std::error_code ec;
            fs::remove(partPath_, ec);
            partPath_.clear();
        }
    }

    bool isOpen() const { return fmt_ != nullptr; }

    std::string encoderName_;
    std::string error_;
    int width_ = 0;
    int height_ = 0;
    int64_t frames_ = 0;

private:
    bool usable() const { return fmt_ && !failed_; }

    bool fail(const std::string& message) {
        if (!failed_) {
            failed_ = true;
            error_ = message;
            LOG_ERROR("Export video: {}", message);
        }
        return false;
    }

    bool openStreams(const fs::path& path, const Mp4WriterOptions& options, std::string& error) {
        sourceHeight_ = options.height;
        width_ = options.width & ~1;
        height_ = options.height & ~1;
        if (width_ < 2 || height_ < 2) {
            error = "The video size must be at least 2 x 2 pixels.";
            return false;
        }
        if (const std::string tooLarge = pictureSizeProblem(width_, height_); !tooLarge.empty()) {
            error = "The video size, " + tooLarge + ".";
            return false;
        }
        if (options.fps <= 0) {
            error = "The frame rate must be positive.";
            return false;
        }
        if (options.audio && (options.sampleRate <= 0 || options.channels < 1 || options.channels > 2)) {
            error = "The sound must be mono or stereo at a positive sample rate.";
            return false;
        }
        fps_ = options.fps;
        finalPath_ = path;
        partPath_ = path;
        partPath_ += ".part";
        const std::string url = utf8FromPath(partPath_);

        if (avformat_alloc_output_context2(&fmt_, nullptr, "mp4", url.c_str()) < 0 || !fmt_) {
            error = "The MP4 writer is not available.";
            return false;
        }
        int64_t bitRate = options.videoBitRate;
        if (bitRate <= 0) {
            bitRate = std::max<int64_t>(
                MIN_AUTO_BIT_RATE,
                static_cast<int64_t>(AUTO_BITS_PER_PIXEL * width_ * height_ * static_cast<double>(fps_)));
        }
        if (!openVideoEncoder(bitRate, error)) return false;
        if (options.audio && !openAudioEncoder(options, error)) return false;

        int rc = avio_open(&fmt_->pb, url.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            error = "The video file could not be created (" + ffmpegError(rc) + "). Is the folder writable?";
            return false;
        }
        AVDictionary* muxerOptions = nullptr;
        av_dict_set(&muxerOptions, "movflags", "+faststart+write_colr", 0);
        rc = avformat_write_header(fmt_, &muxerOptions);
        av_dict_free(&muxerOptions);
        if (rc < 0) {
            error = "The MP4 file could not be started (" + ffmpegError(rc) + ").";
            return false;
        }
        LOG_INFO("Export video: {} encoder, {}x{} at {} fps, {} kb/s{}", encoderLabel_, width_, height_, fps_,
                 bitRate / 1000, audio_ ? ", AAC sound" : "");
        return true;
    }

    bool openVideoEncoder(int64_t bitRate, std::string& error) {
        // BT.709 for HD and up, BT.601 below: the rule players guess by when untagged.
        const bool hd = width_ >= 1280 || height_ >= 720;
        std::string tried;
        for (const EncoderAttempt& attempt : encoderAttempts()) {
            AVCodecContext* ctx = avcodec_alloc_context3(attempt.codec);
            if (!ctx) {
                error = "Out of memory.";
                return false;
            }
            const AVPixelFormat pixelFormat = firstPixelFormat(attempt.codec);
            ctx->width = width_;
            ctx->height = height_;
            ctx->pix_fmt = pixelFormat;
            ctx->time_base = AVRational{1, fps_};
            ctx->framerate = AVRational{fps_, 1};
            ctx->gop_size = fps_;  // a keyframe a second, so players seek quickly
            ctx->bit_rate = bitRate;
            ctx->color_range = AVCOL_RANGE_MPEG;
            ctx->colorspace = hd ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
            ctx->color_primaries = hd ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
            ctx->color_trc = hd ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
            if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (std::string(attempt.codec->name) == "h264_mf") {
                // Media Foundation ignores CRF; it encodes to bit_rate in this mode.
                if (av_opt_set_int(ctx->priv_data, "hw_encoding", attempt.hardware ? 1 : 0, 0) < 0) {
                    LOG_WARNING("Export video: h264_mf rejected hw_encoding");
                }
                if (av_opt_set(ctx->priv_data, "rate_control", "u_vbr", 0) < 0) {
                    LOG_WARNING("Export video: h264_mf rejected rate_control u_vbr");
                }
            }
            std::string label = attempt.codec->name;
            if (label == "h264_mf") label += attempt.hardware ? " (hardware)" : " (software)";
            const int rc = avcodec_open2(ctx, attempt.codec, nullptr);
            if (rc < 0) {
                LOG_WARNING("Export video: {} could not be opened ({})", label, ffmpegError(rc));
                tried += (tried.empty() ? "" : ", ") + label;
                avcodec_free_context(&ctx);
                continue;
            }
            video_ = ctx;
            encoderName_ = attempt.codec->name;
            encoderLabel_ = label;
            break;
        }
        if (!video_) {
            error = tried.empty() ? "No video encoder is available."
                                  : "No video encoder could be opened (tried " + tried + ").";
            return false;
        }

        videoStream_ = avformat_new_stream(fmt_, nullptr);
        if (!videoStream_ || avcodec_parameters_from_context(videoStream_->codecpar, video_) < 0) {
            error = "Out of memory.";
            return false;
        }
        videoStream_->time_base = video_->time_base;
        videoStream_->avg_frame_rate = video_->framerate;

        sws_ = sws_getContext(width_, height_, AV_PIX_FMT_RGBA, width_, height_, video_->pix_fmt, SWS_BILINEAR,
                              nullptr, nullptr, nullptr);
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!sws_ || !frame_ || !packet_) {
            error = "The picture converter could not be set up.";
            return false;
        }
        // RGB in at full range; YUV out at limited range in the tagged matrix.
        sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                 sws_getCoefficients(hd ? SWS_CS_ITU709 : SWS_CS_ITU601), 0, 0, 1 << 16,
                                 1 << 16);
        frame_->format = video_->pix_fmt;
        frame_->width = width_;
        frame_->height = height_;
        if (av_frame_get_buffer(frame_, 0) < 0) {
            error = "Out of memory.";
            return false;
        }
        return true;
    }

    bool openAudioEncoder(const Mp4WriterOptions& options, std::string& error) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec) {
            error = "No AAC sound encoder is available.";
            return false;
        }
        audio_ = avcodec_alloc_context3(codec);
        if (!audio_) {
            error = "Out of memory.";
            return false;
        }
        audio_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio_->sample_rate = options.sampleRate;
        av_channel_layout_default(&audio_->ch_layout, options.channels);
        audio_->bit_rate = AAC_BIT_RATE_PER_CHANNEL * options.channels;
        audio_->time_base = AVRational{1, options.sampleRate};
        if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) audio_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        int rc = avcodec_open2(audio_, codec, nullptr);
        if (rc < 0) {
            error = "The AAC sound encoder could not be opened (" + ffmpegError(rc) + ").";
            return false;
        }
        audioStream_ = avformat_new_stream(fmt_, nullptr);
        if (!audioStream_ || avcodec_parameters_from_context(audioStream_->codecpar, audio_) < 0) {
            error = "Out of memory.";
            return false;
        }
        audioStream_->time_base = audio_->time_base;
        fifo_ = av_audio_fifo_alloc(audio_->sample_fmt, options.channels, std::max(1, audio_->frame_size));
        audioFrame_ = av_frame_alloc();
        if (!fifo_ || !audioFrame_) {
            error = "Out of memory.";
            return false;
        }
        planes_.assign(static_cast<size_t>(options.channels), {});
        return true;
    }

    bool queueAudio(int frames) {
        void* data[2] = {nullptr, nullptr};
        for (size_t ch = 0; ch < planes_.size(); ++ch) data[ch] = planes_[ch].data();
        if (av_audio_fifo_write(fifo_, data, frames) < frames) return fail("Out of memory.");
        audioSamplesIn_ += frames;
        return encodeAudio(false);
    }

    // Encode whole encoder frames from the FIFO; with `flush`, the remainder too.
    bool encodeAudio(bool flush) {
        const int frameSize = audio_->frame_size > 0 ? audio_->frame_size : 1024;
        const bool shortAllowed = (audio_->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
        while (av_audio_fifo_size(fifo_) >= frameSize || (flush && av_audio_fifo_size(fifo_) > 0)) {
            const int available = std::min(frameSize, av_audio_fifo_size(fifo_));
            av_frame_unref(audioFrame_);
            audioFrame_->nb_samples = shortAllowed ? available : frameSize;
            audioFrame_->format = audio_->sample_fmt;
            audioFrame_->sample_rate = audio_->sample_rate;
            if (av_channel_layout_copy(&audioFrame_->ch_layout, &audio_->ch_layout) < 0 ||
                av_frame_get_buffer(audioFrame_, 0) < 0 ||
                av_audio_fifo_read(fifo_, reinterpret_cast<void**>(audioFrame_->data), available) != available) {
                return fail("Out of memory.");
            }
            if (available < audioFrame_->nb_samples) {
                av_samples_set_silence(audioFrame_->data, available, audioFrame_->nb_samples - available,
                                       audioFrame_->ch_layout.nb_channels, audio_->sample_fmt);
            }
            audioFrame_->pts = audioPts_;
            audioPts_ += audioFrame_->nb_samples;
            if (!encode(audio_, audioStream_, audioFrame_)) return false;
        }
        return true;
    }

    // Send one frame (null: end of stream) and write every packet that comes out.
    bool encode(AVCodecContext* encoder, AVStream* stream, const AVFrame* frame) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const int sent = avcodec_send_frame(encoder, frame);
            if (sent == AVERROR_EOF && !frame) return true;  // already flushed
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                return fail(std::string("The ") + encoder->codec->name + " encoder failed (" + ffmpegError(sent) +
                            ").");
            }
            const bool more = sent == AVERROR(EAGAIN);  // drain first, then send again
            for (;;) {
                const int rc = avcodec_receive_packet(encoder, packet_);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) {
                    return fail(std::string("The ") + encoder->codec->name + " encoder failed (" +
                                ffmpegError(rc) + ").");
                }
                av_packet_rescale_ts(packet_, encoder->time_base, stream->time_base);
                packet_->stream_index = stream->index;
                const int written = av_interleaved_write_frame(fmt_, packet_);
                if (written < 0) return fail("Writing the video file failed (" + ffmpegError(written) + ").");
            }
            if (!more) return true;
            if (std::chrono::steady_clock::now() - start > ENCODER_STALL_LIMIT) {
                return fail(std::string("The ") + encoder->codec->name + " encoder stopped responding.");
            }
        }
    }

    void release() {
        if (fmt_) {
            if (fmt_->pb) avio_closep(&fmt_->pb);
            avformat_free_context(fmt_);
            fmt_ = nullptr;
        }
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
        sws_freeContext(sws_);
        sws_ = nullptr;
        if (fifo_) {
            av_audio_fifo_free(fifo_);
            fifo_ = nullptr;
        }
        av_frame_free(&frame_);
        av_frame_free(&audioFrame_);
        av_packet_free(&packet_);
        videoStream_ = audioStream_ = nullptr;
        audioPts_ = 0;
        audioSamplesIn_ = 0;
    }

    std::string encoderLabel_;  // the name, plus hardware or software for h264_mf
    fs::path finalPath_;
    fs::path partPath_;
    bool failed_ = false;
    int fps_ = 30;
    int sourceHeight_ = 0;

    AVFormatContext* fmt_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVStream* videoStream_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* frame_ = nullptr;

    AVCodecContext* audio_ = nullptr;
    AVStream* audioStream_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVFrame* audioFrame_ = nullptr;
    std::vector<std::vector<float>> planes_;
    int64_t audioPts_ = 0;        // in samples
    int64_t audioSamplesIn_ = 0;  // sample frames handed in (and padded)
};

#else  // !QUADMAPPER_HAS_FFMPEG

class Mp4Writer::Impl {
public:
    bool open(const fs::path&, const Mp4WriterOptions&, std::string& error) {
        error = "This build has no FFmpeg, so it cannot write video.";
        return false;
    }
    bool writeVideoFrame(const uint8_t*, int, bool) { return false; }
    bool writeAudio(const float*, int) { return false; }
    bool finish(fs::path&, std::string& error) {
        error = "This build has no FFmpeg, so it cannot write video.";
        return false;
    }
    void abort() {}
    bool isOpen() const { return false; }

    std::string encoderName_;
    std::string error_;
    int width_ = 0;
    int height_ = 0;
    int64_t frames_ = 0;
};

#endif  // QUADMAPPER_HAS_FFMPEG

// --- Mp4Writer ---

Mp4Writer::Mp4Writer() : impl_(std::make_unique<Impl>()) {}
Mp4Writer::~Mp4Writer() = default;

bool Mp4Writer::open(const fs::path& path, const Mp4WriterOptions& options, std::string& error) {
    return impl_->open(path, options, error);
}
bool Mp4Writer::writeVideoFrame(const uint8_t* rgba, int stride, bool bottomUp) {
    return impl_->writeVideoFrame(rgba, stride, bottomUp);
}
bool Mp4Writer::writeAudio(const float* interleaved, int frames) { return impl_->writeAudio(interleaved, frames); }
bool Mp4Writer::finish(fs::path& written, std::string& error) { return impl_->finish(written, error); }
void Mp4Writer::abort() { impl_->abort(); }
bool Mp4Writer::isOpen() const { return impl_->isOpen(); }
const std::string& Mp4Writer::encoderName() const { return impl_->encoderName_; }
const std::string& Mp4Writer::lastError() const { return impl_->error_; }
int64_t Mp4Writer::framesWritten() const { return impl_->frames_; }

// --- AsyncMp4Writer ---

AsyncMp4Writer::AsyncMp4Writer(size_t queueFrames) : queueFrames_(std::max<size_t>(1, queueFrames)) {}

AsyncMp4Writer::~AsyncMp4Writer() { abort(); }

bool AsyncMp4Writer::open(const fs::path& path, const Mp4WriterOptions& options, std::string& error) {
    abort();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        queuedFrames_ = queuedSamples_ = 0;
        opened_ = openOk_ = failed_ = false;
        command_ = Command::None;
        error_.clear();
        written_.clear();
    }
    width_ = options.width;
    height_ = options.height;
    channels_ = options.audio ? options.channels : 0;
    audioCapacity_ = static_cast<size_t>(std::max(1, options.sampleRate));  // about a second
    worker_ = std::thread(&AsyncMp4Writer::run, this, path, options);

    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return opened_; });
    if (openOk_) return true;
    error = error_;
    lock.unlock();
    worker_.join();
    return false;
}

bool AsyncMp4Writer::writeVideoFrame(const uint8_t* rgba, int stride, bool bottomUp) {
    const size_t rowBytes = static_cast<size_t>(std::max(0, width_)) * 4;
    if (!rgba || stride < static_cast<int>(rowBytes)) return false;
    Item item;
    item.video = true;
    item.bottomUp = bottomUp;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_ || !openOk_) return false;
        if (!spare_.empty()) {
            item.pixels = std::move(spare_.back());
            spare_.pop_back();
        }
    }
    item.pixels.resize(rowBytes * static_cast<size_t>(height_));
    for (int row = 0; row < height_; ++row) {
        std::copy_n(rgba + static_cast<ptrdiff_t>(row) * stride, rowBytes, item.pixels.data() + row * rowBytes);
    }
    return push(std::move(item));
}

bool AsyncMp4Writer::writeAudio(const float* interleaved, int frames) {
    if (frames <= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        return openOk_ && !failed_;
    }
    if (!interleaved || channels_ <= 0) return false;
    Item item;
    item.samples.assign(interleaved, interleaved + static_cast<size_t>(frames) * channels_);
    return push(std::move(item));
}

bool AsyncMp4Writer::push(Item item) {
    std::unique_lock<std::mutex> lock(mutex_);
    const size_t samples = item.video ? 0 : item.samples.size() / static_cast<size_t>(channels_);
    changed_.wait(lock, [&] {
        if (failed_ || command_ != Command::None) return true;
        if (item.video) return queuedFrames_ < queueFrames_;
        return queuedSamples_ == 0 || queuedSamples_ + samples <= audioCapacity_;
    });
    if (failed_ || !openOk_ || command_ != Command::None) return false;
    if (item.video) {
        ++queuedFrames_;
    } else {
        queuedSamples_ += samples;
    }
    queue_.push_back(std::move(item));
    changed_.notify_all();
    return true;
}

bool AsyncMp4Writer::finish(fs::path& written, std::string& error) {
    if (!worker_.joinable()) {
        std::lock_guard<std::mutex> lock(mutex_);
        error = error_.empty() ? "The video file is not open." : error_;
        return false;
    }
    stop(Command::Finish);
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) {
        error = error_;
        return false;
    }
    written = written_;
    return true;
}

void AsyncMp4Writer::abort() {
    if (worker_.joinable()) stop(Command::Abort);
}

std::string AsyncMp4Writer::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void AsyncMp4Writer::stop(Command command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_ = command;
        if (command == Command::Abort) {
            queue_.clear();
            queuedFrames_ = queuedSamples_ = 0;
        }
    }
    changed_.notify_all();
    worker_.join();
}

void AsyncMp4Writer::fail(const std::string& message) {
    if (!failed_) {
        failed_ = true;
        error_ = message;
    }
    queue_.clear();
    queuedFrames_ = queuedSamples_ = 0;
    changed_.notify_all();
}

void AsyncMp4Writer::run(fs::path path, Mp4WriterOptions options) {
    Mp4Writer writer;
    std::string openError;
    const bool ok = writer.open(path, options, openError);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoderName_ = writer.encoderName();
        opened_ = true;
        openOk_ = ok;
        if (!ok) {
            failed_ = true;
            error_ = openError;
        }
    }
    changed_.notify_all();
    if (!ok) return;

    const int stride = width_ * 4;
    for (;;) {
        Item item;
        Command command = Command::None;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return !queue_.empty() || command_ != Command::None; });
            if (queue_.empty() || command_ == Command::Abort) {
                command = command_;
            } else {
                item = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (command == Command::Abort) {
            writer.abort();
            return;
        }
        if (command == Command::Finish) {
            std::string finishError;
            fs::path written;
            bool finished = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                finished = !failed_;
            }
            if (finished) finished = writer.finish(written, finishError);
            writer.abort();  // no-op after a successful finish
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished) {
                written_ = written;
            } else if (!failed_) {
                fail(finishError);
            }
            return;
        }

        const bool wrote = item.video ? writer.writeVideoFrame(item.pixels.data(), stride, item.bottomUp)
                                      : writer.writeAudio(item.samples.data(),
                                                          static_cast<int>(item.samples.size() / channels_));
        std::lock_guard<std::mutex> lock(mutex_);
        if (item.video) {
            if (queuedFrames_ > 0) --queuedFrames_;
            if (spare_.size() < queueFrames_) spare_.push_back(std::move(item.pixels));
        } else {
            const size_t samples = item.samples.size() / static_cast<size_t>(channels_);
            queuedSamples_ -= std::min(queuedSamples_, samples);
        }
        if (!wrote) {
            fail(writer.lastError());
            writer.abort();
        }
        changed_.notify_all();
    }
}

}  // namespace quadmapper
