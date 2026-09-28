// Convert to HAP (QM-082) end to end: clips encoded at test time with the linked
// FFmpeg are converted, then played back through HapFileSource (the playback path)
// and FFmpeg's own HAP decoder (what other players read), and compared with the
// source frame by frame.

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include "BcDecode.hpp"
#include "core/FileSystem.hpp"
#include "source/HapFileSource.hpp"
#include "video/HapExport.hpp"
#include "video/VideoDecoder.hpp"

#include <process.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int WIDTH = 70;   // not whole 4x4 blocks: the padded edge is checked too
constexpr int HEIGHT = 42;
constexpr int FPS = 25;
constexpr int FRAMES = 30;
constexpr int SAMPLE_RATE = 44100;
constexpr double PI = 3.14159265358979323846;

// PSNR floors, justified in HapFrameEncoderTest.cpp (BC1 about 35-38 dB on graded
// content, Hap Q a few dB more). The source here is smooth, so both clear them easily;
// a wrong channel order, block layout or YCoCg convention lands far below.
constexpr double HAP_MIN_PSNR = 30.0;
constexpr double HAPQ_MIN_PSNR = 35.0;

// MPEG-4 video of smoothly moving colour (muxed by extension), with an optional
// 440 Hz tone in the given codec. Returns false if an encoder or muxer is missing.
bool writeSourceClip(const fs::path& path, AVCodecID audioCodec) {
    AVFormatContext* fmt = nullptr;
    const std::string url = utf8FromPath(path);
    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const AVCodec* soundCodec = audioCodec != AV_CODEC_ID_NONE ? avcodec_find_encoder(audioCodec) : nullptr;
    if (!videoCodec || (audioCodec != AV_CODEC_ID_NONE && !soundCodec) ||
        avformat_alloc_output_context2(&fmt, nullptr, nullptr, url.c_str()) < 0) {
        return false;
    }
    AVCodecContext* venc = avcodec_alloc_context3(videoCodec);
    AVCodecContext* aenc = soundCodec ? avcodec_alloc_context3(soundCodec) : nullptr;
    AVStream* vst = avformat_new_stream(fmt, nullptr);
    AVStream* ast = soundCodec ? avformat_new_stream(fmt, nullptr) : nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* sound = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    bool ok = venc && vst && frame && sound && packet && (!soundCodec || (aenc && ast));

    if (ok) {
        venc->width = WIDTH;
        venc->height = HEIGHT;
        venc->pix_fmt = AV_PIX_FMT_YUV420P;
        venc->time_base = AVRational{1, FPS};
        venc->framerate = AVRational{FPS, 1};
        venc->gop_size = 10;
        venc->bit_rate = 4'000'000;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        vst->time_base = venc->time_base;
        ok = avcodec_open2(venc, videoCodec, nullptr) >= 0 &&
             avcodec_parameters_from_context(vst->codecpar, venc) >= 0;
        frame->format = venc->pix_fmt;
        frame->width = WIDTH;
        frame->height = HEIGHT;
        ok = ok && av_frame_get_buffer(frame, 0) >= 0;
    }
    if (ok && aenc) {
        const void* formats = nullptr;
        int count = 0;
        avcodec_get_supported_config(aenc, soundCodec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count);
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        for (int i = 0; formats && i < count; ++i) {
            const auto f = static_cast<const AVSampleFormat*>(formats)[i];
            if (f == AV_SAMPLE_FMT_S16 || f == AV_SAMPLE_FMT_FLTP) {
                aenc->sample_fmt = f;
                break;
            }
        }
        aenc->sample_rate = SAMPLE_RATE;
        av_channel_layout_default(&aenc->ch_layout, 2);
        aenc->bit_rate = 128000;
        aenc->time_base = AVRational{1, SAMPLE_RATE};
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ast->time_base = aenc->time_base;
        ok = avcodec_open2(aenc, soundCodec, nullptr) >= 0 &&
             avcodec_parameters_from_context(ast->codecpar, aenc) >= 0;
    }
    ok = ok && avio_open(&fmt->pb, url.c_str(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(fmt, nullptr) >= 0;

    auto drain = [&](AVCodecContext* enc, AVStream* st) {
        while (avcodec_receive_packet(enc, packet) >= 0) {
            if (enc == venc) packet->duration = 1;  // one frame; MP4 sizes its edit list from it
            av_packet_rescale_ts(packet, enc->time_base, st->time_base);
            packet->stream_index = st->index;
            if (av_interleaved_write_frame(fmt, packet) < 0) return false;
        }
        return true;
    };
    int64_t samplesWritten = 0;
    auto writeSoundUntil = [&](int64_t endSample) {
        const int frameSize = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        while (samplesWritten < endSample) {
            av_frame_unref(sound);
            sound->nb_samples = frameSize;
            sound->format = aenc->sample_fmt;
            sound->sample_rate = SAMPLE_RATE;
            if (av_channel_layout_copy(&sound->ch_layout, &aenc->ch_layout) < 0 ||
                av_frame_get_buffer(sound, 0) < 0) {
                return false;
            }
            for (int s = 0; s < frameSize; ++s) {
                const double v = 0.3 * std::sin(2.0 * PI * 440.0 * double(samplesWritten + s) / SAMPLE_RATE);
                for (int ch = 0; ch < 2; ++ch) {
                    if (aenc->sample_fmt == AV_SAMPLE_FMT_S16) {
                        reinterpret_cast<int16_t*>(sound->data[0])[s * 2 + ch] = static_cast<int16_t>(v * 32767);
                    } else {
                        reinterpret_cast<float*>(sound->data[ch])[s] = static_cast<float>(v);
                    }
                }
            }
            sound->pts = samplesWritten;
            samplesWritten += frameSize;
            if (avcodec_send_frame(aenc, sound) < 0 || !drain(aenc, ast)) return false;
        }
        return true;
    };

    for (int i = 0; ok && i < FRAMES; ++i) {
        ok = av_frame_make_writable(frame) >= 0;
        for (int y = 0; ok && y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                frame->data[0][y * frame->linesize[0] + x] =
                    static_cast<uint8_t>(60 + 120 * (0.5 + 0.5 * std::sin((x + 2 * i) * 0.12 + y * 0.05)));
            }
        }
        for (int y = 0; ok && y < HEIGHT / 2; ++y) {
            for (int x = 0; x < WIDTH / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = static_cast<uint8_t>(128 + 40 * std::sin(y * 0.3 + i * 0.1));
                frame->data[2][y * frame->linesize[2] + x] = static_cast<uint8_t>(128 + 40 * std::cos(x * 0.2));
            }
        }
        frame->pts = i;
        ok = ok && avcodec_send_frame(venc, frame) >= 0 && drain(venc, vst);
        if (ok && aenc) ok = writeSoundUntil(int64_t(i + 1) * SAMPLE_RATE / FPS);
    }
    ok = ok && avcodec_send_frame(venc, nullptr) >= 0 && drain(venc, vst);
    if (ok && aenc) ok = avcodec_send_frame(aenc, nullptr) >= 0 && drain(aenc, ast);
    ok = ok && av_write_trailer(fmt) >= 0;

    if (fmt->pb) avio_closep(&fmt->pb);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&sound);
    avcodec_free_context(&venc);
    avcodec_free_context(&aenc);
    avformat_free_context(fmt);
    return ok;
}

