// ADR-055 colour gate, end to end: a clip of known colour bars, encoded at test time in
// BT.601 and BT.709, limited and full range, plays through a VideoFileSource and the
// SourceManager's GPU conversion; the source's RGBA texture matches the reference RGB
// within +-2 per channel. H.264 (Windows Media Foundation encoder) is decoded both on
// the GPU (NV12 planes), on the GPU with the frames left there (ADR-056 zero-copy: the
// planes reach OpenGL through WGL_NV_DX_interop2) and in software (YUV420P planes);
// MPEG-2, which is limited range by definition, in software.
//
// Needs a real GL 3.3 context; SKIPs where none is available (headless CI).

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "ColourBars.hpp"
#include "renderer/DxInterop.hpp"
#include "renderer/Texture.hpp"
#include "source/SourceManager.hpp"
#include "source/VideoFileSource.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;
using namespace quadmapper::colourbars;

namespace {

struct GLContext {
    GLFWwindow* win = nullptr;
    bool ok = false;
    GLContext() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "colour-test", nullptr, nullptr);
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~GLContext() {
        if (win) { glfwDestroyWindow(win); glfwTerminate(); }
    }
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;
};

constexpr int BAR_WIDTH = 64;  // whole macroblocks: flat bars, no block straddles two
constexpr int WIDTH = BAR_WIDTH * static_cast<int>(BARS.size());
constexpr int HEIGHT = 256;
constexpr int FPS = 25;
constexpr int FRAMES = 10;

// Encode FRAMES frames of colour bars with `encoder`, tagged with the matrix and range
// in the stream and the MP4 'colr' box. Returns an empty string on success, else why not.
std::string writeBarsClip(const std::filesystem::path& path, const char* encoder, YuvMatrix matrix,
                          bool fullRange) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder);
    if (!codec) return std::string("no ") + encoder + " encoder";
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.string().c_str()) < 0) return "no mp4 muxer";
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVStream* stream = avformat_new_stream(fmt, nullptr);
    std::string error;

    bool nv12 = false;
    const void* formats = nullptr;
    int formatCount = 0;
    if (enc && avcodec_get_supported_config(enc, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats,
                                            &formatCount) >= 0 && formats) {
        const auto* list = static_cast<const AVPixelFormat*>(formats);
        for (int i = 0; i < formatCount; ++i) nv12 = nv12 || list[i] == AV_PIX_FMT_NV12;
    }
    if (enc && frame && packet && stream) {
        enc->width = WIDTH;
        enc->height = HEIGHT;
        enc->pix_fmt = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        enc->time_base = AVRational{1, FPS};
        enc->framerate = AVRational{FPS, 1};
        enc->gop_size = FPS;
        enc->max_b_frames = 0;
        enc->bit_rate = 40'000'000;  // near-lossless for flat bars
        enc->qmin = 1;
        enc->qmax = 4;
        enc->colorspace = matrix == YuvMatrix::BT601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
        enc->color_primaries = matrix == YuvMatrix::BT601 ? AVCOL_PRI_SMPTE170M : AVCOL_PRI_BT709;
        enc->color_trc = matrix == YuvMatrix::BT601 ? AVCOL_TRC_SMPTE170M : AVCOL_TRC_BT709;
        enc->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        stream->time_base = enc->time_base;
        frame->format = enc->pix_fmt;
        frame->width = WIDTH;
        frame->height = HEIGHT;
        frame->colorspace = enc->colorspace;
        frame->color_range = enc->color_range;
        AVDictionary* muxOptions = nullptr;
        av_dict_set(&muxOptions, "movflags", "+write_colr", 0);
        AVDictionary* codecOptions = nullptr;
        av_dict_set(&codecOptions, "rate_control", "quality", 0);  // h264_mf: best quality
        av_dict_set(&codecOptions, "quality", "100", 0);
        const int opened = avcodec_open2(enc, codec, &codecOptions);
        av_dict_free(&codecOptions);
        if (opened < 0) {
            error = std::string(encoder) + " would not open";
        } else if (avcodec_parameters_from_context(stream->codecpar, enc) < 0 ||
                   av_frame_get_buffer(frame, 0) < 0 ||
                   avio_open(&fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0 ||
                   avformat_write_header(fmt, &muxOptions) < 0) {
            error = "could not start the file";
        }
        av_dict_free(&muxOptions);
    } else {
        error = "out of memory";
    }

    auto drain = [&](AVFrame* input) {
        if (avcodec_send_frame(enc, input) < 0) return false;
        int rc;
        while ((rc = avcodec_receive_packet(enc, packet)) >= 0) {
            packet->duration = 1;
            av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (av_interleaved_write_frame(fmt, packet) < 0) return false;
        }
        return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF;
    };

    for (int i = 0; error.empty() && i < FRAMES; ++i) {
        if (av_frame_make_writable(frame) < 0) { error = "frame not writable"; break; }
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                const Yuv c = toYuv(BARS[static_cast<size_t>(x / BAR_WIDTH)], matrix, fullRange);
                frame->data[0][y * frame->linesize[0] + x] = static_cast<uint8_t>(c.y);
                if (x % 2 || y % 2) continue;
                if (nv12) {
                    frame->data[1][(y / 2) * frame->linesize[1] + x] = static_cast<uint8_t>(c.cb);
                    frame->data[1][(y / 2) * frame->linesize[1] + x + 1] = static_cast<uint8_t>(c.cr);
                } else {
                    frame->data[1][(y / 2) * frame->linesize[1] + x / 2] = static_cast<uint8_t>(c.cb);
                    frame->data[2][(y / 2) * frame->linesize[2] + x / 2] = static_cast<uint8_t>(c.cr);
                }
            }
        }
        frame->pts = i;
        if (!drain(frame)) error = "encoding failed";
    }
    if (error.empty() && (!drain(nullptr) || av_write_trailer(fmt) < 0)) error = "finishing failed";

    if (fmt->pb) avio_closep(&fmt->pb);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    avformat_free_context(fmt);
    return error;
}

