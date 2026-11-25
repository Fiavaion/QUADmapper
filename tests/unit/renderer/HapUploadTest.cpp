// GPU upload test for the HAP BCn compressed-texture path (ADR-044, QM-080 A3).
//
// Verifies the real A3 consumer path issues valid GL calls:
//   1. Texture::createCompressed / updateCompressed for BC1/BC3/BC3_YCoCg produce
//      a valid texture, set the right colorspace, and raise no GL error.
//   2. GpuCapabilities::queryOnce() reports s3tc/bptc for the running context.
//   3. End-to-end: if the real Hap Q clip and s3tc are present, drive HapFileSource
//      to stage a compressed FrameData and upload it through the same code path
//      SourceManager uses — no GL error, non-zero texture, YCoCg colorspace.
//
// Needs a real GL 3.3 context; SKIPs where none is available (headless CI),
// matching the repo's hardware-absent convention (see CropRenderTest).

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "renderer/Texture.hpp"
#include "renderer/CompressedUpload.hpp"
#include "renderer/GpuCapabilities.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/ShaderProgram.hpp"
#include "ShaderSources.hpp"      // generated: shaders::QUAD_VERT / QUAD_FRAG
#include "scene/Quad.hpp"
#include "source/FrameData.hpp"
#include "source/HapFileSource.hpp"
#include "video/PngWriter.hpp"    // writeRgbPng
#include "../TestMedia.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

using namespace quadmapper;

namespace {

// Hidden-window GL context, RAII. ok == false means no GL is available.
struct GLContext {
    GLFWwindow* win = nullptr;
    bool ok = false;
    GLContext() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "hap-upload-test", nullptr, nullptr);
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

// One BC3 (DXT5) block is 16 bytes for a 4x4 texel area; BC1 (DXT1) is 8 bytes.
size_t blockBytes(int w, int h, bool dxt1) {
    const size_t blocksX = static_cast<size_t>((w + 3) / 4);
    const size_t blocksY = static_cast<size_t>((h + 3) / 4);
    return blocksX * blocksY * (dxt1 ? 8u : 16u);
}

} // namespace

// --- 1/2: synthetic blobs + capability query --------------------------------

TEST(HapUploadTest, SyntheticCompressedUploadNoGlError) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    if (!caps.s3tc) GTEST_SKIP() << "GPU lacks GL_EXT_texture_compression_s3tc";

    // Drain any prior error.
    while (glGetError() != GL_NO_ERROR) {}

    const int W = 64, H = 64;

    struct Case { GpuCompressedFormat fmt; bool dxt1; TextureColorSpace cs; };
    const Case cases[] = {
        { GpuCompressedFormat::BC1_RGB,   true,  TextureColorSpace::RGB   },
        { GpuCompressedFormat::BC3_RGBA,  false, TextureColorSpace::RGB   },
        { GpuCompressedFormat::BC3_YCoCg, false, TextureColorSpace::YCoCg },
    };

    for (const auto& c : cases) {
        std::vector<unsigned char> blob(blockBytes(W, H, c.dxt1), 0xAB);

        Texture tex;
        ASSERT_TRUE(tex.createCompressed(W, H, c.fmt, blob.data(), blob.size()));
        EXPECT_TRUE(tex.isValid());
        EXPECT_TRUE(tex.isCompressed());
        EXPECT_EQ(tex.compressedFormat(), c.fmt);
        EXPECT_EQ(tex.colorSpace(), c.cs);
        EXPECT_EQ(tex.getWidth(), W);
        EXPECT_EQ(tex.getHeight(), H);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
            << "createCompressed raised a GL error for format "
            << static_cast<int>(c.fmt);

        // Re-upload the same level (the per-frame path).
        EXPECT_TRUE(tex.updateCompressed(blob.data(), blob.size()));
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
            << "updateCompressed raised a GL error for format "
            << static_cast<int>(c.fmt);
    }
}

