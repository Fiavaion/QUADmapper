// GPU readback tests for "projector truth" (QM-087): a Renderer created in a
// FRESH GL context (as every per-output window gets) must composite exactly like
// the editor, and must apply the output rotation.
//
// Each test owns its own hidden-window GL 3.3 context, so no blend state leaks in
// from another context: the only GL state is what Renderer itself establishes.
// Where no GL context is available (headless CI) the tests SKIP.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "renderer/Renderer.hpp"
#include "renderer/Framebuffer.hpp"
#include "scene/Quad.hpp"

using namespace quadmapper;

namespace {

// Hidden-window GL 3.3 core context, RAII. ok == false means no GL is available.
struct GLContext {
    GLFWwindow* win = nullptr;
    bool ok = false;
    GLContext() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "output-composite-test", nullptr, nullptr);
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

RGB readPixel(int x, int y) {
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return {px[0], px[1], px[2]};
}

// A quad covering the canvas rectangle [x0,x1] x [y0,y1] (canvas y points down).
Quad makeQuad(float x0, float y0, float x1, float y1) {
    Quad quad;
    quad.getGeometry().setCorners({Vec2(x0, y0), Vec2(x1, y0), Vec2(x1, y1), Vec2(x0, y1)});
    return quad;
}

bool isWhite(const RGB& c) { return c.r > 200 && c.g > 200 && c.b > 200; }
bool isBlack(const RGB& c) { return c.r < 50 && c.g < 50 && c.b < 50; }

}  // namespace

// A 50%-opacity white quad over a blue clear must blend to (~128, ~128, 255). A
// context without GL_BLEND writes opaque white — the pre-QM-087 output windows.
TEST(OutputCompositeRenderTest, FreshContextRendererBlendsOpacity) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(16, 16));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    Quad quad = makeQuad(0.0f, 0.0f, 1.0f, 1.0f);
    quad.setOpacity(0.5f);
    renderer.renderQuad(quad, nullptr);  // null texture -> the white default

    RGB c = readPixel(8, 8);
    EXPECT_GT(c.r, 100);
    EXPECT_LT(c.r, 156);
    EXPECT_GT(c.g, 100);
    EXPECT_LT(c.g, 156);
    EXPECT_GT(c.b, 240);

    Framebuffer::unbind();
    renderer.shutdown();
}

// Soft edge feathers alpha; at the very left edge of a half-width left feather the
// quad is nearly transparent, so the clear colour must show through.
TEST(OutputCompositeRenderTest, FreshContextRendererBlendsSoftEdge) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(16, 16));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    Quad quad = makeQuad(0.0f, 0.0f, 1.0f, 1.0f);
    SoftEdge edge;
    edge.left = 0.5f;  // alpha ramps 0 -> 1 across the left half of the quad
    quad.setSoftEdge(edge);
    renderer.renderQuad(quad, nullptr);

    RGB left = readPixel(0, 8);    // alpha ~ 1/16: almost pure clear colour
    EXPECT_LT(left.r, 50);
    EXPECT_GT(left.b, 240);
    RGB right = readPixel(15, 8);  // outside the feather: opaque white
    EXPECT_GT(right.r, 200);

    Framebuffer::unbind();
    renderer.shutdown();
}

// Output rotation (clockwise quarter turns): content drawn in the TOP half of the
// canvas must land on the right (90), bottom (180) or left (270) of the output
// framebuffer. The framebuffer is landscape (32x16); at 90/270 the canvas is the
// portrait effective resolution, exactly as the editor lays it out.
class OutputRotationRenderTest : public ::testing::TestWithParam<Rotation> {};

TEST_P(OutputRotationRenderTest, TopHalfOfCanvasLandsOnRotatedSide) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(32, 16));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    renderer.setOutputRotation(GetParam());
    renderer.renderQuad(makeQuad(0.0f, 0.0f, 1.0f, 0.5f), nullptr);

    // GL window coordinates: origin bottom-left, y up.
    const RGB top = readPixel(16, 12);
    const RGB bottom = readPixel(16, 3);
    const RGB left = readPixel(7, 8);
    const RGB right = readPixel(24, 8);
    switch (GetParam()) {
        case Rotation::R0:
            EXPECT_TRUE(isWhite(top));
            EXPECT_TRUE(isBlack(bottom));
            break;
        case Rotation::R90:
            EXPECT_TRUE(isWhite(right));
            EXPECT_TRUE(isBlack(left));
            break;
        case Rotation::R180:
            EXPECT_TRUE(isWhite(bottom));
            EXPECT_TRUE(isBlack(top));
            break;
        case Rotation::R270:
            EXPECT_TRUE(isWhite(left));
            EXPECT_TRUE(isBlack(right));
            break;
    }

    Framebuffer::unbind();
    renderer.shutdown();
}

// A half-plane probe cannot tell a clockwise turn from a mirror: at 90 a
// transpose also puts the top half on the right. A marker in the canvas's
// top-LEFT corner can: a clockwise turn carries it to the top-right (90),
// bottom-right (180) or bottom-left (270); a mirror lands it in another corner.
TEST_P(OutputRotationRenderTest, TopLeftCornerTurnsClockwise) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(32, 16));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    renderer.setOutputRotation(GetParam());
    renderer.renderQuad(makeQuad(0.0f, 0.0f, 0.5f, 0.5f), nullptr);

    // Quadrant centres, GL window coordinates (origin bottom-left, y up).
    const RGB topLeft = readPixel(8, 12);
    const RGB topRight = readPixel(24, 12);
    const RGB bottomLeft = readPixel(8, 4);
    const RGB bottomRight = readPixel(24, 4);
    const RGB* marked = nullptr;
    switch (GetParam()) {
        case Rotation::R0:   marked = &topLeft; break;
        case Rotation::R90:  marked = &topRight; break;
        case Rotation::R180: marked = &bottomRight; break;
        case Rotation::R270: marked = &bottomLeft; break;
    }
    ASSERT_NE(marked, nullptr);
    for (const RGB* corner : {&topLeft, &topRight, &bottomLeft, &bottomRight}) {
        if (corner == marked) {
            EXPECT_TRUE(isWhite(*corner));
        } else {
            EXPECT_TRUE(isBlack(*corner));
        }
    }

    Framebuffer::unbind();
    renderer.shutdown();
}

INSTANTIATE_TEST_SUITE_P(AllQuarterTurns, OutputRotationRenderTest,
                         ::testing::Values(Rotation::R0, Rotation::R90,
                                           Rotation::R180, Rotation::R270),
                         [](const ::testing::TestParamInfo<Rotation>& info) {
                             return "R" + std::to_string(static_cast<int>(info.param));
                         });