struct Case {
    const char* encoder;
    YuvMatrix matrix;
    bool fullRange;
    bool hardware;  // ask for GPU decoding
    bool zeroCopy = false;  // and leave the frames on the GPU (ADR-056)
};

std::string caseName(const Case& c) {
    return std::string(c.encoder) + (c.matrix == YuvMatrix::BT601 ? " BT.601" : " BT.709") +
           (c.fullRange ? " full" : " limited") + (c.hardware ? " hw" : " sw") + (c.zeroCopy ? " zc" : "");
}

}  // namespace

class ColourBarsPlayback : public testing::TestWithParam<Case> {};

TEST_P(ColourBarsPlayback, TextureMatchesReferenceBars) {
    const Case c = GetParam();
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    const std::filesystem::path clip = std::filesystem::temp_directory_path() /
        ("qm_bars_" + std::string(c.encoder) + (c.matrix == YuvMatrix::BT601 ? "_601" : "_709") +
         (c.fullRange ? "_full" : "_limited") + (c.hardware ? "_hw" : "_sw") + (c.zeroCopy ? "_zc" : "") + ".mp4");
    const std::string why = writeBarsClip(clip, c.encoder, c.matrix, c.fullRange);
    if (!why.empty()) {
        std::filesystem::remove(clip);
        if (std::string(c.encoder) == "mpeg2video") FAIL() << why;  // always built in
        GTEST_SKIP() << caseName(c) << ": " << why;
    }

    bool interop = false;
    if (c.zeroCopy) {
        // Without interop the case would pass on the readback path and prove nothing.
        DxInterop probe;
        interop = probe.available();
        if (!interop) {
            std::filesystem::remove(clip);
            GTEST_SKIP() << caseName(c) << ": " << probe.unavailableReason();
        }
    }
    std::vector<uint8_t> rgba;
    bool hardware = false;
    bool zeroCopy = false;
    {
        SourceManager sm;
        DecodeOptions options = VideoFileSource::playbackDecodeOptions();
        options.hardware = c.hardware;
        options.zeroCopy = c.zeroCopy && interop;
        const UUID id = sm.registerSource(std::make_unique<VideoFileSource>(clip, options));
        ASSERT_FALSE(id.isNull());
        auto* video = sm.getSourceAs<VideoFileSource>(id);
        ASSERT_NE(video, nullptr);
        bool uploaded = false;
        for (int i = 0; i < 200 && !uploaded; ++i) {
            sm.updateAll(1.0 / 60.0);
            const FrameData* frame = video->getCurrentFrame();
            uploaded = frame && frame->isYuv();
            if (!uploaded) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(uploaded) << "no YUV frame reached the texture";
        hardware = video->isHardwareDecoding();
        zeroCopy = video->isZeroCopy();

        Texture* texture = sm.getTexture(id);
        ASSERT_TRUE(texture && texture->getWidth() == WIDTH && texture->getHeight() == HEIGHT &&
                    texture->getChannels() == 4);
        rgba.resize(static_cast<size_t>(WIDTH) * HEIGHT * 4);
        glBindTexture(GL_TEXTURE_2D, texture->getId());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    std::filesystem::remove(clip);
    if (!c.hardware) EXPECT_FALSE(hardware);
    if (c.zeroCopy && !hardware) GTEST_SKIP() << caseName(c) << ": no GPU decoding here, so no zero-copy";
    if (c.zeroCopy) EXPECT_TRUE(zeroCopy) << "the frames were copied through memory";

    int worst = 0;
    for (size_t i = 0; i < BARS.size(); ++i) {
        const size_t x = i * BAR_WIDTH + BAR_WIDTH / 2;
        const uint8_t* px = &rgba[((HEIGHT / 2) * static_cast<size_t>(WIDTH) + x) * 4];
        const Rgb got{px[0], px[1], px[2]};
        const Rgb want = BARS[i];
        worst = std::max({worst, std::abs(got.r - want.r), std::abs(got.g - want.g), std::abs(got.b - want.b)});
        EXPECT_TRUE(within(got, want, 2))
            << caseName(c) << (hardware ? " (hardware)" : " (software)") << " bar " << i << ": got ("
            << got.r << "," << got.g << "," << got.b << ") want (" << want.r << "," << want.g << ","
            << want.b << ")";
    }
    RecordProperty("worst_channel_error", worst);
    std::printf("[ colour ] %s %s: worst channel error %d\n", caseName(c).c_str(),
                zeroCopy ? "hardware (zero-copy)" : hardware ? "hardware" : "software", worst);
}

INSTANTIATE_TEST_SUITE_P(
    MatrixAndRange, ColourBarsPlayback,
    testing::Values(Case{"mpeg2video", YuvMatrix::BT601, false, false},
                    Case{"mpeg2video", YuvMatrix::BT709, false, false},
                    Case{"h264_mf", YuvMatrix::BT601, false, true}, Case{"h264_mf", YuvMatrix::BT601, true, true},
                    Case{"h264_mf", YuvMatrix::BT709, false, true}, Case{"h264_mf", YuvMatrix::BT709, true, true},
                    Case{"h264_mf", YuvMatrix::BT601, false, false}, Case{"h264_mf", YuvMatrix::BT601, true, false},
                    Case{"h264_mf", YuvMatrix::BT709, false, false}, Case{"h264_mf", YuvMatrix::BT709, true, false},
                    Case{"h264_mf", YuvMatrix::BT601, false, true, true}, Case{"h264_mf", YuvMatrix::BT601, true, true, true},
                    Case{"h264_mf", YuvMatrix::BT709, false, true, true}, Case{"h264_mf", YuvMatrix::BT709, true, true, true}),
    [](const testing::TestParamInfo<Case>& info) {
        std::string n = caseName(info.param);
        for (char& ch : n) {
            if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        }
        return n;
    });
