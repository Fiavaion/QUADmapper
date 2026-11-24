#include "AudioDecoder.hpp"
#include "AudioFrame.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#ifdef QUADMAPPER_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace quadmapper {

class AudioDecoder::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    bool open(const std::filesystem::path& path, int outSampleRate, int outChannels) {
        close();
        const std::string pathStr = path.u8string();

        // Local protocols only, as video/LocalMediaInput.hpp (audio does not depend on
        // video): a crafted file cannot make FFmpeg fetch from the network.
        AVDictionary* options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file,crypto,data", 0);
        formatCtx_ = avformat_alloc_context();  // freed by avformat_open_input() if it fails
        if (!formatCtx_) {
            av_dict_free(&options);
            return false;
        }
        formatCtx_->interrupt_callback = {&Impl::interruptRequested, &interrupted_};
        const int opened = avformat_open_input(&formatCtx_, pathStr.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (opened < 0) {
            LOG_WARNING("Audio: cannot open {}", pathStr);
            return false;
        }
        if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
            LOG_WARNING("Audio: no stream info in {}", pathStr);
            close();
            return false;
        }

        int videoIndex = -1;
        for (unsigned int i = 0; i < formatCtx_->nb_streams; ++i) {
            const AVMediaType type = formatCtx_->streams[i]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_AUDIO && audioStreamIndex_ < 0) audioStreamIndex_ = static_cast<int>(i);
            if (type == AVMEDIA_TYPE_VIDEO && videoIndex < 0) videoIndex = static_cast<int>(i);
        }
        if (audioStreamIndex_ < 0) {
            LOG_DEBUG("Audio: no audio stream in {}", pathStr);
            close();
            return false;
        }

        // Only the audio packets are wanted; the demuxer skips the rest (the video is
        // decoded by its own demuxer).
        for (unsigned int i = 0; i < formatCtx_->nb_streams; ++i) {
            if (static_cast<int>(i) != audioStreamIndex_) formatCtx_->streams[i]->discard = AVDISCARD_ALL;
        }

        AVStream* stream = formatCtx_->streams[audioStreamIndex_];
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            LOG_WARNING("Audio: the stream in {} has no valid time base", pathStr);
            close();
            return false;
        }
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            LOG_WARNING("Audio: unsupported codec in {}", pathStr);
            close();
            return false;
        }
        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_ || avcodec_parameters_to_context(codecCtx_, stream->codecpar) < 0 ||
            avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            LOG_WARNING("Audio: cannot open the {} decoder for {}", codec->name, pathStr);
            close();
            return false;
        }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!frame_ || !packet_) {
            close();
            return false;
        }

        info_.channels = codecCtx_->ch_layout.nb_channels;
        info_.sampleRate = codecCtx_->sample_rate;
        info_.codecName = codec->name;
        info_.bitRate = static_cast<int>(codecCtx_->bit_rate);
        if (formatCtx_->duration != AV_NOPTS_VALUE) {
            info_.duration = static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
        } else if (stream->duration != AV_NOPTS_VALUE) {
            info_.duration = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        }
        // A header can claim any length: keep it where the sample count below fits.
        info_.duration = std::clamp(info_.duration, 0.0, MAX_DURATION_SECONDS);
        if (info_.duration > 0 && info_.sampleRate > 0) {
            info_.totalSamples = static_cast<int64_t>(info_.duration * info_.sampleRate);
        }
        if (!info_.isValid()) {
            LOG_WARNING("Audio: stream in {} has no channels or rate", pathStr);
            close();
            return false;
        }

        outSampleRate_ = outSampleRate > 0 ? outSampleRate : info_.sampleRate;
        outChannels_ = (outChannels == 1 || outChannels == 2) ? outChannels : info_.channels;
        if (!setupResampler(codecCtx_->ch_layout, codecCtx_->sample_fmt, codecCtx_->sample_rate)) {
            close();
            return false;
        }

        timeBase_ = av_q2d(stream->time_base);
        // Clip time 0 is where the video starts, as VideoDecoder and HapFileSource count it.
        const AVStream* zeroStream = videoIndex >= 0 ? formatCtx_->streams[videoIndex] : stream;
        zeroSeconds_ = zeroStream->start_time != AV_NOPTS_VALUE
            ? static_cast<double>(zeroStream->start_time) * av_q2d(zeroStream->time_base)
            : 0.0;
        isOpen_ = true;
        currentTime_ = 0.0;
        demuxEnded_ = false;
        atEnd_ = false;

        LOG_INFO("Audio: {} ch {} Hz {} ({} s) -> {} ch {} Hz", info_.channels, info_.sampleRate,
                 info_.codecName, info_.duration, outChannels_, outSampleRate_);
        return true;
    }

    void close() {
        if (swrCtx_) swr_free(&swrCtx_);
        av_channel_layout_uninit(&inLayout_);
        if (frame_) av_frame_free(&frame_);
        if (packet_) av_packet_free(&packet_);
        if (codecCtx_) avcodec_free_context(&codecCtx_);
        if (formatCtx_) avformat_close_input(&formatCtx_);
        info_ = AudioInfo{};
        audioStreamIndex_ = -1;
        isOpen_ = false;
        currentTime_ = 0.0;
        demuxEnded_ = false;
        atEnd_ = false;
    }

    void interrupt() { interrupted_ = true; }

    bool isOpen() const { return isOpen_; }
    const AudioInfo& getInfo() const { return info_; }
    int getOutputSampleRate() const { return isOpen_ ? outSampleRate_ : 0; }
    int getOutputChannels() const { return isOpen_ ? outChannels_ : 0; }
    double getCurrentTime() const { return currentTime_; }
    bool isAtEnd() const { return !isOpen_ || atEnd_; }

    bool seek(double timeSeconds) {
        if (!isOpen_) return false;
        // Land a little early: a transform codec (AAC, MP3) decodes the first packet
        // after a flush without its overlap with the one before, so that packet's sound
        // is wrong. Starting SEEK_PREROLL_SECONDS earlier leaves it in the part the
        // caller trims (for 0 this reaches the priming packet before the first sample).
        const int64_t target = static_cast<int64_t>(
            std::floor((timeSeconds - SEEK_PREROLL_SECONDS + zeroSeconds_) / timeBase_));
        if (av_seek_frame(formatCtx_, audioStreamIndex_, target, AVSEEK_FLAG_BACKWARD) < 0) {
            // Before the first packet (or an unseekable file): start from the top.
            if (av_seek_frame(formatCtx_, audioStreamIndex_, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                LOG_WARNING("Audio seek to {} s failed", timeSeconds);
                return false;
            }
        }
        avcodec_flush_buffers(codecCtx_);
        // Drop samples still buffered in the resampler from before the seek.
        if (swrCtx_ && swr_init(swrCtx_) < 0) return false;
        demuxEnded_ = false;
        atEnd_ = false;
        currentTime_ = timeSeconds;
        return true;
    }

    AudioFrame decodeNextFrame() {
        if (!isOpen_ || atEnd_) return AudioFrame{};
        for (int attempts = 0; attempts < MAX_READ_ATTEMPTS; ++attempts) {
            const int received = avcodec_receive_frame(codecCtx_, frame_);
            if (received >= 0) {
                AudioFrame result = convertFrame();
                av_frame_unref(frame_);
                if (result.isValid()) return result;
                continue;
            }
            if (received == AVERROR_EOF) {
                atEnd_ = true;
                return AudioFrame{};
            }
            if (received != AVERROR(EAGAIN)) return AudioFrame{};  // decode error

            // The decoder wants input.
            if (demuxEnded_) {
                atEnd_ = true;
                return AudioFrame{};
            }
            const int read = av_read_frame(formatCtx_, packet_);
            if (read < 0) {
                // End of file (or a read error): drain what the decoder still holds.
                demuxEnded_ = true;
                avcodec_send_packet(codecCtx_, nullptr);
                continue;
            }
            if (packet_->stream_index == audioStreamIndex_) {
                avcodec_send_packet(codecCtx_, packet_);  // a bad packet is skipped
            }
            av_packet_unref(packet_);
        }
        return AudioFrame{};
    }

private:
    static constexpr int MAX_READ_ATTEMPTS = 4096;
    static constexpr double SEEK_PREROLL_SECONDS = 0.1;
    static constexpr double MAX_DURATION_SECONDS = 1e7;  // 115 days

    // FFmpeg's interrupt callback: non-zero ends a read.
    static int interruptRequested(void* interrupted) {
        return static_cast<const std::atomic<bool>*>(interrupted)->load(std::memory_order_relaxed) ? 1 : 0;
    }

    // (Re)build the resampler for input in this layout, sample format and rate.
    bool setupResampler(const AVChannelLayout& layout, AVSampleFormat format, int rate) {
        if (swrCtx_) swr_free(&swrCtx_);
        if (layout.nb_channels <= 0 || rate <= 0) return false;
        AVChannelLayout outLayout;
        if (outChannels_ == layout.nb_channels) {
            av_channel_layout_copy(&outLayout, &layout);
        } else {
            av_channel_layout_default(&outLayout, outChannels_);
        }
        const int rc = swr_alloc_set_opts2(&swrCtx_, &outLayout, AV_SAMPLE_FMT_FLT, outSampleRate_,
                                           &layout, format, rate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
        if (rc < 0 || swr_init(swrCtx_) < 0 || av_channel_layout_copy(&inLayout_, &layout) < 0) {
            // Once: convertFrame() tries again for every frame.
            if (!resamplerFailed_) LOG_WARNING("Audio: cannot set up the resampler");
            resamplerFailed_ = true;
            if (swrCtx_) swr_free(&swrCtx_);
            return false;
        }
        resamplerFailed_ = false;
        inFormat_ = format;
        inRate_ = rate;
        return true;
    }

    bool resamplerFits(const AVFrame& frame) const {
        return swrCtx_ && frame.format == inFormat_ && frame.sample_rate == inRate_ &&
               av_channel_layout_compare(&frame.ch_layout, &inLayout_) == 0;
    }

    AudioFrame convertFrame() {
        AudioFrame result;
        // A stream can change its layout, sample format or rate part-way (and a damaged
        // file can claim to). The resampler reads the frame's buffers by the format it
        // was built for, so it is rebuilt for a frame that differs.
        if (!resamplerFits(*frame_) &&
            !setupResampler(frame_->ch_layout, static_cast<AVSampleFormat>(frame_->format),
                            frame_->sample_rate)) {
            return result;
        }
        const int capacity = swr_get_out_samples(swrCtx_, frame_->nb_samples);
        if (capacity <= 0) return result;
        result.samples.resize(static_cast<size_t>(capacity) * outChannels_);
        uint8_t* out = reinterpret_cast<uint8_t*>(result.samples.data());
        const int converted = swr_convert(swrCtx_, &out, capacity,
                                          const_cast<const uint8_t**>(frame_->extended_data),
                                          frame_->nb_samples);
        if (converted <= 0) return AudioFrame{};
        result.samples.resize(static_cast<size_t>(converted) * outChannels_);
        result.channels = outChannels_;
        result.sampleRate = outSampleRate_;
        result.sampleCount = converted;

        const int64_t pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE
            ? frame_->best_effort_timestamp : frame_->pts;
        if (pts != AV_NOPTS_VALUE) {
            result.pts = static_cast<double>(pts) * timeBase_ - zeroSeconds_;
        } else {
            result.pts = currentTime_;
        }
        currentTime_ = result.pts + static_cast<double>(converted) / outSampleRate_;
        return result;
    }

    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    // The input swrCtx_ was built for (setupResampler).
    AVChannelLayout inLayout_{};
    int inFormat_ = AV_SAMPLE_FMT_NONE;
    int inRate_ = 0;
    bool resamplerFailed_ = false;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;

    int audioStreamIndex_ = -1;
    AudioInfo info_;
    double timeBase_ = 0.0;
    double zeroSeconds_ = 0.0;
    int outSampleRate_ = 0;
    int outChannels_ = 0;

    bool isOpen_ = false;
    double currentTime_ = 0.0;
    bool demuxEnded_ = false;
    bool atEnd_ = false;
    std::atomic<bool> interrupted_{false};  // interrupt(): never reset
};

#else // !QUADMAPPER_HAS_FFMPEG

namespace quadmapper {

class AudioDecoder::Impl {
public:
    bool open(const std::filesystem::path&, int, int) { return false; }
    void close() {}
    void interrupt() {}
    bool isOpen() const { return false; }
    const AudioInfo& getInfo() const { return info_; }
    int getOutputSampleRate() const { return 0; }
    int getOutputChannels() const { return 0; }
    bool seek(double) { return false; }
    AudioFrame decodeNextFrame() { return AudioFrame{}; }
    double getCurrentTime() const { return 0.0; }
    bool isAtEnd() const { return true; }
private:
    AudioInfo info_;
};

#endif // QUADMAPPER_HAS_FFMPEG

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;
AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;

bool AudioDecoder::open(const std::filesystem::path& path, int outSampleRate, int outChannels) {
    return impl_->open(path, outSampleRate, outChannels);
}
void AudioDecoder::close() { impl_->close(); }
void AudioDecoder::interrupt() { impl_->interrupt(); }
bool AudioDecoder::isOpen() const { return impl_->isOpen(); }
const AudioInfo& AudioDecoder::getInfo() const { return impl_->getInfo(); }
int AudioDecoder::getOutputSampleRate() const { return impl_->getOutputSampleRate(); }
int AudioDecoder::getOutputChannels() const { return impl_->getOutputChannels(); }
bool AudioDecoder::seek(double timeSeconds) { return impl_->seek(timeSeconds); }
AudioFrame AudioDecoder::decodeNextFrame() { return impl_->decodeNextFrame(); }
double AudioDecoder::getCurrentTime() const { return impl_->getCurrentTime(); }
bool AudioDecoder::isAtEnd() const { return impl_->isAtEnd(); }

} // namespace quadmapper
