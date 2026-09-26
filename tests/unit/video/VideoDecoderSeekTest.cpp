// VideoDecoder seek + end-of-stream drain (QM-091). Each test encodes a tiny B-frame
// clip with the linked FFmpeg, starting 1.4 s into its timeline the way MPEG-TS
// does, then checks that one pass yields every frame (the decoder is drained at EOF)
// and that seeks land on the frame at the target, relative to the stream start.

#include <gtest/gtest.h>

#include "video/VideoDecoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int FPS = 25;
constexpr int FRAME_COUNT = 30;
constexpr int64_t START_OFFSET_FRAMES = 35;  // 1.4 s, the MPEG-TS muxer's usual start
constexpr double PTS_TOLERANCE = 1e-3;

struct ClipSpec {
    const char* extension;
    int gopSize;
};

void PrintTo(const ClipSpec& spec, std::ostream* os) {
    *os << spec.extension << " GOP " << spec.gopSize;
}

std::string clipName(const testing::TestParamInfo<ClipSpec>& info) {
    return std::string(info.param.extension + 1) + "_gop" + std::to_string(info.param.gopSize);
}

// Writes FRAME_COUNT frames of MPEG-2 video with two B-frames between references,
// muxed by file extension. Returns false if the encoder or muxer is unavailable.
bool writeClip(const fs::path& path, int gopSize) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    AVFormatContext* fmt = nullptr;
    if (!codec || avformat_alloc_output_context2(&fmt, nullptr, nullptr, path.string().c_str()) < 0) {
        return false;
    }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVStream* stream = avformat_new_stream(fmt, nullptr);

    bool ok = enc && frame && packet && stream;
    if (ok) {
        enc->width = 64;
        enc->height = 48;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->time_base = AVRational{1, FPS};
        enc->framerate = AVRational{FPS, 1};
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
             avio_open(&fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE) >= 0 &&
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

    for (int i = 0; ok && i < FRAME_COUNT; ++i) {
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
        frame->pts = START_OFFSET_FRAMES + i;
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

// Decodes until end of stream, returning each frame's pts.
std::vector<double> decodeToEnd(VideoDecoder& decoder) {
    std::vector<double> pts;
    for (VideoFrame f = decoder.decodeNextFrame(); f.isValid(); f = decoder.decodeNextFrame()) {
        pts.push_back(f.pts);
    }
    return pts;
}

class VideoDecoderSeek : public testing::TestWithParam<ClipSpec> {
protected:
    void SetUp() override {
        // One file per test, so parallel ctest runs never share a clip.
        std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        std::replace(name.begin(), name.end(), '/', '_');
        path_ = fs::temp_directory_path() / ("qm_decoder_" + name + GetParam().extension);
        if (!writeClip(path_, GetParam().gopSize)) GTEST_SKIP() << "FFmpeg cannot write " << path_;
        ASSERT_TRUE(decoder_.open(path_));
    }

    void TearDown() override {
        decoder_.close();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    double firstPtsAfterSeek(double seconds) {
        EXPECT_TRUE(decoder_.seek(seconds));
        VideoFrame f = decoder_.decodeNextFrame();
        EXPECT_TRUE(f.isValid()) << "no frame after seek(" << seconds << ")";
        return f.pts;
    }

    fs::path path_;
    VideoDecoder decoder_;
};

}  // namespace

TEST_P(VideoDecoderSeek, OnePassDecodesEveryFrameFromStreamStart) {
    const std::vector<double> pts = decodeToEnd(decoder_);

    ASSERT_EQ(pts.size(), static_cast<size_t>(FRAME_COUNT));
    for (int i = 0; i < FRAME_COUNT; ++i) {
        EXPECT_NEAR(pts[i], static_cast<double>(i) / FPS, PTS_TOLERANCE) << "frame " << i;
    }
    EXPECT_TRUE(decoder_.isAtEnd());
}

TEST_P(VideoDecoderSeek, SeekToStartAfterEndReplaysWholeClip) {
    decodeToEnd(decoder_);
    ASSERT_TRUE(decoder_.isAtEnd());

    ASSERT_TRUE(decoder_.seek(0.0));
    EXPECT_FALSE(decoder_.isAtEnd());
    const std::vector<double> pts = decodeToEnd(decoder_);

    ASSERT_EQ(pts.size(), static_cast<size_t>(FRAME_COUNT));
    EXPECT_NEAR(pts.front(), 0.0, PTS_TOLERANCE);
    EXPECT_NEAR(pts.back(), static_cast<double>(FRAME_COUNT - 1) / FPS, PTS_TOLERANCE);
}

TEST_P(VideoDecoderSeek, SeekMidClipLandsOnFrameAtTarget) {
    // Frame 15 is two frames past a keyframe (GOP 12) and not a keyframe itself.
    EXPECT_NEAR(firstPtsAfterSeek(15.0 / FPS), 15.0 / FPS, PTS_TOLERANCE);
    // Between frames, the frame on screen at that time is the earlier one.
    EXPECT_NEAR(firstPtsAfterSeek(20.5 / FPS), 20.0 / FPS, PTS_TOLERANCE);
    // Seeking backwards after a forward seek.
    EXPECT_NEAR(firstPtsAfterSeek(3.0 / FPS), 3.0 / FPS, PTS_TOLERANCE);
    EXPECT_NEAR(firstPtsAfterSeek(0.0), 0.0, PTS_TOLERANCE);
}

TEST_P(VideoDecoderSeek, SeekToEndShowsLastFrame) {
    EXPECT_NEAR(firstPtsAfterSeek(static_cast<double>(FRAME_COUNT) / FPS),
                static_cast<double>(FRAME_COUNT - 1) / FPS, PTS_TOLERANCE);
    EXPECT_FALSE(decoder_.decodeNextFrame().isValid());
    EXPECT_TRUE(decoder_.isAtEnd());
}

// Decoded frames are stored top row first, the convention every source shares (stills
// included) so texture v = 0 is the picture's top. The clip's luma grows with y, so the
// first stored row must be darker than the last.
TEST(VideoDecoderOrientation, DecodedFramesAreStoredTopRowFirst) {
    const fs::path path = fs::temp_directory_path() / "qm_decoder_orientation.mkv";
    if (!writeClip(path, 12)) GTEST_SKIP() << "FFmpeg cannot write " << path;
    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(path));
    const VideoFrame frame = decoder.decodeNextFrame();
    decoder.close();
    std::error_code ec;
    fs::remove(path, ec);
    ASSERT_TRUE(frame.isValid());
    ASSERT_EQ(frame.data.size(), static_cast<size_t>(frame.width) * frame.height * 3);

    auto rowMean = [&](int row) {
        double sum = 0.0;
        for (int i = 0; i < frame.width * 3; ++i) {
            sum += frame.data[static_cast<size_t>(row) * frame.width * 3 + i];
        }
        return sum / (frame.width * 3);
    };
    EXPECT_LT(rowMean(0) + 30.0, rowMean(frame.height - 1));
}

INSTANTIATE_TEST_SUITE_P(Containers, VideoDecoderSeek,
                         testing::Values(ClipSpec{".ts", 12},
                                         ClipSpec{".ts", 300},  // whole clip is one GOP
                                         ClipSpec{".mp4", 12},
                                         ClipSpec{".mkv", 12}),
                         clipName);
