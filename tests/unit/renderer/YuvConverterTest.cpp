// The GPU YUV -> RGBA pass (ADR-055): colour bars built with each matrix (BT.601, 709,
// 2020), range (limited, full) and plane layout (NV12, P010, YUV420P, YUV420P10) come
// back as the reference RGB within +-2 per channel; padded rows upload correctly; the
// pass restores the GL state it changes and frees every GL object it made.
//
// Needs a real GL 3.3 context; SKIPs where none is available (headless CI).

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "../video/ColourBars.hpp"
#include "core/GlObjectStats.hpp"
#include "renderer/Texture.hpp"
#include "renderer/YuvConverter.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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
        win = glfwCreateWindow(64, 64, "yuv-test", nullptr, nullptr);
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

constexpr int BAR_WIDTH = 16;  // even, so no 2x2 chroma block straddles two bars
constexpr int HEIGHT = 8;
constexpr int WIDTH = BAR_WIDTH * static_cast<int>(BARS.size());

// A bars picture in `layout`, rows padded by `padding` bytes, backed by its own memory.
struct BarsPicture {
    std::vector<uint8_t> planes[3];
    YuvPicture picture;
};

std::unique_ptr<BarsPicture> makeBars(YuvLayout layout, YuvMatrix matrix, bool fullRange, int padding) {
    auto bars = std::make_unique<BarsPicture>();
    YuvPicture& pic = bars->picture;
    pic.layout = layout;
    pic.width = WIDTH;
    pic.height = HEIGHT;
    pic.matrix = matrix;
    pic.fullRange = fullRange;
    const int bits = pic.bytesPerSample() == 2 ? 10 : 8;

    auto store = [&](std::vector<uint8_t>& plane, int stride, int x, int y, int value) {
        uint8_t* at = plane.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * pic.bytesPerSample();
        if (pic.bytesPerSample() == 1) {
            *at = static_cast<uint8_t>(value);
        } else {
            const uint16_t v = layout == YuvLayout::P010 ? static_cast<uint16_t>(value << 6)
                                                         : static_cast<uint16_t>(value);
            std::memcpy(at, &v, 2);
        }
    };

    for (int p = 0; p < pic.planeCount(); ++p) {
        const int w = p == 0 ? WIDTH : pic.chromaWidth() * (pic.semiPlanar() ? 2 : 1);
        const int h = p == 0 ? HEIGHT : pic.chromaHeight();
        pic.strides[p] = w * pic.bytesPerSample() + padding;
        bars->planes[p].assign(static_cast<size_t>(pic.strides[p]) * h, 0xEE);  // padding: garbage
        pic.planes[p] = bars->planes[p].data();
    }
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const Yuv c = toYuv(BARS[static_cast<size_t>(x / BAR_WIDTH)], matrix, fullRange, bits);
            store(bars->planes[0], pic.strides[0], x, y, c.y);
            if (x % 2 || y % 2) continue;
            if (pic.semiPlanar()) {
                store(bars->planes[1], pic.strides[1], x, y / 2, c.cb);  // x = 2 * (x / 2)
                store(bars->planes[1], pic.strides[1], x + 1, y / 2, c.cr);
            } else {
                store(bars->planes[1], pic.strides[1], x / 2, y / 2, c.cb);
                store(bars->planes[2], pic.strides[2], x / 2, y / 2, c.cr);
            }
        }
    }
    return bars;
}

std::vector<uint8_t> readBack(const Texture& texture) {
    std::vector<uint8_t> rgba(static_cast<size_t>(texture.getWidth()) * texture.getHeight() * 4);
    glBindTexture(GL_TEXTURE_2D, texture.getId());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return rgba;
}

const char* name(YuvLayout layout) {
    switch (layout) {
        case YuvLayout::NV12: return "NV12";
        case YuvLayout::P010: return "P010";
        case YuvLayout::YUV420P: return "YUV420P";
        case YuvLayout::YUV420P10: return "YUV420P10";
        default: return "?";
    }
}

const char* name(YuvMatrix matrix) {
    return matrix == YuvMatrix::BT601 ? "BT.601" : matrix == YuvMatrix::BT709 ? "BT.709" : "BT.2020";
}

}  // namespace

