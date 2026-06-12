#include "AudioDecoder.hpp"
#include "AudioFrame.hpp"
#include "AudioBuffer.hpp"
#include "core/Logger.hpp"

#ifdef QUADMAPPER_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

namespace quadmapper {

class AudioDecoder::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    bool open(const std::filesystem::path& path) {
        close();

        std::string pathStr = path.string();

        // Open input file
        if (avformat_open_input(&formatCtx_, pathStr.c_str(), nullptr, nullptr) < 0) {
            LOG_ERROR("Failed to open audio file: {}", pathStr);
            return false;
        }

        // Find stream info
        if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
            LOG_ERROR("Failed to find stream info");
            close();
            return false;
        }

        // Find audio stream
        audioStreamIndex_ = -1;
        for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
            if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex_ = static_cast<int>(i);
                break;
            }
        }

        if (audioStreamIndex_ < 0) {
            LOG_ERROR("No audio stream found in: {}", pathStr);
            close();
            return false;
        }

        // Get codec parameters
        AVCodecParameters* codecPar = formatCtx_->streams[audioStreamIndex_]->codecpar;

        // Find decoder
        const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Unsupported audio codec");
            close();
            return false;
        }

        // Allocate codec context
        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_) {
            LOG_ERROR("Failed to allocate audio codec context");
            close();
            return false;
        }

        // Copy codec parameters
        if (avcodec_parameters_to_context(codecCtx_, codecPar) < 0) {
            LOG_ERROR("Failed to copy audio codec parameters");
            close();
            return false;
        }

        // Open codec
        if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            LOG_ERROR("Failed to open audio codec");
            close();
            return false;
        }

        // Allocate frame
        frame_ = av_frame_alloc();
        if (!frame_) {
            LOG_ERROR("Failed to allocate audio frame");
            close();
            return false;
        }

        // Setup resampler to convert to float format
        if (!setupResampler()) {
            close();
            return false;
        }

        // Fill audio info
        AVStream* audioStream = formatCtx_->streams[audioStreamIndex_];
        info_.channels = codecCtx_->ch_layout.nb_channels;
        info_.sampleRate = codecCtx_->sample_rate;
        info_.codecName = codec->name;
        info_.bitRate = static_cast<int>(codecCtx_->bit_rate);

        // Calculate duration
        if (formatCtx_->duration != AV_NOPTS_VALUE) {
            info_.duration = static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
        } else if (audioStream->duration != AV_NOPTS_VALUE) {
            info_.duration = static_cast<double>(audioStream->duration) * av_q2d(audioStream->time_base);
        }

        // Calculate total samples
        if (info_.duration > 0 && info_.sampleRate > 0) {
            info_.totalSamples = static_cast<int64_t>(info_.duration * info_.sampleRate);
        }

        timeBase_ = av_q2d(audioStream->time_base);
        isOpen_ = true;
        currentTime_ = 0.0;
        atEnd_ = false;

        LOG_INFO("Opened audio: {} ch, {} Hz, {:.2f}s, codec: {}",
                 info_.channels, info_.sampleRate, info_.duration, info_.codecName);
        return true;
    }

    void close() {
        stopBackgroundDecoding();

        if (swrCtx_) {
            swr_free(&swrCtx_);
            swrCtx_ = nullptr;
        }
        if (frame_) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (codecCtx_) {
            avcodec_free_context(&codecCtx_);
            codecCtx_ = nullptr;
        }
        if (formatCtx_) {
            avformat_close_input(&formatCtx_);
            formatCtx_ = nullptr;
        }

        info_ = AudioInfo{};
        audioStreamIndex_ = -1;
        isOpen_ = false;
        currentTime_ = 0.0;
        atEnd_ = false;
    }

    bool isOpen() const { return isOpen_; }
    const AudioInfo& getInfo() const { return info_; }
    double getCurrentTime() const { return currentTime_; }
    bool isAtEnd() const { return !isOpen_ || atEnd_; }

    bool seek(double timeSeconds) {
        if (!isOpen_) return false;

        // Calculate timestamp in audio stream time base
        int64_t timestamp = static_cast<int64_t>(timeSeconds / timeBase_);

        if (av_seek_frame(formatCtx_, audioStreamIndex_, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            LOG_WARNING("Audio seek failed");
            return false;
        }

        avcodec_flush_buffers(codecCtx_);
        atEnd_ = false;
        currentTime_ = timeSeconds;

        return true;
    }

    AudioFrame decodeNextFrame() {
        if (!isOpen_) return AudioFrame{};

        AVPacket* packet = av_packet_alloc();
        if (!packet) return AudioFrame{};

        AudioFrame result;
        bool gotFrame = false;

        while (!gotFrame) {
            int readResult = av_read_frame(formatCtx_, packet);
            if (readResult < 0) {
                if (readResult == AVERROR_EOF) {
                    atEnd_ = true;
                }
                av_packet_free(&packet);
                return AudioFrame{};
            }

            if (packet->stream_index == audioStreamIndex_) {
                int sendResult = avcodec_send_packet(codecCtx_, packet);
                if (sendResult >= 0) {
                    int receiveResult = avcodec_receive_frame(codecCtx_, frame_);
                    if (receiveResult >= 0) {
                        // Convert to float format
                        result = convertFrame();
                        if (result.isValid()) {
                            gotFrame = true;
                        }
                    }
                }
            }

            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        return result;
    }

    // Background decoding
    void startBackgroundDecoding(AudioBuffer* buffer) {
        if (!isOpen_) return;
        if (backgroundThread_.joinable()) return;
        if (!buffer) return;

        targetBuffer_ = buffer;
        stopDecoding_ = false;
        backgroundThread_ = std::thread(&Impl::backgroundDecodingLoop, this);
    }

    void stopBackgroundDecoding() {
        if (!backgroundThread_.joinable()) return;

        stopDecoding_ = true;
        backgroundThread_.join();
        targetBuffer_ = nullptr;
    }

    bool isBackgroundDecodingActive() const {
        return backgroundThread_.joinable() && !stopDecoding_;
    }

private:
    bool setupResampler() {
        // Create resampler context
        swrCtx_ = swr_alloc();
        if (!swrCtx_) {
            LOG_ERROR("Failed to allocate resampler context");
            return false;
        }

        // Set input options
        av_opt_set_chlayout(swrCtx_, "in_chlayout", &codecCtx_->ch_layout, 0);
        av_opt_set_int(swrCtx_, "in_sample_rate", codecCtx_->sample_rate, 0);
        av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt", codecCtx_->sample_fmt, 0);

        // Set output options - convert to float planar, then interleave
        AVChannelLayout outLayout;
        av_channel_layout_copy(&outLayout, &codecCtx_->ch_layout);
        av_opt_set_chlayout(swrCtx_, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swrCtx_, "out_sample_rate", codecCtx_->sample_rate, 0);
        av_opt_set_sample_fmt(swrCtx_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);  // Float interleaved

        // Initialize resampler
        if (swr_init(swrCtx_) < 0) {
            LOG_ERROR("Failed to initialize resampler");
            swr_free(&swrCtx_);
            swrCtx_ = nullptr;
            return false;
        }

        av_channel_layout_uninit(&outLayout);
        return true;
    }

    AudioFrame convertFrame() {
        AudioFrame result;

        int outSamples = frame_->nb_samples;
        int channels = info_.channels;

        // Allocate output buffer
        result.samples.resize(static_cast<size_t>(outSamples) * channels);

        // Convert samples
        uint8_t* outBuffer = reinterpret_cast<uint8_t*>(result.samples.data());
        int convertedSamples = swr_convert(swrCtx_,
                                           &outBuffer, outSamples,
                                           const_cast<const uint8_t**>(frame_->extended_data),
                                           frame_->nb_samples);

        if (convertedSamples < 0) {
            LOG_ERROR("Audio sample conversion failed");
            return AudioFrame{};
        }

        // Fill frame metadata
        result.channels = channels;
        result.sampleRate = info_.sampleRate;
        result.sampleCount = convertedSamples;

        // Resize if fewer samples were converted
        if (convertedSamples < outSamples) {
            result.samples.resize(static_cast<size_t>(convertedSamples) * channels);
        }

        // Calculate PTS
        if (frame_->pts != AV_NOPTS_VALUE) {
            result.pts = static_cast<double>(frame_->pts) * timeBase_;
            currentTime_ = result.pts;
        } else {
            result.pts = currentTime_;
            if (info_.sampleRate > 0) {
                currentTime_ += static_cast<double>(convertedSamples) / info_.sampleRate;
            }
        }

        return result;
    }

    void backgroundDecodingLoop() {
        while (!stopDecoding_) {
            AudioFrame frame = decodeNextFrame();
            if (!frame.isValid()) {
                if (atEnd_) {
                    targetBuffer_->signalEndOfStream();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Push samples to buffer, blocking if full
            if (!targetBuffer_->pushBlocking(frame.samples.data(),
                                             frame.samples.size(),
                                             frame.pts,
                                             100)) {
                if (stopDecoding_ || targetBuffer_->isEndOfStream()) {
                    break;
                }
            }
        }
    }

    // FFmpeg objects
    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    AVFrame* frame_ = nullptr;

    // Stream info
    int audioStreamIndex_ = -1;
    AudioInfo info_;
    double timeBase_ = 0.0;

    // State
    bool isOpen_ = false;
    double currentTime_ = 0.0;
    bool atEnd_ = false;

    // Background decoding
    std::thread backgroundThread_;
    AudioBuffer* targetBuffer_ = nullptr;
    std::atomic<bool> stopDecoding_{false};
};

#else // !QUADMAPPER_HAS_FFMPEG

// Stub implementation when FFmpeg is not available
namespace quadmapper {

class AudioDecoder::Impl {
public:
    bool open(const std::filesystem::path&) { return false; }
    void close() {}
    bool isOpen() const { return false; }
    const AudioInfo& getInfo() const { return info_; }
    bool seek(double) { return false; }
    AudioFrame decodeNextFrame() { return AudioFrame{}; }
    double getCurrentTime() const { return 0.0; }
    bool isAtEnd() const { return true; }
    void startBackgroundDecoding(AudioBuffer*) {}
    void stopBackgroundDecoding() {}
    bool isBackgroundDecodingActive() const { return false; }
private:
    AudioInfo info_;
};

#endif // QUADMAPPER_HAS_FFMPEG

// Public interface implementation
AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;
AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;

bool AudioDecoder::open(const std::filesystem::path& path) { return impl_->open(path); }
void AudioDecoder::close() { impl_->close(); }
bool AudioDecoder::isOpen() const { return impl_->isOpen(); }
const AudioInfo& AudioDecoder::getInfo() const { return impl_->getInfo(); }
bool AudioDecoder::seek(double timeSeconds) { return impl_->seek(timeSeconds); }
AudioFrame AudioDecoder::decodeNextFrame() { return impl_->decodeNextFrame(); }
double AudioDecoder::getCurrentTime() const { return impl_->getCurrentTime(); }
bool AudioDecoder::isAtEnd() const { return impl_->isAtEnd(); }
void AudioDecoder::startBackgroundDecoding(AudioBuffer* buffer) { impl_->startBackgroundDecoding(buffer); }
void AudioDecoder::stopBackgroundDecoding() { impl_->stopBackgroundDecoding(); }
bool AudioDecoder::isBackgroundDecodingActive() const { return impl_->isBackgroundDecodingActive(); }

} // namespace quadmapper