// The source's frames as RGB24, as a one-off decode sees them.
std::vector<std::vector<uint8_t>> sourceFrames(const fs::path& path) {
    std::vector<std::vector<uint8_t>> frames;
    VideoDecoder decoder;
    if (!decoder.open(path)) return frames;
    for (VideoFrame f = decoder.decodeNextFrame(); f.isValid(); f = decoder.decodeNextFrame()) {
        frames.push_back(f.data);
    }
    return frames;
}

// Frame `index` of a HAP file shown by HapFileSource (paused, seeked into the frame),
// decoded to RGBA as the GPU and quad.frag would show it.
struct HapPlayback {
    explicit HapPlayback(const fs::path& path, double rate = FPS) : fps(rate), source(path) {
        source.setTimeSource([this] { return now; });
        opened = source.initialize();
    }

    std::vector<uint8_t> frame(int index) {
        source.seek((index + 0.5) / fps);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            source.update(0.0);
            const FrameData* f = source.getCurrentFrame();
            if (f && std::lround(f->timestamp / 1e6 * fps) == index) {
                const bool q = f->compressedFormat == GpuCompressedFormat::BC3_YCoCg;
                EXPECT_TRUE(q || f->compressedFormat == GpuCompressedFormat::BC1_RGB);
                std::vector<uint8_t> rgba = bctest::decodeBlocks(*f->compressed, f->width, f->height, q);
                if (q) bctest::ycocgToRgb(rgba);
                return rgba;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "HapFileSource never showed frame " << index;
        return {};
    }

    double fps;
    PlaybackTimePoint now = PlaybackTimePoint{} + std::chrono::hours(1);
    HapFileSource source;
    bool opened = false;
};

// Every frame FFmpeg's HAP decoder reads from the file, as packed RGB(A/0).
std::vector<std::vector<uint8_t>> ffmpegHapFrames(const fs::path& path) {
    std::vector<std::vector<uint8_t>> frames;
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, utf8FromPath(path).c_str(), nullptr, nullptr) < 0) return frames;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HAP);
    const int index = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (index >= 0 && dec && avcodec_parameters_to_context(dec, in->streams[index]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0) {
        auto receive = [&] {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                EXPECT_TRUE(frame->format == AV_PIX_FMT_RGB0 || frame->format == AV_PIX_FMT_RGBA)
                    << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
                std::vector<uint8_t> rgba(static_cast<size_t>(frame->width) * frame->height * 4);
                for (int y = 0; y < frame->height; ++y) {
                    std::copy_n(frame->data[0] + y * frame->linesize[0], frame->width * 4,
                                rgba.begin() + static_cast<ptrdiff_t>(y) * frame->width * 4);
                }
                frames.push_back(std::move(rgba));
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(in, packet) >= 0) {
            if (packet->stream_index == index && avcodec_send_packet(dec, packet) >= 0) receive();
            av_packet_unref(packet);
        }
        avcodec_send_packet(dec, nullptr);
        receive();
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec);
    avformat_close_input(&in);
    return frames;
}

