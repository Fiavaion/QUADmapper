// GPU readback test for the per-quad crop (ADR-040 P1, QM-068).
//
// Renders a full quad textured with a 2x1 red|green source through the real
// quad shader, into an offscreen FBO, then reads back the centre pixel. A crop
// of the LEFT half must sample the red texel; the RIGHT half must sample green.
// This verifies the on-screen result of the crop, not just the data round-trip.
//
// Needs a real GL 3.3 context; where none is available (headless CI) it SKIPs,
// matching the repo's hardware-absent test convention.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "renderer/ShaderProgram.hpp"
#include "renderer/Texture.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "scene/Quad.hpp"
#include "ShaderSources.hpp"  // generated: quadmapper::shaders::QUAD_VERT / QUAD_FRAG

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
        win = glfwCreateWindow(64, 64, "crop-test", nullptr, nullptr);
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

struct RGB { int r, g, b; };

// Render the quad with crop = {cropX, 0, cropW, 1} and return the FBO centre RGB.
RGB renderCropCentre(float cropX, float cropW) {
    // 2x1 source: left texel red, right texel green (texel centres at u=0.25/0.75).
    unsigned char texels[6] = {255, 0, 0,  0, 255, 0};
    Texture tex;
    tex.create(2, 1, 3, texels);

    // Full quad: position in [0,1] (quad.vert maps to NDC), projective UV (u, v, q)
    // with q = 1 (no perspective).
    const float verts[] = {
        0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
        1.0f, 0.0f,  1.0f, 0.0f, 1.0f,
        1.0f, 1.0f,  1.0f, 1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f, 1.0f,
    };
    const unsigned int idx[] = {0, 1, 2, 0, 2, 3};

    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    ShaderProgram prog;
    EXPECT_TRUE(prog.create(shaders::QUAD_VERT, shaders::QUAD_FRAG));
    prog.bind();
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
    prog.setVec2("u_CropOffset", glm::vec2(cropX, 0.0f));
    prog.setVec2("u_CropSize", glm::vec2(cropW, 1.0f));
    // Editor view transform (ADR-041); identity = full-frame, as the Renderer uses
    // for output/preview. Must be set explicitly here since this drives the shader
    // directly (unset GLSL uniforms default to 0, collapsing the quad to a point).
    prog.setVec2("u_ViewScale", glm::vec2(1.0f, 1.0f));
    prog.setVec2("u_ViewOffset", glm::vec2(0.0f, 0.0f));

    Framebuffer fbo;
    fbo.resize(16, 16);
    fbo.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    tex.bind(0);
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

    Framebuffer::unbind();
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteVertexArrays(1, &vao);

    return {px[0], px[1], px[2]};
}

}  // namespace

TEST(CropRenderTest, LeftHalfCropSamplesLeftOfSource) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    RGB c = renderCropCentre(0.0f, 0.5f);  // left half → red texel
    EXPECT_GT(c.r, 200);
    EXPECT_LT(c.g, 60);
}

TEST(CropRenderTest, RightHalfCropSamplesRightOfSource) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    RGB c = renderCropCentre(0.5f, 0.5f);  // right half → green texel
    EXPECT_GT(c.g, 200);
    EXPECT_LT(c.r, 60);
}

// A quarter-strip (the box-wrap case): crop column 4 = {0.75, 0, 0.25, 1}. Its
// centre samples u = 0.75 + 0.25*0.5 = 0.875 → still the green (right) texel.
TEST(CropRenderTest, RightQuarterStripSamplesGreen) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    RGB c = renderCropCentre(0.75f, 0.25f);
    EXPECT_GT(c.g, 200);
    EXPECT_LT(c.r, 60);
}

// Warp mode (ADR-058) through the real Renderer: switching only the mode of a quad must
// rebuild its cached GPU mesh, or the projector keeps the old warp until the next
// corner edit. Same file as the crop tests because it is the same readback check.
TEST(WarpModeRenderTest, SwitchingTheModeRedrawsTheQuad) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    constexpr int N = 32;

    // 16x1 source: left half red, right half green, so u < 0.5 is red.
    std::vector<unsigned char> texels;
    for (int i = 0; i < 16; ++i) {
        texels.insert(texels.end(), {static_cast<unsigned char>(i < 8 ? 255 : 0),
                                     static_cast<unsigned char>(i < 8 ? 0 : 255), 0});
    }
    Texture tex;
    ASSERT_TRUE(tex.create(16, 1, 3, texels.data()));

    Quad quad;  // Perspective by default
    quad.getGeometry().setCorners(
        {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(0.95f, 1.0f), Vec2(0.45f, 0.8f)});

    // A pixel the two warps put on opposite sides of u = 0.5, with margin.
    QuadGeometry linear = quad.getGeometry();
    linear.setWarpMode(WarpMode::LINEAR);
    int pickX = -1, pickY = -1;
    bool perspectiveRed = false;
    for (int y = 0; y < N && pickX < 0; ++y) {
        for (int x = 0; x < N; ++x) {
            const Vec2 p((x + 0.5f) / N, 1.0f - (y + 0.5f) / N);  // canvas y runs down
            const float up = quad.getGeometry().surfaceParamsAt(p).x;
            const float ul = linear.surfaceParamsAt(p).x;
            if (up < 0.0f || ul < 0.0f) continue;
            if ((up < 0.45f && ul > 0.55f) || (up > 0.55f && ul < 0.45f)) {
                pickX = x;
                pickY = y;
                perspectiveRed = up < 0.5f;
                break;
            }
        }
    }
    ASSERT_GE(pickX, 0) << "the corners must separate the two warps somewhere";

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    auto pixel = [&]() {
        Framebuffer fbo;
        EXPECT_TRUE(fbo.resize(N, N));
        fbo.bind();
        glViewport(0, 0, N, N);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderQuad(quad, &tex);
        unsigned char px[4] = {0, 0, 0, 0};
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(pickX, pickY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        Framebuffer::unbind();
        return RGB{px[0], px[1], px[2]};
    };

    const RGB first = pixel();
    EXPECT_EQ(first.r > first.g, perspectiveRed) << first.r << "," << first.g;
    quad.getGeometry().setWarpMode(WarpMode::LINEAR);  // same quad id, same cache slot
    const RGB second = pixel();
    EXPECT_EQ(second.r > second.g, !perspectiveRed) << second.r << "," << second.g;
    renderer.shutdown();
}