// A level of the wrong size is refused with nothing uploaded; the right size still works.
TEST(HapUploadTest, CompressedUploadOfTheWrongSizeIsRefused) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    if (!caps.s3tc) GTEST_SKIP() << "GPU lacks GL_EXT_texture_compression_s3tc";
    while (glGetError() != GL_NO_ERROR) {}

    const int W = 64, H = 32;
    const size_t bytes = blockBytes(W, H, /*dxt1=*/false);
    EXPECT_EQ(Texture::compressedByteSize(GpuCompressedFormat::BC3_YCoCg, W, H), bytes);
    EXPECT_EQ(Texture::compressedByteSize(GpuCompressedFormat::BC1_RGB, W, H), blockBytes(W, H, /*dxt1=*/true));
    EXPECT_EQ(Texture::compressedByteSize(GpuCompressedFormat::BC1_RGB, 5, 5), 4u * 8u);

    std::vector<unsigned char> blob(bytes, 0x11);
    Texture tex;
    EXPECT_FALSE(tex.createCompressed(W, H, GpuCompressedFormat::BC3_RGBA, blob.data(), bytes - 16));
    EXPECT_FALSE(tex.isValid());
    ASSERT_TRUE(tex.createCompressed(W, H, GpuCompressedFormat::BC3_RGBA, blob.data(), bytes));
    EXPECT_FALSE(tex.updateCompressed(blob.data(), bytes - 16));
    EXPECT_FALSE(tex.updateCompressed(blob.data(), bytes + 16));
    EXPECT_TRUE(tex.updateCompressed(blob.data(), bytes));
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

// TD-31: frames go up through the source's pixel buffers, copied there off the main
// thread. Each texture holds exactly the frame uploaded, whether prefetch() staged it
// (the next frame, as in steady playback) or not (a seek or a dropped frame), and no
// unpack buffer is left bound for later client-memory uploads.
TEST(HapUploadTest, PixelBufferUploadsMatchTheFrameWithAndWithoutPrefetch) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    if (!caps.s3tc) GTEST_SKIP() << "GPU lacks GL_EXT_texture_compression_s3tc";
    while (glGetError() != GL_NO_ERROR) {}

    const int W = 64, H = 32;
    const size_t bytes = blockBytes(W, H, /*dxt1=*/false);
    auto frame = [bytes](int seed) {
        auto blob = std::make_shared<std::vector<uint8_t>>(bytes);
        for (size_t i = 0; i < bytes; ++i) (*blob)[i] = static_cast<uint8_t>(i * 7 + seed * 31);
        return std::shared_ptr<const std::vector<uint8_t>>(blob);
    };
    auto readBack = [bytes](const Texture& tex) {
        std::vector<uint8_t> out(bytes);
        tex.bind(0);
        glGetCompressedTexImage(GL_TEXTURE_2D, 0, out.data());
        Texture::unbind();
        return out;
    };

    Texture tex;
    ASSERT_TRUE(tex.createCompressed(W, H, GpuCompressedFormat::BC3_RGBA, nullptr, bytes));
    CompressedUploader uploader;
    CompressedUploadState state;
    const auto f1 = frame(1), f2 = frame(2), f3 = frame(3), f4 = frame(4);

    ASSERT_TRUE(uploader.upload(f1, tex, state));  // nothing staged yet
    EXPECT_EQ(readBack(tex), *f1);
    uploader.prefetch(f2, state);
    ASSERT_TRUE(uploader.upload(f2, tex, state));  // the staged frame
    EXPECT_EQ(readBack(tex), *f2);
    uploader.prefetch(f3, state);
    ASSERT_TRUE(uploader.upload(f4, tex, state));  // not the staged one: f3 was skipped
    EXPECT_EQ(readBack(tex), *f4);

    GLint unpack = -1;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    EXPECT_EQ(unpack, 0);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    // The uploader keeps no frame alive once it is uploaded.
    EXPECT_EQ(f1.use_count(), 1);
    EXPECT_EQ(f2.use_count(), 1);
    EXPECT_EQ(f4.use_count(), 1);
}