TEST(YuvConverter, ColourBarsMatchReferenceForEveryMatrixRangeAndLayout) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    YuvConverter converter;
    YuvUploadState state;
    Texture target;
    ASSERT_TRUE(target.createEmpty(WIDTH, HEIGHT, 4));

    int worst = 0;
    for (YuvLayout layout : {YuvLayout::NV12, YuvLayout::P010, YuvLayout::YUV420P, YuvLayout::YUV420P10}) {
        for (YuvMatrix matrix : {YuvMatrix::BT601, YuvMatrix::BT709, YuvMatrix::BT2020}) {
            for (bool fullRange : {false, true}) {
                for (int padding : {0, 24}) {
                    const auto bars = makeBars(layout, matrix, fullRange, padding);
                    ASSERT_TRUE(converter.convert(bars->picture, target, state));
                    const std::vector<uint8_t> rgba = readBack(target);
                    for (size_t i = 0; i < BARS.size(); ++i) {
                        const size_t x = i * BAR_WIDTH + BAR_WIDTH / 2;
                        const uint8_t* px = &rgba[((HEIGHT / 2) * static_cast<size_t>(WIDTH) + x) * 4];
                        const Rgb got{px[0], px[1], px[2]};
                        const Rgb want = BARS[i];
                        worst = std::max({worst, std::abs(got.r - want.r), std::abs(got.g - want.g),
                                          std::abs(got.b - want.b)});
                        EXPECT_TRUE(within(got, want, 2))
                            << name(layout) << " " << name(matrix) << (fullRange ? " full" : " limited")
                            << " padding " << padding << " bar " << i << ": got (" << got.r << ","
                            << got.g << "," << got.b << ") want (" << want.r << "," << want.g << ","
                            << want.b << ")";
                        EXPECT_EQ(px[3], 255);
                    }
                }
            }
        }
    }
    RecordProperty("worst_channel_error", worst);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST(YuvConverter, RestoresGlStateAndFreesItsObjects) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";
    const auto baseline = GlObjectStats::Snapshot::capture();
    {
        Texture target;
        ASSERT_TRUE(target.createEmpty(WIDTH, HEIGHT, 4));
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(3, 4, 50, 40);
        glEnable(GL_BLEND);
        glEnable(GL_SCISSOR_TEST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glActiveTexture(GL_TEXTURE3);

        {
            YuvConverter converter;
            YuvUploadState state;
            const auto bars = makeBars(YuvLayout::NV12, YuvMatrix::BT709, false, 0);
            converter.begin();
            ASSERT_TRUE(converter.convert(bars->picture, target, state));
            ASSERT_TRUE(converter.convert(bars->picture, target, state));  // state reused
            converter.end();

            GLint value = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
            EXPECT_EQ(static_cast<GLuint>(value), fbo);
            GLint viewport[4] = {};
            glGetIntegerv(GL_VIEWPORT, viewport);
            EXPECT_EQ(viewport[0], 3);
            EXPECT_EQ(viewport[1], 4);
            EXPECT_EQ(viewport[2], 50);
            EXPECT_EQ(viewport[3], 40);
            EXPECT_TRUE(glIsEnabled(GL_BLEND));
            EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &value);
            EXPECT_EQ(value, 4);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
            EXPECT_EQ(value, GL_TEXTURE3);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &value);
            EXPECT_EQ(value, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
    }
    const auto after = GlObjectStats::Snapshot::capture();
    EXPECT_TRUE(after.isFlat(baseline)) << after.diff(baseline);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

// prefetch() copies the next frame on the copier thread; convert() uses it when that
// frame comes, and copies anew when another does (a seek, a dropped frame). A state
// destroyed with a copy in flight waits for it and unmaps.
TEST(YuvConverter, PrefetchedFrameIsUsedAndAMismatchIsCopiedAnew) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";
    const auto baseline = GlObjectStats::Snapshot::capture();
    {
        YuvConverter converter;
        Texture target;
        ASSERT_TRUE(target.createEmpty(WIDTH, HEIGHT, 4));
        // Three frames told apart by matrix and range; each owns its planes as a decoder's would.
        auto frame = [](YuvMatrix matrix, bool fullRange) {
            std::shared_ptr<BarsPicture> bars = makeBars(YuvLayout::NV12, matrix, fullRange, 8);
            YuvPicture picture = bars->picture;
            picture.owner = bars;
            return picture;
        };
        const YuvPicture a = frame(YuvMatrix::BT709, false);
        const YuvPicture b = frame(YuvMatrix::BT601, true);
        const YuvPicture c = frame(YuvMatrix::BT2020, false);
        auto matches = [&target]() {
            const std::vector<uint8_t> rgba = readBack(target);
            for (size_t i = 0; i < BARS.size(); ++i) {
                const size_t x = i * BAR_WIDTH + BAR_WIDTH / 2;
                const uint8_t* px = &rgba[((HEIGHT / 2) * static_cast<size_t>(WIDTH) + x) * 4];
                if (!within(Rgb{px[0], px[1], px[2]}, BARS[i], 2)) return false;
            }
            return true;
        };

        YuvUploadState state;
        converter.begin();
        ASSERT_TRUE(converter.convert(a, target, state));
        converter.prefetch(b, state);
        converter.end();
        EXPECT_TRUE(matches());

        converter.begin();
        ASSERT_TRUE(converter.convert(b, target, state));  // the staged frame
        converter.prefetch(c, state);
        converter.end();
        EXPECT_TRUE(matches());

        converter.begin();
        ASSERT_TRUE(converter.convert(a, target, state));  // not the staged c: copied anew
        converter.end();
        EXPECT_TRUE(matches());

        // Destroyed with a copy staged: waits for it and unmaps.
        auto doomed = std::make_unique<YuvUploadState>();
        converter.begin();
        ASSERT_TRUE(converter.convert(a, target, *doomed));
        converter.prefetch(b, *doomed);
        converter.end();
        doomed.reset();
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
    const auto after = GlObjectStats::Snapshot::capture();
    EXPECT_TRUE(after.isFlat(baseline)) << after.diff(baseline);
}

TEST(YuvConverter, CoefficientsInvertTheForwardTransform) {
    // Pure maths, no GL: BT.709 limited white and black land on 1 and 0.
    const YuvConverter::Coefficients c =
        YuvConverter::coefficients(YuvMatrix::BT709, false, YuvLayout::NV12);
    const glm::vec3 white = c.matrix * (glm::vec3(235.0f, 128.0f, 128.0f) / 255.0f * c.scale - c.offset);
    const glm::vec3 black = c.matrix * (glm::vec3(16.0f, 128.0f, 128.0f) / 255.0f * c.scale - c.offset);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(white[i], 1.0f, 1e-4f);
        EXPECT_NEAR(black[i], 0.0f, 1e-4f);
    }
}