struct StreamSummary {
    AVCodecID video = AV_CODEC_ID_NONE;
    uint32_t videoTag = 0;
    int64_t videoFrames = 0;
    double fps = 0.0;
    double videoDuration = 0.0;
    AVCodecID audio = AV_CODEC_ID_NONE;
    int streams = 0;
};

StreamSummary summarize(const fs::path& path) {
    StreamSummary s;
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, utf8FromPath(path).c_str(), nullptr, nullptr) < 0) return s;
    avformat_find_stream_info(in, nullptr);
    s.streams = static_cast<int>(in->nb_streams);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* st = in->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            s.video = st->codecpar->codec_id;
            s.videoTag = st->codecpar->codec_tag;
            s.videoFrames = st->nb_frames;
            s.fps = av_q2d(st->avg_frame_rate);
            s.videoDuration = st->duration * av_q2d(st->time_base);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            s.audio = st->codecpar->codec_id;
        }
    }
    avformat_close_input(&in);
    return s;
}

double minPsnr(const std::vector<std::vector<uint8_t>>& decoded, const std::vector<std::vector<uint8_t>>& source) {
    double worst = 1e9;
    for (size_t i = 0; i < decoded.size() && i < source.size(); ++i) {
        if (decoded[i].size() != size_t(WIDTH) * HEIGHT * 4) return 0.0;
        worst = std::min(worst, bctest::psnr(decoded[i].data(), 4, source[i].data(), 3, WIDTH * HEIGHT));
    }
    return worst;
}

class HapExport : public testing::Test {
protected:
    void SetUp() override {
        folder_ = fs::temp_directory_path() /
                  ("qm_hapexport_" + std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) +
                   "_" + std::to_string(_getpid()));
        std::error_code ec;
        fs::remove_all(folder_, ec);
        fs::create_directories(folder_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(folder_, ec);
    }

