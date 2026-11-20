#include "HapExport.hpp"

#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/MediaLimits.hpp"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#ifdef QUADMAPPER_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace fs = std::filesystem;

namespace quadmapper {

fs::path firstFreePath(const fs::path& desired) {
    std::error_code ec;
    if (!fs::exists(desired, ec)) return desired;
    constexpr int MAX_SUFFIX = 10000;
    fs::path candidate = desired;
    for (int n = 2; n < MAX_SUFFIX; ++n) {
        fs::path name = desired.stem();
        name += " " + std::to_string(n);
        name += desired.extension();
        candidate = desired.parent_path() / name;
        if (!fs::exists(candidate, ec)) break;
    }
    return candidate;
}

fs::path hapOutputPath(const fs::path& source, hap::HapVariant variant) {
    fs::path name = source.stem();
    name += variant == hap::HapVariant::Hap ? "_hap.mov" : "_hapq.mov";
    return firstFreePath(source.parent_path() / name);
}

#ifdef QUADMAPPER_HAS_FFMPEG

namespace {

constexpr int64_t AAC_BIT_RATE_PER_CHANNEL = 96000;

std::string ffmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, text, sizeof(text));
    return text;
}

// Sound a QuickTime file carries as it is. Anything else is re-encoded to AAC.
bool movHoldsAudio(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_MP3:
        case AV_CODEC_ID_ALAC:
        case AV_CODEC_ID_AC3:
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S16BE:
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S24BE:
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_S32BE:
        case AV_CODEC_ID_PCM_F32LE:
        case AV_CODEC_ID_PCM_F32BE:
        case AV_CODEC_ID_PCM_U8:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_MULAW:
            return true;
        default:
            return false;
    }
}

// The YUV matrix swscale should use, by the rule playback uses (VideoDecoder): the
// tagged one, else BT.709 for HD and larger and BT.601 below, so the HAP file looks
// like the original did.
int swsColorspace(const AVFrame* frame, const AVCodecContext* decoder) {
    AVColorSpace space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED) space = decoder->colorspace;
    switch (space) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_SMPTE240M:
            return SWS_CS_ITU709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_FCC:
            return SWS_CS_ITU601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return SWS_CS_BT2020;
        default: {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
            if (desc && std::string(desc->name).rfind("yuvj", 0) == 0) return SWS_CS_ITU601;
            return frame->height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        }
    }
}

// One conversion: demux the source, decode its video to RGBA, encode HAP frames and
// mux them to a .mov with the sound copied or re-encoded.
class Transcoder {
public:
    Transcoder(hap::HapVariant variant, int threads, const std::atomic<bool>& cancel,
               const HapExportProgress& progress)
        : variant_(variant), threads_(std::max(1, threads)), cancel_(cancel), progress_(progress) {}

    ~Transcoder() { release(); }

    Transcoder(const Transcoder&) = delete;
    Transcoder& operator=(const Transcoder&) = delete;

    // Writes `temp`; the caller renames it once this returns Done.
    HapExportResult run(const fs::path& source, const fs::path& temp) {
        HapExportResult result;
        std::string error;
        const bool ok = openInput(source, error) && openOutput(temp, error) && transcode(error);
        result.frames = frames_;
        result.audioNote = audioNote_;
        if (cancelled_) {
            result.status = HapExportResult::Status::Cancelled;
        } else if (ok) {
            result.status = HapExportResult::Status::Done;
        } else {
            result.status = HapExportResult::Status::Failed;
            result.error = error;
        }
        release();  // closes the output file, so the caller can rename or delete it
        return result;
    }

private:
    enum class AudioMode { None, Copy, Encode };

