// Tightly packed RGB24 uploads at widths not divisible by 4 (QM-088).
//
// Video frames arrive as RGB24 rows with no padding. With GL's default unpack
// alignment of 4, an 854-wide row (2562 bytes) is read as 2564, so every later row
// shifts sideways (and the last row reads past the buffer). Uploads an 854-wide
// gradient both ways the app does (create with data; createEmpty + update, the video
// path) and reads the texture back.
//
// Needs a real GL 3.3 context; SKIPs where none is available (headless CI).

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "renderer/Texture.hpp"

#include <cstdint>
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
        win = glfwCreateWindow(64, 64, "upload-test", nullptr, nullptr);
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

constexpr int WIDTH = 854;  // 854 * 3 = 2562 bytes per row: not a multiple of 4
constexpr int HEIGHT = 4;

// Distinct value per pixel: R = x low byte, G = x high byte, B = row.
std::vector<uint8_t> gradient() {
    // Slack past the image so a mis-aligned (pre-fix) upload over-reads into owned
    // memory: the test then fails on the pixel check instead of crashing.
    std::vector<uint8_t> pixels(static_cast<size_t>(WIDTH) * HEIGHT * 3 + 64, 0);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            uint8_t* p = &pixels[(static_cast<size_t>(y) * WIDTH + x) * 3];
            p[0] = static_cast<uint8_t>(x & 0xFF);
            p[1] = static_cast<uint8_t>(x >> 8);
            p[2] = static_cast<uint8_t>(40 * y + 7);
        }
    }
    return pixels;
}

// Read level 0 back as tightly packed RGB.
std::vector<uint8_t> readBack(const Texture& tex) {
    std::vector<uint8_t> out(static_cast<size_t>(WIDTH) * HEIGHT * 3, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, tex.getId());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, out.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return out;
}

void expectPixel(const std::vector<uint8_t>& image, int x, int y) {
    const uint8_t* p = &image[(static_cast<size_t>(y) * WIDTH + x) * 3];
    EXPECT_EQ(p[0], x & 0xFF) << "R at (" << x << "," << y << ")";
    EXPECT_EQ(p[1], x >> 8) << "G at (" << x << "," << y << ")";
    EXPECT_EQ(p[2], 40 * y + 7) << "B at (" << x << "," << y << ")";
}

}  // namespace

TEST(TextureUpload, CreateWithTightRgbRowsAtOddWidth) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    const std::vector<uint8_t> pixels = gradient();
    Texture tex;
    ASSERT_TRUE(tex.create(WIDTH, HEIGHT, 3, pixels.data()));

    const std::vector<uint8_t> image = readBack(tex);
    expectPixel(image, WIDTH - 3, 2);  // near the right edge of a later row
    expectPixel(image, WIDTH - 1, HEIGHT - 1);
    expectPixel(image, 0, 3);
}

TEST(TextureUpload, UpdateWithTightRgbRowsAtOddWidth) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    const std::vector<uint8_t> pixels = gradient();
    Texture tex;
    ASSERT_TRUE(tex.createEmpty(WIDTH, HEIGHT, 3));
    tex.update(pixels.data());  // the per-frame video upload path

    const std::vector<uint8_t> image = readBack(tex);
    expectPixel(image, WIDTH - 3, 2);
    expectPixel(image, WIDTH - 1, HEIGHT - 1);
}

TEST(TextureUpload, RestoresTheCallersUnpackAlignment) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context";

    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    const std::vector<uint8_t> pixels = gradient();
    Texture tex;
    ASSERT_TRUE(tex.create(WIDTH, HEIGHT, 3, pixels.data()));
    tex.update(pixels.data());

    GLint alignment = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    EXPECT_EQ(alignment, 8);
}
