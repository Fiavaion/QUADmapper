#pragma once

// Tiny clips encoded at test time with the linked FFmpeg, so playback tests need no
// media files. Frame i has pts i / fps relative to the stream start, which begins
// startOffsetFrames into the timeline the way MPEG-TS does.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "core/FileSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace quadmapper::testclips {

constexpr int CLIP_WIDTH = 64;
constexpr int CLIP_HEIGHT = 48;

// MPEG-2 video with two B-frames between references, muxed by file extension.
// Returns false if the encoder or muxer is unavailable.
inline bool writeMpeg2Clip(const std::filesystem::path& path, int gopSize, int frameCount,
                           int fps, int64_t startOffsetFrames) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    AVFormatContext* fmt = nullptr;
    if (!codec || avformat_alloc_output_context2(&fmt, nullptr, nullptr, utf8FromPath(path).c_str()) < 0) {
        return false;
    }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVStream* stream = avformat_new_stream(fmt, nullptr);

    bool ok = enc && frame && packet && stream;
    if (ok) {
        enc->width = CLIP_WIDTH;
        enc->height = CLIP_HEIGHT;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->time_base = AVRational{1, fps};
        enc->framerate = AVRational{fps, 1};
        enc->gop_size = gopSize;
        enc->max_b_frames = 2;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        stream->time_base = enc->time_base;

        frame->format = enc->pix_fmt;
        frame->width = enc->width;
        frame->height = enc->height;

        ok = avcodec_open2(enc, codec, nullptr) >= 0 &&
             avcodec_parameters_from_context(stream->codecpar, enc) >= 0 &&
             av_frame_get_buffer(frame, 0) >= 0 &&
             avio_open(&fmt->pb, utf8FromPath(path).c_str(), AVIO_FLAG_WRITE) >= 0 &&
             avformat_write_header(fmt, nullptr) >= 0;
    }

    auto encode = [&](AVFrame* input) {
        if (avcodec_send_frame(enc, input) < 0) return false;
        while (avcodec_receive_packet(enc, packet) >= 0) {
            packet->duration = 1;  // one frame; MP4 sizes its edit list from it
            av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (av_interleaved_write_frame(fmt, packet) < 0) return false;
        }
        return true;
    };

    for (int i = 0; ok && i < frameCount; ++i) {
        ok = av_frame_make_writable(frame) >= 0;
        if (!ok) break;
        for (int y = 0; y < frame->height; ++y) {
            for (int x = 0; x < frame->width; ++x) {
                frame->data[0][y * frame->linesize[0] + x] = static_cast<uint8_t>(x + y + i * 8);
            }
        }
        for (int y = 0; y < frame->height / 2; ++y) {
            for (int x = 0; x < frame->width / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 128;
            }
        }
        frame->pts = startOffsetFrames + i;
        ok = encode(frame);
    }
    ok = ok && encode(nullptr) && av_write_trailer(fmt) >= 0;

    if (fmt->pb) avio_closep(&fmt->pb);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    avformat_free_context(fmt);
    return ok;
}

// HAP (DXT1, no second-stage compression) in a QuickTime container: every packet is
// a raw HAP frame, as HapFileSource expects. The frame content is constant; tests
// identify frames by timestamp.
inline bool writeHapClip(const std::filesystem::path& path, int frameCount, int fps) {
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "mov", utf8FromPath(path).c_str()) < 0) return false;
    AVStream* stream = avformat_new_stream(fmt, nullptr);
    AVPacket* packet = av_packet_alloc();
    bool ok = stream && packet;
    if (ok) {
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_HAP;
        stream->codecpar->codec_tag = MKTAG('H', 'a', 'p', '1');
        stream->codecpar->width = CLIP_WIDTH;
        stream->codecpar->height = CLIP_HEIGHT;
        stream->time_base = AVRational{1, fps};
        ok = avio_open(&fmt->pb, utf8FromPath(path).c_str(), AVIO_FLAG_WRITE) >= 0 &&
             avformat_write_header(fmt, nullptr) >= 0;
    }

    // 4-byte section header (24-bit size, type 0xAB = DXT1, no compressor) + BC1 blocks.
    const uint32_t textureBytes = (CLIP_WIDTH / 4) * (CLIP_HEIGHT / 4) * 8;
    std::vector<uint8_t> hapFrame{static_cast<uint8_t>(textureBytes & 0xFF),
                                  static_cast<uint8_t>((textureBytes >> 8) & 0xFF),
                                  static_cast<uint8_t>((textureBytes >> 16) & 0xFF), 0xAB};
    hapFrame.resize(4 + textureBytes, 0x5A);

    for (int i = 0; ok && i < frameCount; ++i) {
        ok = av_new_packet(packet, static_cast<int>(hapFrame.size())) >= 0;
        if (!ok) break;
        std::copy(hapFrame.begin(), hapFrame.end(), packet->data);
        packet->pts = packet->dts = i;
        packet->duration = 1;
        packet->flags |= AV_PKT_FLAG_KEY;
        packet->stream_index = stream->index;
        av_packet_rescale_ts(packet, AVRational{1, fps}, stream->time_base);
        ok = av_interleaved_write_frame(fmt, packet) >= 0;
    }
    ok = ok && av_write_trailer(fmt) >= 0;

    if (fmt->pb) avio_closep(&fmt->pb);
    av_packet_free(&packet);
    avformat_free_context(fmt);
    return ok;
}

}  // namespace quadmapper::testclips
