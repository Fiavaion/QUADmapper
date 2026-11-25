// Export video (Mp4Writer, AsyncMp4Writer): synthetic frames and a tone are written,
// then the file is read back with FFmpeg as a standalone player would.

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "core/FileSystem.hpp"
#include "video/Mp4Writer.hpp"

#include "../TestPlatform.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int FPS = 30;
constexpr int FRAMES = 60;  // 2 s
constexpr int SAMPLE_RATE = 48000;
constexpr double PI = 3.14159265358979323846;

// A dark background with a bright bar that moves right one step a frame.
std::vector<uint8_t> barFrame(int width, int height, int index) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    const int barX = (index * 4) % width;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            const bool bar = x >= barX && x < barX + 16;
            p[0] = bar ? 255 : 20;
            p[1] = bar ? 200 : 30;
            p[2] = bar ? 40 : 90;
            p[3] = 255;
        }
    }
    return rgba;
}

// 440 Hz stereo tone (right channel quieter), samples [first, first + frames).
std::vector<float> tone(int64_t first, int frames) {
    std::vector<float> pcm(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double s = std::sin(2.0 * PI * 440.0 * static_cast<double>(first + i) / SAMPLE_RATE);
        pcm[i * 2] = static_cast<float>(0.5 * s);
        pcm[i * 2 + 1] = static_cast<float>(0.25 * s);
    }
    return pcm;
}

struct Probe {
    bool opened = false;
    int videoStreams = 0;
    int audioStreams = 0;
    AVCodecID videoCodec = AV_CODEC_ID_NONE;
    AVCodecID audioCodec = AV_CODEC_ID_NONE;
    int width = 0;
    int height = 0;
    int videoPackets = 0;
    double duration = 0.0;
};

Probe probe(const fs::path& path) {
    Probe result;
    AVFormatContext* fmt = nullptr;
    const std::string url = utf8FromPath(path);
    if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) < 0) return result;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return result;
    }
    result.opened = true;
    int videoIndex = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++result.videoStreams;
            videoIndex = static_cast<int>(i);
            result.videoCodec = par->codec_id;
            result.width = par->width;
            result.height = par->height;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++result.audioStreams;
            result.audioCodec = par->codec_id;
        }
    }
    result.duration = fmt->duration != AV_NOPTS_VALUE ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index == videoIndex) ++result.videoPackets;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&fmt);
    return result;
}

// Writes FRAMES frames (and the tone when the writer has sound) through either writer.
template <typename Writer>
bool writeClip(Writer& writer, const fs::path& path, int width, int height, bool audio, fs::path& written,
               std::string& error) {
    Mp4WriterOptions options;
    options.width = width;
    options.height = height;
    options.fps = FPS;
    options.audio = audio;
    if (!writer.open(path, options, error)) return false;
    const int samplesPerFrame = SAMPLE_RATE / FPS;
    for (int i = 0; i < FRAMES; ++i) {
        const std::vector<uint8_t> frame = barFrame(width, height, i);
        if (!writer.writeVideoFrame(frame.data(), width * 4, false)) {
            error = "video frame " + std::to_string(i);
            return false;
        }
        if (audio) {
            const std::vector<float> pcm = tone(static_cast<int64_t>(i) * samplesPerFrame, samplesPerFrame);
            if (!writer.writeAudio(pcm.data(), samplesPerFrame)) {
                error = "audio chunk " + std::to_string(i);
                return false;
            }
        }
    }
    return writer.finish(written, error);
}

bool isAcceptedVideoCodec(AVCodecID id) { return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_MPEG4; }

class Mp4WriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        folder_ = fs::temp_directory_path() / ("qm_mp4writer_" + std::to_string(testProcessId()));
        fs::create_directories(folder_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(folder_, ec);
    }
    fs::path folder_;
};

void expectMainClip(const fs::path& path, const fs::path& written) {
    EXPECT_EQ(written, path);
    EXPECT_FALSE(fs::exists(fs::path(path) += ".part"));
    const Probe info = probe(path);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.videoStreams, 1);
    EXPECT_TRUE(isAcceptedVideoCodec(info.videoCodec)) << avcodec_get_name(info.videoCodec);
    EXPECT_EQ(info.width, 320);
    EXPECT_EQ(info.height, 180);
    EXPECT_EQ(info.audioStreams, 1);
    EXPECT_EQ(info.audioCodec, AV_CODEC_ID_AAC);
    EXPECT_NEAR(info.duration, 2.0, 0.1);
    EXPECT_NEAR(info.videoPackets, FRAMES, 1);
}

}  // namespace

TEST_F(Mp4WriterTest, WritesH264AndAacThatReadsBack) {
    const fs::path path = folder_ / "show.mp4";
    { std::ofstream(path) << "an older export"; }  // finish replaces a file of the same name
    Mp4Writer writer;
    fs::path written;
    std::string error;
    ASSERT_TRUE(writeClip(writer, path, 320, 180, true, written, error)) << error;
    std::cout << "[ info ] video encoder: " << writer.encoderName() << "\n";
    EXPECT_FALSE(writer.encoderName().empty());
    EXPECT_EQ(writer.framesWritten(), FRAMES);
    expectMainClip(path, written);
}

TEST_F(Mp4WriterTest, AsyncWriterWritesTheSameFile) {
    const fs::path path = folder_ / "async.mp4";
    AsyncMp4Writer writer(2);  // a short queue, so the producer blocks on the worker
    fs::path written;
    std::string error;
    ASSERT_TRUE(writeClip(writer, path, 320, 180, true, written, error)) << error << " / " << writer.error();
    EXPECT_FALSE(writer.encoderName().empty());
    EXPECT_TRUE(writer.error().empty());
    expectMainClip(path, written);
}