    bool openInput(const fs::path& source, std::string& error) {
        const std::string url = utf8FromPath(source);
        if (avformat_open_input(&in_, url.c_str(), nullptr, nullptr) < 0) {
            error = "The file could not be opened.";
            return false;
        }
        if (avformat_find_stream_info(in_, nullptr) < 0) {
            error = "The file's streams could not be read.";
            return false;
        }
        const AVCodec* decoder = nullptr;
        videoIn_ = av_find_best_stream(in_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (videoIn_ < 0 || !decoder) {
            error = "The file has no video QUADmapper can decode.";
            return false;
        }
        AVStream* stream = in_->streams[videoIn_];
        videoDec_ = avcodec_alloc_context3(decoder);
        if (!videoDec_ || avcodec_parameters_to_context(videoDec_, stream->codecpar) < 0) {
            error = "The video decoder could not be set up.";
            return false;
        }
        videoDec_->pkt_timebase = stream->time_base;
        videoDec_->thread_count = threads_;
        if (avcodec_open2(videoDec_, decoder, nullptr) < 0) {
            error = std::string("The ") + decoder->name + " video decoder could not be opened.";
            return false;
        }
        width_ = videoDec_->width;
        height_ = videoDec_->height;
        if (width_ <= 0 || height_ <= 0) {
            error = "The video has no picture size.";
            return false;
        }
        if (const std::string tooLarge = pictureSizeProblem(width_, height_); !tooLarge.empty()) {
            error = "The video's picture size, " + tooLarge + ".";
            return false;
        }

        // Every stream is moved by the same amount so the first frame is at 0 and
        // sound stays in step with the picture.
        startUs_ = in_->start_time != AV_NOPTS_VALUE ? in_->start_time : 0;
        videoOffset_ = av_rescale_q(startUs_, AV_TIME_BASE_Q, stream->time_base);
        frameRate_ = av_guess_frame_rate(in_, stream, nullptr);
        if (frameRate_.num <= 0 || frameRate_.den <= 0) frameRate_ = AVRational{30, 1};
        frameDuration_ = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(frameRate_), stream->time_base));
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            durationSeconds_ = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        } else if (in_->duration != AV_NOPTS_VALUE && in_->duration > 0) {
            durationSeconds_ = static_cast<double>(in_->duration) / AV_TIME_BASE;
        }

        audioIn_ = av_find_best_stream(in_, AVMEDIA_TYPE_AUDIO, -1, videoIn_, nullptr, 0);
        rgba_.resize(static_cast<size_t>(width_) * height_ * 4);
        packet_ = av_packet_alloc();
        outPacket_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!packet_ || !outPacket_ || !frame_) {
            error = "Out of memory.";
            return false;
        }
        return true;
    }

    bool openOutput(const fs::path& temp, std::string& error) {
        const std::string url = utf8FromPath(temp);
        if (avformat_alloc_output_context2(&out_, nullptr, "mov", url.c_str()) < 0 || !out_) {
            error = "The QuickTime writer is not available.";
            return false;
        }
        AVStream* in = in_->streams[videoIn_];
        videoOut_ = avformat_new_stream(out_, nullptr);
        if (!videoOut_) {
            error = "Out of memory.";
            return false;
        }
        AVCodecParameters* par = videoOut_->codecpar;
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = AV_CODEC_ID_HAP;
        par->codec_tag = variant_ == hap::HapVariant::Hap ? MKTAG('H', 'a', 'p', '1') : MKTAG('H', 'a', 'p', 'Y');
        par->width = width_;
        par->height = height_;
        par->sample_aspect_ratio = in->codecpar->sample_aspect_ratio;
        videoOut_->time_base = in->time_base;
        videoOut_->avg_frame_rate = in->avg_frame_rate;
        videoOut_->r_frame_rate = in->r_frame_rate;
        videoOut_->sample_aspect_ratio = in->sample_aspect_ratio;

        if (audioIn_ >= 0) setUpAudio();

        if (avio_open(&out_->pb, url.c_str(), AVIO_FLAG_WRITE) < 0) {
            error = "The output file could not be created. Is the folder writable?";
            return false;
        }
        const int rc = avformat_write_header(out_, nullptr);
        if (rc < 0) {
            error = "The QuickTime file could not be started (" + ffmpegError(rc) + ").";
            return false;
        }
        return true;
    }

    // Copy the sound if a .mov holds it, else re-encode it to AAC, else leave it out.
    void setUpAudio() {
        AVStream* in = in_->streams[audioIn_];
        const std::string codec = avcodec_get_name(in->codecpar->codec_id);
        if (movHoldsAudio(in->codecpar->codec_id)) {
            audioOut_ = avformat_new_stream(out_, nullptr);
            if (audioOut_ && avcodec_parameters_copy(audioOut_->codecpar, in->codecpar) >= 0) {
                audioOut_->codecpar->codec_tag = 0;  // the .mov writer picks its own tag
                audioOut_->time_base = in->time_base;
                audioOffset_ = av_rescale_q(startUs_, AV_TIME_BASE_Q, in->time_base);
                audioMode_ = AudioMode::Copy;
                audioNote_ = "Sound copied (" + codec + ").";
                return;
            }
            audioBroken_ = audioOut_ != nullptr;
            audioNote_ = "Sound left out: it could not be copied.";
            return;
        }

        const AVCodec* decoder = avcodec_find_decoder(in->codecpar->codec_id);
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            audioNote_ = "Sound left out: a .mov cannot hold " + codec + " and no AAC encoder is available.";
            return;
        }
        if (!decoder) {
            audioNote_ = "Sound left out: " + codec + " cannot be decoded.";
            return;
        }
        audioDec_ = avcodec_alloc_context3(decoder);
        audioEnc_ = avcodec_alloc_context3(encoder);
        bool ok = audioDec_ && audioEnc_ && avcodec_parameters_to_context(audioDec_, in->codecpar) >= 0;
        if (ok) {
            audioDec_->pkt_timebase = in->time_base;
            ok = avcodec_open2(audioDec_, decoder, nullptr) >= 0 && audioDec_->sample_rate > 0 &&
                 audioDec_->ch_layout.nb_channels > 0;
        }
        if (ok) {
            audioEnc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
            audioEnc_->sample_rate = audioDec_->sample_rate;
            if (audioDec_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
                av_channel_layout_default(&audioEnc_->ch_layout, audioDec_->ch_layout.nb_channels);
            } else {
                ok = av_channel_layout_copy(&audioEnc_->ch_layout, &audioDec_->ch_layout) >= 0;
            }
        }
        if (ok) {
            audioEnc_->bit_rate = AAC_BIT_RATE_PER_CHANNEL * audioEnc_->ch_layout.nb_channels;
            audioEnc_->time_base = AVRational{1, audioEnc_->sample_rate};
            if (out_->oformat->flags & AVFMT_GLOBALHEADER) audioEnc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            ok = avcodec_open2(audioEnc_, encoder, nullptr) >= 0;
        }
        if (ok) {
            swr_ = swr_alloc();
            fifo_ = av_audio_fifo_alloc(audioEnc_->sample_fmt, audioEnc_->ch_layout.nb_channels,
                                        std::max(1, audioEnc_->frame_size));
            audioFrame_ = av_frame_alloc();
            resampled_ = av_frame_alloc();
            ok = swr_ && fifo_ && audioFrame_ && resampled_;
        }
        if (ok) {
            audioOut_ = avformat_new_stream(out_, nullptr);  // last: a stream cannot be removed
            ok = audioOut_ != nullptr;
            if (ok && avcodec_parameters_from_context(audioOut_->codecpar, audioEnc_) < 0) {
                audioBroken_ = true;  // fail rather than write a broken track
                ok = false;
            }
        }
        if (!ok) {
            audioNote_ = "Sound left out: " + codec + " could not be re-encoded to AAC.";
            return;
        }
        audioOut_->time_base = audioEnc_->time_base;
        audioOffset_ = av_rescale_q(startUs_, AV_TIME_BASE_Q, in->time_base);
        audioMode_ = AudioMode::Encode;
        audioNote_ = "Sound re-encoded to AAC (a .mov cannot hold " + codec + ").";
    }

    bool transcode(std::string& error) {
        if (audioBroken_) {
            error = "The sound could not be set up.";
            return false;
        }
        for (;;) {
            if (cancel_) {
                cancelled_ = true;
                return false;
            }
            const int rc = av_read_frame(in_, packet_);
            if (rc == AVERROR_EOF) break;
            if (rc < 0) {
                error = "Reading the file failed (" + ffmpegError(rc) + ").";
                return false;
            }
            bool ok = true;
            if (packet_->stream_index == videoIn_) {
                ok = decodeVideo(packet_, error);
            } else if (packet_->stream_index == audioIn_ && audioMode_ == AudioMode::Copy) {
                ok = copyAudio(packet_, error);
            } else if (packet_->stream_index == audioIn_ && audioMode_ == AudioMode::Encode) {
                ok = decodeAudio(packet_, error);
            }
            av_packet_unref(packet_);
            if (!ok) return false;
        }
        if (!decodeVideo(nullptr, error)) return false;
        if (audioMode_ == AudioMode::Encode && !finishAudio(error)) return false;
        if (frames_ == 0) {
            error = "No video frames could be decoded.";
            return false;
        }
        const int rc = av_write_trailer(out_);
        if (rc < 0) {
            error = "Finishing the QuickTime file failed (" + ffmpegError(rc) + ").";
            return false;
        }
        return true;
    }

    // --- Video ---

    // packet null: drain the decoder at the end of the file.
    bool decodeVideo(const AVPacket* packet, std::string& error) {
        const int sent = avcodec_send_packet(videoDec_, packet);
        if (sent < 0 && sent != AVERROR_EOF) {
            // A damaged packet costs its frame, not the conversion.
            Logger::warning("Convert to HAP: skipped a damaged video packet ({})", ffmpegError(sent));
            return true;
        }
        for (;;) {
            const int rc = avcodec_receive_frame(videoDec_, frame_);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                Logger::warning("Convert to HAP: a video frame failed to decode ({})", ffmpegError(rc));
                return true;
            }
            const bool ok = writeVideoFrame(frame_, error);
            av_frame_unref(frame_);
            if (!ok) return false;
        }
    }

    bool writeVideoFrame(const AVFrame* frame, std::string& error) {
        if (cancel_) {
            cancelled_ = true;
            return false;
        }
        const auto format = static_cast<AVPixelFormat>(frame->format);
        SwsContext* previous = sws_;
        sws_ = sws_getCachedContext(sws_, frame->width, frame->height, format, width_, height_,
                                    AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws_) {
            error = std::string("The video's pixel format (") + av_get_pix_fmt_name(format) +
                    ") cannot be converted.";
            return false;
        }
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
        const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
                               (desc && std::string(desc->name).rfind("yuvj", 0) == 0);
        const int colorKey = swsColorspace(frame, videoDec_) * 2 + (fullRange ? 1 : 0);
        if (sws_ != previous || colorKey != colorKey_) {
            // swscale ignores this for RGB sources, which have no matrix.
            sws_setColorspaceDetails(sws_, sws_getCoefficients(colorKey / 2), fullRange ? 1 : 0,
                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
            colorKey_ = colorKey;
        }
        uint8_t* planes[4] = {rgba_.data(), nullptr, nullptr, nullptr};
        int strides[4] = {width_ * 4, 0, 0, 0};
        if (sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, planes, strides) <= 0) {
            error = std::string("A ") + av_get_pix_fmt_name(format) + " frame could not be converted.";
            return false;
        }

        const std::vector<uint8_t> hapFrame = hap::encodeHapFrame(
            rgba_.data(), width_, height_, static_cast<size_t>(width_) * 4, variant_, threads_);

        // Times stay the source's; a frame without one follows the previous frame.
        const AVRational inBase = in_->streams[videoIn_]->time_base;
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = nextVideoTs_;
        const int64_t duration = frame->duration > 0 ? frame->duration : frameDuration_;
        nextVideoTs_ = ts + duration;
        int64_t pts = av_rescale_q(ts - videoOffset_, inBase, videoOut_->time_base);
        if (lastVideoPts_ != AV_NOPTS_VALUE && pts <= lastVideoPts_) pts = lastVideoPts_ + 1;
        lastVideoPts_ = pts;

        if (av_new_packet(outPacket_, static_cast<int>(hapFrame.size())) < 0) {
            error = "Out of memory.";
            return false;
        }
        std::copy(hapFrame.begin(), hapFrame.end(), outPacket_->data);
        outPacket_->pts = outPacket_->dts = pts;
        outPacket_->duration = std::max<int64_t>(1, av_rescale_q(duration, inBase, videoOut_->time_base));
        outPacket_->flags |= AV_PKT_FLAG_KEY;
        outPacket_->stream_index = videoOut_->index;
        const int rc = av_interleaved_write_frame(out_, outPacket_);
        if (rc < 0) {
            error = "Writing the file failed (" + ffmpegError(rc) + "). Is the disk full?";
            return false;
        }
        ++frames_;
        if (progress_ && durationSeconds_ > 0.0) {
            const double seconds = static_cast<double>(ts - videoOffset_ + duration) * av_q2d(inBase);
            progress_(std::clamp(seconds / durationSeconds_, 0.0, 1.0));
        }
        return true;
    }

    // --- Audio ---

    bool copyAudio(AVPacket* packet, std::string& error) {
        if (packet->pts != AV_NOPTS_VALUE) packet->pts -= audioOffset_;
        if (packet->dts != AV_NOPTS_VALUE) packet->dts -= audioOffset_;
        av_packet_rescale_ts(packet, in_->streams[audioIn_]->time_base, audioOut_->time_base);
        packet->stream_index = audioOut_->index;
        packet->pos = -1;
        const int rc = av_interleaved_write_frame(out_, packet);
        if (rc < 0) {
            error = "Writing the sound failed (" + ffmpegError(rc) + ").";
            return false;
        }
        return true;
    }

    bool decodeAudio(const AVPacket* packet, std::string& error) {
        const int sent = avcodec_send_packet(audioDec_, packet);
        if (sent < 0 && sent != AVERROR_EOF) {
            Logger::warning("Convert to HAP: skipped a damaged sound packet ({})", ffmpegError(sent));
            return true;
        }
        for (;;) {
            const int rc = avcodec_receive_frame(audioDec_, audioFrame_);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return true;  // a bad sound frame is skipped, like a bad packet
            const bool ok = resample(audioFrame_, error);
            av_frame_unref(audioFrame_);
            if (!ok) return false;
        }
    }

    // Resample one decoded frame (null: flush the resampler) into the FIFO the
    // encoder takes fixed-size frames from.
    bool resample(const AVFrame* decoded, std::string& error) {
        if (decoded && audioNextPts_ == AV_NOPTS_VALUE) {
            // The encoded sound starts where the source's did.
            audioNextPts_ = decoded->pts == AV_NOPTS_VALUE
                ? 0
                : av_rescale_q(decoded->pts - audioOffset_, in_->streams[audioIn_]->time_base,
                               audioEnc_->time_base);
        }
        resampled_->format = audioEnc_->sample_fmt;
        resampled_->sample_rate = audioEnc_->sample_rate;
        if (av_channel_layout_copy(&resampled_->ch_layout, &audioEnc_->ch_layout) < 0) {
            error = "Out of memory.";
            return false;
        }
        if (decoded && !swrConfigured_) {
            if (swr_config_frame(swr_, resampled_, decoded) < 0 || swr_init(swr_) < 0) {
                error = "The sound could not be resampled.";
                return false;
            }
            swrConfigured_ = true;
        }
        if (!swrConfigured_) return true;  // flushing a resampler that never ran
        const int rc = swr_convert_frame(swr_, resampled_, decoded);
        if (rc < 0) {
            av_frame_unref(resampled_);
            error = "The sound could not be resampled (" + ffmpegError(rc) + ").";
            return false;
        }
        const bool ok = resampled_->nb_samples <= 0 ||
                        av_audio_fifo_write(fifo_, reinterpret_cast<void**>(resampled_->extended_data),
                                            resampled_->nb_samples) >= resampled_->nb_samples;
        av_frame_unref(resampled_);
        if (!ok) {
            error = "Out of memory.";
            return false;
        }
        return encodeAudio(false, error);
    }

    // Encode whole frames from the FIFO; at the end (flush) the rest too.
    bool encodeAudio(bool flush, std::string& error) {
        const int frameSize = audioEnc_->frame_size > 0 ? audioEnc_->frame_size : 1024;
        while (av_audio_fifo_size(fifo_) >= frameSize || (flush && av_audio_fifo_size(fifo_) > 0)) {
            const int available = std::min(frameSize, av_audio_fifo_size(fifo_));
            const bool shortAllowed = (audioEnc_->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                error = "Out of memory.";
                return false;
            }
            frame->nb_samples = shortAllowed ? available : frameSize;
            frame->format = audioEnc_->sample_fmt;
            frame->sample_rate = audioEnc_->sample_rate;
            bool ok = av_channel_layout_copy(&frame->ch_layout, &audioEnc_->ch_layout) >= 0 &&
                      av_frame_get_buffer(frame, 0) >= 0 &&
                      av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame->data), available) == available;
            if (ok && available < frame->nb_samples) {
                av_samples_set_silence(frame->data, available, frame->nb_samples - available,
                                       frame->ch_layout.nb_channels, audioEnc_->sample_fmt);
            }
            if (ok) {
                frame->pts = audioNextPts_;
                audioNextPts_ += frame->nb_samples;
                ok = avcodec_send_frame(audioEnc_, frame) >= 0;
            }
            av_frame_free(&frame);
            if (!ok) {
                error = "The sound could not be encoded.";
                return false;
            }
            if (!writeAudioPackets(error)) return false;
        }
        return true;
    }

    bool writeAudioPackets(std::string& error) {
        for (;;) {
            const int rc = avcodec_receive_packet(audioEnc_, outPacket_);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                error = "The sound could not be encoded (" + ffmpegError(rc) + ").";
                return false;
            }
            av_packet_rescale_ts(outPacket_, audioEnc_->time_base, audioOut_->time_base);
            outPacket_->stream_index = audioOut_->index;
            const int written = av_interleaved_write_frame(out_, outPacket_);
            if (written < 0) {
                error = "Writing the sound failed (" + ffmpegError(written) + ").";
                return false;
            }
        }
    }

    bool finishAudio(std::string& error) {
        if (!decodeAudio(nullptr, error) || !resample(nullptr, error) || !encodeAudio(true, error)) return false;
        if (avcodec_send_frame(audioEnc_, nullptr) < 0) return true;  // nothing was ever sent
        return writeAudioPackets(error);
    }

    void release() {
        if (out_) {
            if (out_->pb) avio_closep(&out_->pb);
            avformat_free_context(out_);
            out_ = nullptr;
        }
        if (in_) avformat_close_input(&in_);
        avcodec_free_context(&videoDec_);
        avcodec_free_context(&audioDec_);
        avcodec_free_context(&audioEnc_);
        sws_freeContext(sws_);
        sws_ = nullptr;
        swr_free(&swr_);
        if (fifo_) {
            av_audio_fifo_free(fifo_);
            fifo_ = nullptr;
        }
        av_frame_free(&frame_);
        av_frame_free(&audioFrame_);
        av_frame_free(&resampled_);
        av_packet_free(&packet_);
        av_packet_free(&outPacket_);
    }

    const hap::HapVariant variant_;
    const int threads_;
    const std::atomic<bool>& cancel_;
    const HapExportProgress& progress_;
    bool cancelled_ = false;

    AVFormatContext* in_ = nullptr;
    AVFormatContext* out_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVPacket* outPacket_ = nullptr;
    int64_t startUs_ = 0;  // the source's start, removed from every timestamp

    // Video
    int videoIn_ = -1;
    AVStream* videoOut_ = nullptr;
    AVCodecContext* videoDec_ = nullptr;
    SwsContext* sws_ = nullptr;
    int colorKey_ = -1;
    AVFrame* frame_ = nullptr;
    std::vector<uint8_t> rgba_;
    int width_ = 0;
    int height_ = 0;
    AVRational frameRate_{30, 1};
    int64_t frameDuration_ = 1;  // one frame, in the source stream's time base
    int64_t videoOffset_ = 0;
    int64_t nextVideoTs_ = 0;
    int64_t lastVideoPts_ = AV_NOPTS_VALUE;
    double durationSeconds_ = 0.0;
    int64_t frames_ = 0;

    // Audio
    AudioMode audioMode_ = AudioMode::None;
    bool audioBroken_ = false;
    std::string audioNote_;
    int audioIn_ = -1;
    AVStream* audioOut_ = nullptr;
    int64_t audioOffset_ = 0;
    AVCodecContext* audioDec_ = nullptr;
    AVCodecContext* audioEnc_ = nullptr;
    SwrContext* swr_ = nullptr;
    bool swrConfigured_ = false;
    AVAudioFifo* fifo_ = nullptr;
    AVFrame* audioFrame_ = nullptr;
    AVFrame* resampled_ = nullptr;
    int64_t audioNextPts_ = AV_NOPTS_VALUE;
};

} // namespace

