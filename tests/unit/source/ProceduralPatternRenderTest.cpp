// The built-in procedural patterns are OpenGL 3.3 fragment shaders drawn into the
// pattern's own texture (ADR-062). Each test renders a small texture and reads it
// back. Needs a real GL 3.3 context; skips otherwise.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "source/procedural/CheckerboardSource.hpp"
#include "source/procedural/GradientSource.hpp"
#include "source/procedural/NoiseSource.hpp"
#include "source/procedural/PlasmaSource.hpp"
#include "source/procedural/SolidColorSource.hpp"
#include "source/procedural/TestPatternSource.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace quadmapper;

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
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        win = glfwCreateWindow(64, 64, "pattern-test", nullptr, nullptr);
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~GLContext() { if (win) { glfwDestroyWindow(win); glfwTerminate(); } }
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;
};

constexpr int N = 64;  // pattern side in pixels

using Rgba = std::array<int, 4>;

struct Image {
    std::vector<uint8_t> px;  // row 0 first, as the texture holds it
    Rgba at(int x, int y) const {
        const uint8_t* p = &px[(static_cast<size_t>(y) * N + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
};

Image readback(const ProceduralSource& s) {
    Image img;
    img.px.resize(static_cast<size_t>(N) * N * 4);
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glBindTexture(GL_TEXTURE_2D, s.getGLTextureId());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
    return img;
}

Rgba bytes(const Color& c) {
    return {static_cast<int>(std::lround(c.r * 255.0f)), static_cast<int>(std::lround(c.g * 255.0f)),
            static_cast<int>(std::lround(c.b * 255.0f)), static_cast<int>(std::lround(c.a * 255.0f))};
}

void expectNear(const Rgba& got, const Rgba& want, int tol, const char* what, int x, int y) {
    for (int i = 0; i < 4; ++i) {
        EXPECT_LE(std::abs(got[i] - want[i]), tol)
            << what << " at (" << x << "," << y << ") channel " << i << ": got " << got[i]
            << " want " << want[i];
    }
}

bool uniform(const Image& img) {
    const Rgba first = img.at(0, 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            if (img.at(x, y) != first) return false;
    return true;
}

}  // namespace

#define REQUIRE_GL() \
    GLContext ctx;   \
    if (!ctx.ok) GTEST_SKIP() << "no GL 3.3 context"

TEST(ProceduralPatternRender, SolidColourIsExactEverywhere) {
    REQUIRE_GL();
    SolidColorSource s(N, N);
    ASSERT_TRUE(s.initialize());
    const Color c(0.2f, 0.4f, 0.6f, 1.0f);
    s.setColor(c);
    ASSERT_TRUE(s.update(0.0));
    const Image img = readback(s);
    const Rgba want = bytes(c);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            ASSERT_EQ(img.at(x, y), want) << "(" << x << "," << y << ")";
}

TEST(ProceduralPatternRender, CheckerboardCellsMatchTheCpu) {
    REQUIRE_GL();
    CheckerboardSource s(N, N);
    ASSERT_TRUE(s.initialize());
    const Color c1(1.0f, 0.0f, 0.0f, 1.0f), c2(0.0f, 0.0f, 1.0f, 1.0f);
    s.setColor1(c1);
    s.setColor2(c2);
    s.setSize(8.0f);
    ASSERT_TRUE(s.update(0.0));
    const Image img = readback(s);
    // Same maths as the shader: cell = floor(pixel / N * size), even cells are colour 1.
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const int cx = static_cast<int>(std::floor(static_cast<float>(x) / N * 8.0f));
            const int cy = static_cast<int>(std::floor(static_cast<float>(y) / N * 8.0f));
            const Rgba want = bytes(((cx + cy) % 2) == 0 ? c1 : c2);
            ASSERT_EQ(img.at(x, y), want) << "(" << x << "," << y << ")";
        }
    }
    // Spot checks that include texture row 0 and the last row.
    EXPECT_EQ(img.at(0, 0), bytes(c1));
    EXPECT_EQ(img.at(8, 0), bytes(c2));
    EXPECT_EQ(img.at(0, 8), bytes(c2));
    EXPECT_EQ(img.at(0, N - 1), bytes(c2));  // row 7, column 0: odd
}

TEST(ProceduralPatternRender, GradientEndsAndRow0) {
    REQUIRE_GL();
    GradientSource s(N, N);
    ASSERT_TRUE(s.initialize());
    const Color a(1.0f, 0.0f, 0.0f, 1.0f), b(0.0f, 0.0f, 1.0f, 1.0f);
    s.setStartColor(a);
    s.setEndColor(b);
    s.setAngle(0.0f);
    s.setGradientType(GradientSource::GradientType::Linear);
    ASSERT_TRUE(s.update(0.0));
    const Image img = readback(s);
    // Linear, angle 0: t = x / N, the same on every row.
    expectNear(img.at(0, 0), bytes(a), 1, "start", 0, 0);
    expectNear(img.at(0, N - 1), bytes(a), 1, "start", 0, N - 1);
    expectNear(img.at(N - 1, 0), {4, 0, 251, 255}, 2, "end (t = 63/64)", N - 1, 0);
    expectNear(img.at(N / 2, 0), {128, 0, 128, 255}, 2, "middle", N / 2, 0);
}

TEST(ProceduralPatternRender, TestPatternsAtKnownPlaces) {
    REQUIRE_GL();
    TestPatternSource s(N, N);
    ASSERT_TRUE(s.initialize());

    s.setPatternType(TestPatternSource::PatternType::SMPTEBars);
    ASSERT_TRUE(s.update(0.0));
    Image img = readback(s);
    expectNear(img.at(4, 4), {191, 191, 191, 255}, 1, "smpte white bar, row 0 area", 4, 4);
    expectNear(img.at(0, 0), {191, 191, 191, 255}, 1, "smpte row 0", 0, 0);
    expectNear(img.at(60, 4), {0, 0, 191, 255}, 1, "smpte blue bar", 60, 4);
    expectNear(img.at(32, 60), {128, 128, 128, 255}, 1, "smpte grey ramp", 32, 60);

    s.setPatternType(TestPatternSource::PatternType::Grid);
    ASSERT_TRUE(s.update(0.0));
    img = readback(s);
    expectNear(img.at(0, 0), {255, 255, 255, 255}, 0, "grid line", 0, 0);
    expectNear(img.at(1, 1), {51, 51, 51, 255}, 1, "grid even cell", 1, 1);
    expectNear(img.at(3, 1), {77, 77, 77, 255}, 1, "grid odd cell", 3, 1);

    s.setPatternType(TestPatternSource::PatternType::Circle);
    ASSERT_TRUE(s.update(0.0));
    img = readback(s);
    expectNear(img.at(32, 32), {255, 0, 0, 255}, 0, "circle centre marker", 32, 32);
    expectNear(img.at(32, 10), {255, 255, 255, 255}, 0, "circle crosshair", 32, 10);

    s.setPatternType(TestPatternSource::PatternType::Gradient);
    ASSERT_TRUE(s.update(0.0));
    img = readback(s);
    expectNear(img.at(0, 0), {255, 0, 0, 255}, 1, "hue gradient, red at left of row 0", 0, 0);
}

TEST(ProceduralPatternRender, NoiseIsRepeatableAndAnimated) {
    REQUIRE_GL();
    NoiseSource a(N, N), b(N, N), c(N, N);
    for (NoiseSource* s : {&a, &b, &c}) {
        ASSERT_TRUE(s->initialize());
        s->setAnimate(true);
        s->setSpeed(1.0f);
    }
    ASSERT_TRUE(a.update(0.5));
    ASSERT_TRUE(b.update(0.5));
    ASSERT_TRUE(c.update(0.5));
    ASSERT_TRUE(c.update(0.5));  // time 1.0
    const Image ia = readback(a), ib = readback(b), ic = readback(c);
    EXPECT_FALSE(uniform(ia));
    EXPECT_EQ(ia.px, ib.px);
    EXPECT_NE(ia.px, ic.px);
}

TEST(ProceduralPatternRender, PlasmaIsRepeatableAndAnimated) {
    REQUIRE_GL();
    PlasmaSource a(N, N), b(N, N), c(N, N);
    for (PlasmaSource* s : {&a, &b, &c}) ASSERT_TRUE(s->initialize());
    ASSERT_TRUE(a.update(0.5));
    ASSERT_TRUE(b.update(0.5));
    ASSERT_TRUE(c.update(0.5));
    ASSERT_TRUE(c.update(0.5));
    const Image ia = readback(a), ib = readback(b), ic = readback(c);
    EXPECT_FALSE(uniform(ia));
    EXPECT_EQ(ia.px, ib.px);
    EXPECT_NE(ia.px, ic.px);
}

TEST(ProceduralPatternRender, UpdateLeavesGlStateAsItFoundIt) {
    REQUIRE_GL();
    // A framebuffer, viewport, program and VAO the caller owns.
    GLuint tex = 0, fbo = 0, vao = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(3, 5, 7, 6);
    glEnable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 2, 2);

    SolidColorSource s(N, N);
    ASSERT_TRUE(s.initialize());  // also must not disturb the binding
    ASSERT_TRUE(s.update(0.0));

    GLint drawFbo = 0, readFbo = 0, program = -1, boundVao = 0;
    GLint vp[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &boundVao);
    EXPECT_EQ(static_cast<GLuint>(drawFbo), fbo);
    EXPECT_EQ(static_cast<GLuint>(readFbo), fbo);
    EXPECT_EQ(vp[0], 3);
    EXPECT_EQ(vp[1], 5);
    EXPECT_EQ(vp[2], 7);
    EXPECT_EQ(vp[3], 6);
    EXPECT_EQ(program, 0);
    EXPECT_EQ(static_cast<GLuint>(boundVao), vao);
    EXPECT_TRUE(glIsEnabled(GL_BLEND));
    EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
    EXPECT_FALSE(glIsEnabled(GL_DEPTH_TEST));

    s.shutdown();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}