TEST_F(Mp4WriterTest, OddSizeIsRoundedDownToEven) {
    const fs::path path = folder_ / "odd.mp4";
    Mp4Writer writer;
    fs::path written;
    std::string error;
    ASSERT_TRUE(writeClip(writer, path, 321, 181, false, written, error)) << error;
    const Probe info = probe(written);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.width, 320);
    EXPECT_EQ(info.height, 180);
}

TEST_F(Mp4WriterTest, WithoutAudioTheFileIsVideoOnly) {
    const fs::path path = folder_ / "silent.mp4";
    Mp4Writer writer;
    fs::path written;
    std::string error;
    ASSERT_TRUE(writeClip(writer, path, 320, 180, false, written, error)) << error;
    const Probe info = probe(written);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.videoStreams, 1);
    EXPECT_EQ(info.audioStreams, 0);
    EXPECT_NEAR(info.videoPackets, FRAMES, 1);
}

TEST_F(Mp4WriterTest, AudioTrackWithNothingWrittenIsStillValid) {
    const fs::path path = folder_ / "empty_sound.mp4";
    Mp4Writer writer;
    Mp4WriterOptions options;
    options.width = 320;
    options.height = 180;
    options.fps = FPS;
    options.audio = true;
    std::string error;
    ASSERT_TRUE(writer.open(path, options, error)) << error;
    for (int i = 0; i < FRAMES; ++i) {
        const std::vector<uint8_t> frame = barFrame(320, 180, i);
        ASSERT_TRUE(writer.writeVideoFrame(frame.data(), 320 * 4, false));
    }
    fs::path written;
    ASSERT_TRUE(writer.finish(written, error)) << error;
    const Probe info = probe(written);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.audioStreams, 1);
    EXPECT_NEAR(info.duration, 2.0, 0.1);
}

TEST_F(Mp4WriterTest, AbortDeletesThePartFile) {
    const fs::path path = folder_ / "cancelled.mp4";
    const fs::path part = fs::path(path) += ".part";
    Mp4Writer writer;
    Mp4WriterOptions options;
    options.width = 320;
    options.height = 180;
    std::string error;
    ASSERT_TRUE(writer.open(path, options, error)) << error;
    const std::vector<uint8_t> frame = barFrame(320, 180, 0);
    ASSERT_TRUE(writer.writeVideoFrame(frame.data(), 320 * 4, false));
    EXPECT_TRUE(fs::exists(part));
    writer.abort();
    EXPECT_FALSE(fs::exists(part));
    EXPECT_FALSE(fs::exists(path));
    writer.abort();  // idempotent
    fs::path written;
    EXPECT_FALSE(writer.finish(written, error));

    AsyncMp4Writer async;
    ASSERT_TRUE(async.open(path, options, error)) << error;
    ASSERT_TRUE(async.writeVideoFrame(frame.data(), 320 * 4, false));
    async.abort();
    EXPECT_FALSE(fs::exists(part));
    EXPECT_FALSE(fs::exists(path));
    EXPECT_FALSE(async.writeVideoFrame(frame.data(), 320 * 4, false));
}

// GL readback rows come bottom row first; the picture must still be upright.
TEST_F(Mp4WriterTest, BottomUpRowsAreFlipped) {
    constexpr int W = 64, H = 64;
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    for (int row = 0; row < H; ++row) {  // memory row 0 = picture bottom = blue; top = red
        const bool top = row >= H / 2;
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &rgba[(static_cast<size_t>(row) * W + x) * 4];
            p[0] = top ? 255 : 0;
            p[1] = 0;
            p[2] = top ? 0 : 255;
            p[3] = 255;
        }
    }
    const fs::path path = folder_ / "upright.mp4";
    Mp4Writer writer;
    Mp4WriterOptions options;
    options.width = W;
    options.height = H;
    std::string error;
    ASSERT_TRUE(writer.open(path, options, error)) << error;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(writer.writeVideoFrame(rgba.data(), W * 4, true));
    fs::path written;
    ASSERT_TRUE(writer.finish(written, error)) << error;

    AVFormatContext* fmt = nullptr;
    const std::string url = utf8FromPath(written);
    ASSERT_GE(avformat_open_input(&fmt, url.c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(fmt, nullptr), 0);
    const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    ASSERT_GE(index, 0);
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[index]->codecpar->codec_id);
    ASSERT_NE(codec, nullptr);
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[index]->codecpar);
    ASSERT_GE(avcodec_open2(dec, codec, nullptr), 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    while (!got && av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index == index) {
            avcodec_send_packet(dec, packet);
            got = avcodec_receive_frame(dec, frame) >= 0;
        }
        av_packet_unref(packet);
    }
    if (!got) {
        avcodec_send_packet(dec, nullptr);
        got = avcodec_receive_frame(dec, frame) >= 0;
    }
    ASSERT_TRUE(got);
    ASSERT_EQ(frame->format, AV_PIX_FMT_YUV420P);
    // Red: Cr high, Cb low. Blue: Cb high, Cr low. Chroma is at half resolution.
    const int cb_top = frame->data[1][4 * frame->linesize[1] + 16];
    const int cr_top = frame->data[2][4 * frame->linesize[2] + 16];
    const int cb_bottom = frame->data[1][28 * frame->linesize[1] + 16];
    const int cr_bottom = frame->data[2][28 * frame->linesize[2] + 16];
    EXPECT_GT(cr_top, 200);
    EXPECT_LT(cb_top, 128);
    EXPECT_GT(cb_bottom, 200);
    EXPECT_LT(cr_bottom, 128);
    EXPECT_EQ(fmt->streams[index]->codecpar->color_range, AVCOL_RANGE_MPEG);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}