HapExportResult convertToHap(const fs::path& source, const fs::path& output, hap::HapVariant variant,
                             int threads, const std::atomic<bool>& cancel, const HapExportProgress& progress) {
    fs::path temp = output;
    temp += ".part";
    temp = firstFreePath(temp);

    HapExportResult result = Transcoder(variant, threads, cancel, progress).run(source, temp);
    std::error_code ec;
    if (result.status == HapExportResult::Status::Done) {
        const fs::path finalPath = firstFreePath(output);
        fs::rename(temp, finalPath, ec);
        if (ec) {
            result.status = HapExportResult::Status::Failed;
            result.error = "The finished file could not be renamed to " + utf8FromPath(finalPath.filename()) +
                           " (" + ec.message() + ").";
        } else {
            result.output = finalPath;
            Logger::info("Convert to HAP: wrote '{}' ({} frames). {}", utf8FromPath(finalPath), result.frames,
                         result.audioNote);
        }
    }
    if (result.status != HapExportResult::Status::Done) {
        fs::remove(temp, ec);
        if (result.status == HapExportResult::Status::Failed) {
            Logger::error("Convert to HAP: '{}' failed: {}", utf8FromPath(source), result.error);
        }
    }
    return result;
}

#else // !QUADMAPPER_HAS_FFMPEG

HapExportResult convertToHap(const fs::path&, const fs::path&, hap::HapVariant, int,
                             const std::atomic<bool>&, const HapExportProgress&) {
    HapExportResult result;
    result.error = "This build has no FFmpeg, so it cannot convert video.";
    return result;
}

#endif // QUADMAPPER_HAS_FFMPEG

// --- HapConversion ---

HapConversion::HapConversion(fs::path source, hap::HapVariant variant)
    : source_(std::move(source)), plannedOutput_(hapOutputPath(source_, variant)), variant_(variant) {
    thread_ = std::thread([this] {
        result_ = convertToHap(source_, plannedOutput_, variant_, workerThreads(), cancel_,
                               [this](double fraction) { progress_ = fraction; });
        finished_ = true;
    });
}

HapConversion::~HapConversion() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

int HapConversion::workerThreads() {
    const unsigned cores = std::thread::hardware_concurrency();
    return std::max(1, static_cast<int>(cores / 2));
}

} // namespace quadmapper