// TD-35: as YuvUploadState::idle(): never waits; false while the state's copy runs, true
// once it is done; a copy queued behind another state's is dropped and never runs.
TEST(HapUploadTest, IdleNeverWaitsForACopyInFlightAndDropsAQueuedOne) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    if (!caps.s3tc) GTEST_SKIP() << "GPU lacks GL_EXT_texture_compression_s3tc";
    while (glGetError() != GL_NO_ERROR) {}

    const int W = 64, H = 32;
    const size_t bytes = blockBytes(W, H, /*dxt1=*/false);
    auto frame = [bytes](int seed) {
        auto blob = std::make_shared<std::vector<uint8_t>>(bytes);
        for (size_t i = 0; i < bytes; ++i) (*blob)[i] = static_cast<uint8_t>(i * 7 + seed * 31);
        return std::shared_ptr<const std::vector<uint8_t>>(blob);
    };
    std::atomic<int> copiesStarted{0};
    std::atomic<bool> release{true};
    CompressedUploader::setCopyHookForTesting([&] {
        ++copiesStarted;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    {
        Texture tex;
        ASSERT_TRUE(tex.createCompressed(W, H, GpuCompressedFormat::BC3_RGBA, nullptr, bytes));
        CompressedUploader uploader;
        auto running = std::make_unique<CompressedUploadState>();
        auto queued = std::make_unique<CompressedUploadState>();
        const auto f1 = frame(1), f2 = frame(2);
        ASSERT_TRUE(uploader.upload(f1, tex, *running));
        ASSERT_TRUE(uploader.upload(f1, tex, *queued));
        EXPECT_TRUE(running->idle());  // nothing staged

        copiesStarted = 0;
        release = false;
        uploader.prefetch(f2, *running);  // starts, and blocks in the hook
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (copiesStarted == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(copiesStarted, 1);
        uploader.prefetch(f2, *queued);  // queued behind it

        const auto asked = std::chrono::steady_clock::now();
        EXPECT_FALSE(running->idle()) << "its copy is running";
        EXPECT_TRUE(queued->idle()) << "a queued copy is dropped";
        EXPECT_LT(std::chrono::steady_clock::now() - asked, std::chrono::milliseconds(100)) << "idle() waited";
        queued.reset();

        release = true;
        while (!running->idle() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(running->idle());
        running.reset();
        EXPECT_EQ(copiesStarted, 1) << "the dropped copy ran anyway";
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
    CompressedUploader::setCopyHookForTesting(nullptr);
}

TEST(HapUploadTest, CapabilityQueryReportsExtensions) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    // Not asserting true (hardware-dependent) — just that the gate is consistent.
    EXPECT_EQ(caps.supports(GpuCompressedFormat::BC1_RGB),   caps.s3tc);
    EXPECT_EQ(caps.supports(GpuCompressedFormat::BC3_RGBA),  caps.s3tc);
    EXPECT_EQ(caps.supports(GpuCompressedFormat::BC3_YCoCg), caps.s3tc);
    EXPECT_EQ(caps.supports(GpuCompressedFormat::BC7_RGBA),  caps.bptc);
    EXPECT_FALSE(caps.supports(GpuCompressedFormat::None));
    SUCCEED() << "s3tc=" << caps.s3tc << " bptc=" << caps.bptc;
}

// --- 3: end-to-end via the real Hap Q clip ----------------------------------

TEST(HapUploadTest, RealHapClipUploadsCompressedTexture) {
    const std::filesystem::path clip = testMediaFile("Hap/5sec_hap.mov");
    if (!std::filesystem::exists(clip)) {
        GTEST_SKIP() << "Hap test clip not present: " << clip.string();
    }

    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    if (!caps.s3tc) GTEST_SKIP() << "GPU lacks s3tc — Hap Q would be skipped (by design)";

    HapFileSource src(clip);
    ASSERT_TRUE(src.initialize()) << "HapFileSource failed to open the clip";
    src.play();

    // Pump the source until a compressed frame is staged (background demux+parse).
    const FrameData* frame = nullptr;
    for (int i = 0; i < 600 && !frame; ++i) {
        src.update(1.0 / 60.0);
        const FrameData* f = src.getCurrentFrame();
        if (f && f->isValid() && f->isCompressed()) {
            frame = f;
            break;
        }
        glfwWaitEventsTimeout(0.005);  // let the demux thread make progress
    }
    ASSERT_NE(frame, nullptr) << "No compressed HAP frame was staged in time";

    EXPECT_GT(frame->width, 0);
    EXPECT_GT(frame->height, 0);
    ASSERT_TRUE(frame->compressed);
    EXPECT_FALSE(frame->compressed->empty());
    // 5sec_hap.mov is Hap Q -> BC3_YCoCg.
    EXPECT_EQ(frame->compressedFormat, GpuCompressedFormat::BC3_YCoCg);

    while (glGetError() != GL_NO_ERROR) {}

    Texture tex;
    ASSERT_TRUE(tex.createCompressed(frame->width, frame->height,
                                     frame->compressedFormat,
                                     frame->compressed->data(), frame->compressed->size()));
    EXPECT_NE(tex.getId(), 0u);
    EXPECT_TRUE(tex.isCompressed());
    EXPECT_EQ(tex.colorSpace(), TextureColorSpace::YCoCg);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
        << "Uploading the real HAP BCn frame raised a GL error";

    src.shutdown();
}

// --- A5: end-to-end visual capture through the REAL render path -------------
//
// Renders the real Hap Q YCoCg texture full-frame into an offscreen FBO with the
// production quad shader, reads the pixels back, writes a PNG, and prints
// objective stats. Two captures:
//   * a5_hapq_shader.png — via the real Renderer::renderQuad (u_ColorSpace driven
//     from texture->colorSpace() == YCoCg -> 1). The proven natural-color result.
//   * a5_hapq_raw.png    — the SAME texture with the shader bound directly and
//     u_ColorSpace forced to 0 (BC3-as-RGB). The "wrong colors" control baseline.

namespace {

struct PixelStats {
    double meanR = 0, meanG = 0, meanB = 0;
};

// Read an FBO's RGBA8 color buffer back into a tightly-packed, top-row-first RGB24
// buffer (GL reads bottom-up, so rows are flipped for a viewable PNG). Also fills
// the mean-RGB stats over the full frame.
std::vector<unsigned char> readbackRgb(int w, int h, PixelStats& stats) {
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3, 0);
    double sumR = 0, sumG = 0, sumB = 0;
    for (int y = 0; y < h; ++y) {
        const int srcRow = h - 1 - y;  // flip V (GL origin is bottom-left)
        for (int x = 0; x < w; ++x) {
            const size_t s = (static_cast<size_t>(srcRow) * w + x) * 4;
            const size_t d = (static_cast<size_t>(y) * w + x) * 3;
            rgb[d + 0] = rgba[s + 0];
            rgb[d + 1] = rgba[s + 1];
            rgb[d + 2] = rgba[s + 2];
            sumR += rgba[s + 0];
            sumG += rgba[s + 1];
            sumB += rgba[s + 2];
        }
    }
    const double n = static_cast<double>(w) * h;
    stats.meanR = sumR / n;
    stats.meanG = sumG / n;
    stats.meanB = sumB / n;
    return rgb;
}

// Print mean RGB plus 5 fixed sample points (centre + 4 quarter points), reading
// the top-row-first RGB24 buffer produced by readbackRgb.
void reportStats(const char* label, const std::vector<unsigned char>& rgb,
                 int w, int h, const PixelStats& s) {
    auto px = [&](int x, int y, int& r, int& g, int& b) {
        const size_t i = (static_cast<size_t>(y) * w + x) * 3;
        r = rgb[i]; g = rgb[i + 1]; b = rgb[i + 2];
    };
    // Fixed sample points: centre, the 4 quarters, plus a horizontal sweep across
    // the vertical mid-line so a frame with content (e.g. timecode text on black)
    // shows real spatial variation rather than only background.
    std::array<std::pair<int, int>, 11> pts = {{
        {w / 2, h / 2}, {w / 4, h / 4}, {3 * w / 4, h / 4},
        {w / 4, 3 * h / 4}, {3 * w / 4, 3 * h / 4},
        {w / 12, h / 2}, {w / 6, h / 2}, {w / 3, h / 2},
        {w / 2 + w / 12, h / 2}, {2 * w / 3, h / 2}, {5 * w / 6, h / 2},
    }};
    // Per-channel min/max over the whole frame => objective "is it flat?" evidence.
    int rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        rmin = std::min<int>(rmin, rgb[i]);     rmax = std::max<int>(rmax, rgb[i]);
        gmin = std::min<int>(gmin, rgb[i + 1]); gmax = std::max<int>(gmax, rgb[i + 1]);
        bmin = std::min<int>(bmin, rgb[i + 2]); bmax = std::max<int>(bmax, rgb[i + 2]);
    }
    std::printf("[A5] %s mean RGB = (%.1f, %.1f, %.1f)  "
                "range R[%d..%d] G[%d..%d] B[%d..%d]\n",
                label, s.meanR, s.meanG, s.meanB,
                rmin, rmax, gmin, gmax, bmin, bmax);
    for (const auto& p : pts) {
        int r, g, b;
        px(p.first, p.second, r, g, b);
        std::printf("[A5] %s pixel (%4d,%4d) = (%3d, %3d, %3d)\n",
                    label, p.first, p.second, r, g, b);
    }
    std::fflush(stdout);
}

// Set the full non-colorspace uniform identity (mirrors Renderer::renderQuad) on a
// directly-bound program, used for the raw control where u_ColorSpace is forced 0.
void setIdentityUniforms(ShaderProgram& prog) {
    prog.setInt("u_Texture", 0);
    prog.setFloat("u_Opacity", 1.0f);
    prog.setVec2("u_UVOffset", glm::vec2(0.0f));
    prog.setVec2("u_UVScale", glm::vec2(1.0f));
    prog.setFloat("u_UVRotation", 0.0f);
    prog.setInt("u_FlipH", 0);
    prog.setInt("u_FlipV", 0);
    prog.setVec3("u_ColorRGBGain", glm::vec3(1.0f));
    prog.setFloat("u_ColorBrightness", 0.0f);
    prog.setFloat("u_ColorContrast", 1.0f);
    prog.setFloat("u_ColorGamma", 1.0f);
    prog.setFloat("u_EdgeLeft", 0.0f);
    prog.setFloat("u_EdgeRight", 0.0f);
    prog.setFloat("u_EdgeTop", 0.0f);
    prog.setFloat("u_EdgeBottom", 0.0f);
    prog.setFloat("u_EdgeGamma", 1.0f);
    prog.setVec2("u_CropOffset", glm::vec2(0.0f));
    prog.setVec2("u_CropSize", glm::vec2(1.0f));
    prog.setVec2("u_ViewScale", glm::vec2(1.0f, 1.0f));
    prog.setVec2("u_ViewOffset", glm::vec2(0.0f, 0.0f));
}

}  // namespace

TEST(HapUploadTest, A5VisualCaptureThroughRealPipeline) {
    const std::filesystem::path clip = testMediaFile("Hap/5sec_hap.mov");
    if (!std::filesystem::exists(clip)) {
        GTEST_SKIP() << "Hap test clip not present: " << clip.string();
    }

    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    auto& caps = GpuCapabilities::get();
    caps.queryOnce();
    if (!caps.s3tc) GTEST_SKIP() << "GPU lacks s3tc — Hap Q would be skipped (by design)";

    // 1) Drive the real source ~1s in so we capture a content frame, not frame 0.
    HapFileSource src(clip);
    ASSERT_TRUE(src.initialize());
    src.play();

    const FrameData* frame = nullptr;
    for (int i = 0; i < 600; ++i) {
        src.update(1.0 / 60.0);
        const FrameData* f = src.getCurrentFrame();
        if (f && f->isValid() && f->isCompressed()) {
            frame = f;
            if (i >= 60) break;  // ~1s of advancement before we grab the frame
        }
        glfwWaitEventsTimeout(0.005);
    }
    ASSERT_NE(frame, nullptr) << "No compressed HAP frame staged in time";
    ASSERT_EQ(frame->compressedFormat, GpuCompressedFormat::BC3_YCoCg);

    // 2) Real upload path produces the YCoCg texture.
    while (glGetError() != GL_NO_ERROR) {}
    Texture tex;
    ASSERT_TRUE(tex.createCompressed(frame->width, frame->height,
                                     frame->compressedFormat,
                                     frame->compressed->data(), frame->compressed->size()));
    ASSERT_EQ(tex.colorSpace(), TextureColorSpace::YCoCg);
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    const int CW = 480, CH = 270;
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(CW, CH));

    // Shared full-frame quad geometry + VAO for the raw control path.
    // Position in [0,1], projective UV (u, v, q) with q = 1 (quad.vert contract).
    const float verts[] = {
        0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
        1.0f, 0.0f,  1.0f, 0.0f, 1.0f,
        1.0f, 1.0f,  1.0f, 1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f, 1.0f,
    };
    const unsigned int idx[] = {0, 1, 2, 0, 2, 3};

    // --- Capture A: the REAL Renderer::renderQuad (u_ColorSpace from texture). ---
    {
        Renderer renderer;
        ASSERT_TRUE(renderer.initialize());

        // Full-frame quad so the whole FBO is video (default quad is only 40%).
        Quad quad;
        std::array<Vec2, 4> full = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
        quad.getGeometry().setCorners(full);

        fbo.bind();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.renderQuad(quad, &tex);   // sets u_ColorSpace=1 from YCoCg
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        PixelStats stats;
        auto rgb = readbackRgb(CW, CH, stats);
        const std::filesystem::path out = R"(c:\tmp\a5_hapq_shader.png)";
        EXPECT_TRUE(writeRgbPng(rgb.data(), CW, CH, out)) << "PNG write failed: " << out;
        reportStats("SHADER(cs=1)", rgb, CW, CH, stats);

        renderer.shutdown();
    }

    // --- Capture B: the SAME texture, shader bound directly, u_ColorSpace=0. ---
    {
        GLuint vao = 0, vbo = 0, ebo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        ShaderProgram prog;
        ASSERT_TRUE(prog.create(shaders::QUAD_VERT, shaders::QUAD_FRAG));
        prog.bind();
        setIdentityUniforms(prog);
        prog.setInt("u_ColorSpace", 0);  // force raw BC3-as-RGB (the wrong path)

        fbo.bind();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        tex.bind(0);
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        PixelStats stats;
        auto rgb = readbackRgb(CW, CH, stats);
        const std::filesystem::path out = R"(c:\tmp\a5_hapq_raw.png)";
        EXPECT_TRUE(writeRgbPng(rgb.data(), CW, CH, out)) << "PNG write failed: " << out;
        reportStats("RAW(cs=0)   ", rgb, CW, CH, stats);

        glBindVertexArray(0);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ebo);
        glDeleteVertexArrays(1, &vao);
    }

    Framebuffer::unbind();
    src.shutdown();
}