    fs::path source(const char* name, AVCodecID audio) {
        const fs::path path = folder_ / name;
        if (!writeSourceClip(path, audio)) return {};
        return path;
    }

    std::vector<fs::path> files() const {
        std::vector<fs::path> out;
        for (const auto& entry : fs::directory_iterator(folder_)) out.push_back(entry.path().filename());
        return out;
    }

    // Convert, check the file's streams, then compare every frame with the source
    // through HapFileSource and FFmpeg's HAP decoder. Returns the playback PSNR.
    double convertAndCompare(const fs::path& src, hap::HapVariant variant, AVCodecID expectedAudio) {
        std::atomic<bool> cancel{false};
        std::vector<double> progress;
        const HapExportResult result = convertToHap(src, hapOutputPath(src, variant), variant, 2, cancel,
                                                    [&](double p) { progress.push_back(p); });
        EXPECT_EQ(result.status, HapExportResult::Status::Done) << result.error;
        EXPECT_EQ(result.frames, FRAMES);
        EXPECT_FALSE(progress.empty());
        if (!progress.empty()) EXPECT_NEAR(progress.back(), 1.0, 1e-6);

        const StreamSummary s = summarize(result.output);
        EXPECT_EQ(s.video, AV_CODEC_ID_HAP);
        EXPECT_EQ(s.videoTag, variant == hap::HapVariant::Hap ? MKTAG('H', 'a', 'p', '1') : MKTAG('H', 'a', 'p', 'Y'));
        EXPECT_EQ(s.videoFrames, FRAMES);
        EXPECT_NEAR(s.fps, FPS, 1e-6);
        EXPECT_NEAR(s.videoDuration, double(FRAMES) / FPS, 1e-6);
        EXPECT_EQ(s.audio, expectedAudio);

        const auto reference = sourceFrames(src);
        EXPECT_EQ(reference.size(), size_t(FRAMES));

        HapPlayback playback(result.output);
        EXPECT_TRUE(playback.opened);
        EXPECT_NEAR(playback.source.getDuration(), double(FRAMES) / FPS, 1e-6);
        std::vector<std::vector<uint8_t>> shown;
        for (int i = 0; playback.opened && i < FRAMES; ++i) shown.push_back(playback.frame(i));
        const double db = minPsnr(shown, reference);

        const auto ffmpeg = ffmpegHapFrames(result.output);
        if (avcodec_find_decoder(AV_CODEC_ID_HAP)) {
            EXPECT_EQ(ffmpeg.size(), size_t(FRAMES));
            const double ffmpegDb = minPsnr(ffmpeg, reference);
            RecordProperty(variant == hap::HapVariant::Hap ? "ffmpeg_hap_psnr_db" : "ffmpeg_hapq_psnr_db",
                           std::to_string(ffmpegDb));
            EXPECT_GE(ffmpegDb, variant == hap::HapVariant::Hap ? HAP_MIN_PSNR : HAPQ_MIN_PSNR)
                << "FFmpeg's HAP decoder reads the file differently";
        }
        RecordProperty(variant == hap::HapVariant::Hap ? "hap_psnr_db" : "hapq_psnr_db", std::to_string(db));
        return db;
    }

    fs::path folder_;
};

}  // namespace

TEST_F(HapExport, HapPreservesFramesRateAndCopiesAacSound) {
    const fs::path src = source("clip.mp4", AV_CODEC_ID_AAC);
    if (src.empty()) GTEST_SKIP() << "FFmpeg cannot write an MPEG-4 + AAC clip";
    EXPECT_GE(convertAndCompare(src, hap::HapVariant::Hap, AV_CODEC_ID_AAC), HAP_MIN_PSNR);
    EXPECT_TRUE(fs::exists(folder_ / "clip_hap.mov"));
}

TEST_F(HapExport, HapQPreservesFramesAndDecodesThroughTheShaderMaths) {
    const fs::path src = source("clip.mp4", AV_CODEC_ID_AAC);
    if (src.empty()) GTEST_SKIP() << "FFmpeg cannot write an MPEG-4 + AAC clip";
    EXPECT_GE(convertAndCompare(src, hap::HapVariant::HapQ, AV_CODEC_ID_AAC), HAPQ_MIN_PSNR);
    EXPECT_TRUE(fs::exists(folder_ / "clip_hapq.mov"));
}

TEST_F(HapExport, SoundAMovCannotHoldIsReencodedToAac) {
    const fs::path src = source("clip.mkv", AV_CODEC_ID_FLAC);
    if (src.empty() || !avcodec_find_encoder(AV_CODEC_ID_AAC)) GTEST_SKIP() << "no FLAC or AAC encoder";
    std::atomic<bool> cancel{false};
    const HapExportResult result =
        convertToHap(src, hapOutputPath(src, hap::HapVariant::Hap), hap::HapVariant::Hap, 2, cancel);
    ASSERT_EQ(result.status, HapExportResult::Status::Done) << result.error;
    EXPECT_NE(result.audioNote.find("re-encoded to AAC"), std::string::npos) << result.audioNote;
    const StreamSummary s = summarize(result.output);
    EXPECT_EQ(s.audio, AV_CODEC_ID_AAC);
    EXPECT_EQ(s.videoFrames, FRAMES);
}

TEST_F(HapExport, SilentSourceGivesSilentFile) {
    const fs::path src = source("silent.mp4", AV_CODEC_ID_NONE);
    if (src.empty()) GTEST_SKIP() << "FFmpeg cannot write an MPEG-4 clip";
    std::atomic<bool> cancel{false};
    const HapExportResult result =
        convertToHap(src, hapOutputPath(src, hap::HapVariant::Hap), hap::HapVariant::Hap, 1, cancel);
    ASSERT_EQ(result.status, HapExportResult::Status::Done) << result.error;
    EXPECT_TRUE(result.audioNote.empty());
    EXPECT_EQ(summarize(result.output).streams, 1);
}

TEST_F(HapExport, CancelLeavesNoPartialFile) {
    const fs::path src = source("clip.mp4", AV_CODEC_ID_AAC);
    if (src.empty()) GTEST_SKIP() << "FFmpeg cannot write an MPEG-4 + AAC clip";
    std::atomic<bool> cancel{false};
    const HapExportResult result = convertToHap(src, hapOutputPath(src, hap::HapVariant::HapQ),
                                                hap::HapVariant::HapQ, 1, cancel, [&](double) { cancel = true; });
    EXPECT_EQ(result.status, HapExportResult::Status::Cancelled);
    EXPECT_EQ(result.frames, 1);  // stopped at the next frame
    EXPECT_EQ(files(), std::vector<fs::path>{"clip.mp4"});
}

TEST_F(HapExport, UnreadableSourceFailsWithoutLeavingAFile) {
    const fs::path src = folder_ / "not a video.mp4";
    ASSERT_TRUE(FileSystem::writeFile(src, "this is text").isSuccess());
    std::atomic<bool> cancel{false};
    const HapExportResult result =
        convertToHap(src, hapOutputPath(src, hap::HapVariant::Hap), hap::HapVariant::Hap, 1, cancel);
    EXPECT_EQ(result.status, HapExportResult::Status::Failed);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(files(), std::vector<fs::path>{"not a video.mp4"});
}

TEST_F(HapExport, NonAsciiFolderAndName) {
    const fs::path inner = folder_ / pathFromUtf8(u8"\u00fc \u65e5\u672c \u0416");
    std::error_code ec;
    fs::create_directories(inner, ec);
    const fs::path src = inner / pathFromUtf8(u8"\u0416\u00e9.mp4");
    if (!writeSourceClip(src, AV_CODEC_ID_NONE)) GTEST_SKIP() << "FFmpeg cannot write an MPEG-4 clip";
    std::atomic<bool> cancel{false};
    const HapExportResult result =
        convertToHap(src, hapOutputPath(src, hap::HapVariant::Hap), hap::HapVariant::Hap, 1, cancel);
    ASSERT_EQ(result.status, HapExportResult::Status::Done) << result.error;
    EXPECT_EQ(utf8FromPath(result.output.filename()), u8"\u0416\u00e9_hap.mov");
    HapFileSource played(result.output);
    EXPECT_TRUE(played.initialize());
}

// The worker thread class the UI polls; a second conversion of the same clip gets
// the next free name instead of overwriting the first.
TEST_F(HapExport, BackgroundConversionTakesTheNextFreeName) {
    const fs::path src = source("clip.mp4", AV_CODEC_ID_NONE);
    if (src.empty()) GTEST_SKIP() << "FFmpeg cannot write an MPEG-4 clip";
    for (const char* expected : {"clip_hapq.mov", "clip_hapq 2.mov"}) {
        HapConversion job(src, hap::HapVariant::HapQ);
        EXPECT_EQ(job.plannedOutput().filename(), fs::path(expected));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!job.finished() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(job.finished());
        EXPECT_EQ(job.result().status, HapExportResult::Status::Done) << job.result().error;
        EXPECT_EQ(job.result().output, job.plannedOutput());
        EXPECT_DOUBLE_EQ(job.progress(), 1.0);
    }
}

// Opt-in: converts D:\testing Videos\5sec.mp4 both ways and plays the results back
// headless (first, middle and last frame through HapFileSource), printing size and
// time. Run with
//   test_video_decoder --gtest_also_run_disabled_tests --gtest_filter=*RealFile*
TEST(HapExportRealMedia, DISABLED_RealFile) {
    const fs::path src = "D:\\testing Videos\\5sec.mp4";
    std::error_code ec;
    if (!fs::exists(src, ec)) GTEST_SKIP() << "test media not present";
    const fs::path folder = fs::temp_directory_path() / "qm_hapexport_real";
    fs::create_directories(folder, ec);

    VideoDecoder probe;
    ASSERT_TRUE(probe.open(src));
    const VideoInfo info = probe.getInfo();
    probe.close();
    const auto srcBytes = fs::file_size(src, ec);
    std::printf("source: %dx%d %.3f fps %.3f s, %s, %.1f MB\n", info.width, info.height, info.frameRate,
                info.duration, info.codecName.c_str(), srcBytes / 1e6);

    for (const auto variant : {hap::HapVariant::Hap, hap::HapVariant::HapQ}) {
        fs::path name = src.stem();
        name += variant == hap::HapVariant::Hap ? "_hap.mov" : "_hapq.mov";
        std::atomic<bool> cancel{false};
        const auto start = std::chrono::steady_clock::now();
        const HapExportResult result =
            convertToHap(src, folder / name, variant, HapConversion::workerThreads(), cancel);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ASSERT_EQ(result.status, HapExportResult::Status::Done) << result.error;

        HapPlayback played(result.output, info.frameRate);
        ASSERT_TRUE(played.opened);
        for (const int64_t frame : {int64_t(0), result.frames / 2, result.frames - 1}) {
            EXPECT_EQ(played.frame(static_cast<int>(frame)).size(), size_t(info.width) * info.height * 4)
                << "frame " << frame;
        }
        const StreamSummary s = summarize(result.output);
        std::printf("%s: %lld frames in %.1f s (%.1f fps, %d threads), %.1f MB, %.3f fps, %.3f s, played %.3f s. %s\n",
                    utf8FromPath(result.output.filename()).c_str(), static_cast<long long>(result.frames), seconds,
                    result.frames / seconds, HapConversion::workerThreads(),
                    fs::file_size(result.output, ec) / 1e6, s.fps, s.videoDuration, played.source.getDuration(),
                    result.audioNote.c_str());
        EXPECT_EQ(s.video, AV_CODEC_ID_HAP);
        EXPECT_NEAR(s.fps, info.frameRate, 0.01);
        EXPECT_NEAR(played.source.getDuration(), info.duration, 0.1);
    }
    fs::remove_all(folder, ec);
}
