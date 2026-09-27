// GPU readback tests for 3D objects seen from a projector (ADR-052, QM-095).
//
// ProjectorCamera::project() is the reference model. These tests prove the rendered
// pixels agree with it: an object lands where project() says, occludes by depth,
// and follows the same editor view transform and output rotation as quads.
//
// Each test owns a hidden-window GL 3.3 context; where none is available (headless
// CI) the tests SKIP, matching the repo's hardware-absent convention.

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/GlObjectStats.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "scene/MeshObject.hpp"
#include "scene/ObjLoader.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Quad.hpp"
#include "source/StaticImageSource.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
        win = glfwCreateWindow(64, 64, "mesh-object-test", nullptr, nullptr);
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

// Read back the bound framebuffer, top row first, so [row * w + col] is image
// pixel (col, row) in the projector's top-left-origin convention.
std::vector<RGB> readImage(int w, int h) {
    std::vector<unsigned char> raw(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    std::vector<RGB> image(static_cast<size_t>(w) * h);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const unsigned char* p = &raw[(static_cast<size_t>(h - 1 - row) * w + col) * 4];
            image[static_cast<size_t>(row) * w + col] = {p[0], p[1], p[2]};
        }
    }
    return image;
}

bool isRed(const RGB& c) { return c.r > 200 && c.g < 60 && c.b < 60; }
bool isGreen(const RGB& c) { return c.g > 200 && c.r < 60 && c.b < 60; }
bool isBlue(const RGB& c) { return c.b > 200 && c.r < 60 && c.g < 60; }
bool isWhite(const RGB& c) { return c.r > 200 && c.g > 200 && c.b > 200; }

// A 2x2 texture of one colour.
std::unique_ptr<Texture> solidTexture(unsigned char r, unsigned char g, unsigned char b) {
    const unsigned char px[16] = {r, g, b, 255, r, g, b, 255, r, g, b, 255, r, g, b, 255};
    auto texture = std::make_unique<Texture>();
    texture->create(2, 2, 4, px);
    return texture;
}

// Two triangles over the model-space square [-h, h]^2 at z = 0, facing +Z.
std::unique_ptr<MeshObject> makeSquare(float half, bool withUvs = true) {
    auto object = std::make_unique<MeshObject>();
    MeshData& mesh = object->editMesh();
    mesh.positions = {Vec3(-half, half, 0), Vec3(half, half, 0),
                      Vec3(half, -half, 0), Vec3(-half, -half, 0)};
    if (withUvs) {
        mesh.uvs = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
    }
    mesh.indices = {0, 3, 1, 1, 3, 2};
    return object;
}

// A mesh whose vertices project, through the identity-pose camera `cam`, onto the
// image rectangle [u0,u1] x [v0,v1] (pixels): each lies on its pixel's ray at z = 1.
std::unique_ptr<MeshObject> makeImageRect(const ProjectorCamera& cam, float u0, float v0,
                                          float u1, float v1) {
    auto ray = [&](float u, float v) {
        return Vec3((u - cam.intrinsics.cx) / cam.intrinsics.fx,
                    (v - cam.intrinsics.cy) / cam.intrinsics.fy, 1.0f);
    };
    auto object = std::make_unique<MeshObject>();
    MeshData& mesh = object->editMesh();
    mesh.positions = {ray(u0, v0), ray(u1, v0), ray(u1, v1), ray(u0, v1)};
    mesh.uvs = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
    mesh.indices = {0, 3, 1, 1, 3, 2};
    return object;
}

// A camera looking from `eye` at `target` with world +Y up (OpenCV frame:
// x right, y down, z forward), over a w x h image with an off-centre principal point.
ProjectorCamera lookAtCamera(const Vec3& eye, const Vec3& target, int w, int h) {
    ProjectorCamera cam;
    cam.intrinsics.width = w;
    cam.intrinsics.height = h;
    cam.intrinsics.fx = 1.30f * static_cast<float>(w);
    cam.intrinsics.fy = 1.25f * static_cast<float>(w);  // non-square pixels on purpose
    cam.intrinsics.cx = 0.53f * static_cast<float>(w);
    cam.intrinsics.cy = 0.46f * static_cast<float>(h);
    const Vec3 z = glm::normalize(target - eye);
    const Vec3 x = glm::normalize(glm::cross(z, Vec3(0, 1, 0)));
    const Vec3 y = glm::cross(z, x);
    cam.pose.rotation = glm::transpose(glm::mat3(x, y, z));  // rows are the camera axes
    cam.pose.translation = -(cam.pose.rotation * eye);
    return cam;
}

// A non-trivial model transform: rotated about Z and Y, shifted.
glm::mat4 skewedModel() {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), Vec3(0.08f, -0.05f, 0.1f));
    m = glm::rotate(m, glm::radians(25.0f), Vec3(0, 0, 1));
    m = glm::rotate(m, glm::radians(20.0f), Vec3(0, 1, 0));
    return m;
}

Vec2 projected(const ProjectorCamera& cam, const glm::mat4& model, const Vec3& p) {
    Vec2 pixel(0.0f);
    EXPECT_TRUE(cam.project(Vec3(model * glm::vec4(p, 1.0f)), pixel));
    return pixel;
}

// Distance from p to segment ab.
float segmentDistance(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a;
    const float t = std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
    return glm::length(p - (a + t * ab));
}

// Point in a convex polygon given in order (either winding).
bool insideConvex(const Vec2& p, const std::vector<Vec2>& poly) {
    int sign = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2 a = poly[i];
        const Vec2 b = poly[(i + 1) % poly.size()];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        const int s = cross > 0 ? 1 : (cross < 0 ? -1 : 0);
        if (s != 0) {
            if (sign != 0 && s != sign) return false;
            sign = s;
        }
    }
    return true;
}

}  // namespace

// (a) The pixel at project(centre) is the object's texture colour; far outside it
// is the clear colour.
TEST(MeshObjectRenderTest, CentreHasTextureColourOutsideIsClear) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 96, H = 64;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const ProjectorCamera cam = lookAtCamera(Vec3(0.4f, 0.3f, 2.2f), Vec3(0.0f), W, H);
    auto square = makeSquare(0.3f);
    square->setTransform(skewedModel());
    auto red = solidTexture(255, 0, 0);
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(std::move(square));
    std::unordered_map<UUID, Texture*> textures{{objects[0]->getId(), red.get()}};
    renderer.renderMeshObjects(objects, cam, textures);

    const auto image = readImage(W, H);
    const Vec2 centre = projected(cam, objects[0]->getTransform(), Vec3(0.0f));
    ASSERT_GE(centre.x, 0.0f); ASSERT_LT(centre.x, W);
    ASSERT_GE(centre.y, 0.0f); ASSERT_LT(centre.y, H);
    EXPECT_TRUE(isRed(image[static_cast<int>(centre.y) * W + static_cast<int>(centre.x)]));
    EXPECT_TRUE(isBlue(image[1 * W + 1]));            // top-left corner of the image
    EXPECT_TRUE(isBlue(image[(H - 2) * W + (W - 2)])); // bottom-right corner
    EXPECT_FALSE(glIsEnabled(GL_DEPTH_TEST));          // the mesh pass leaves depth off

    Framebuffer::unbind();
    renderer.shutdown();
}

// (b) Every pixel more than 1 px from the projected outline is inside/outside exactly
// as project() says: the corners and edges land within 1 px of project(corner).
TEST(MeshObjectRenderTest, CoverageMatchesProjectedCornersWithinOnePixel) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 160, H = 120;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const ProjectorCamera cam = lookAtCamera(Vec3(-0.6f, 0.5f, 2.0f), Vec3(0.05f, 0.0f, 0.0f), W, H);
    auto square = makeSquare(0.35f);
    square->setTransform(skewedModel());
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(std::move(square));
    renderer.renderMeshObjects(objects, cam, {});  // no texture: the white default

    const MeshData& mesh = objects[0]->getMesh();
    std::vector<Vec2> outline;
    for (const Vec3& p : mesh.positions) {
        outline.push_back(projected(cam, objects[0]->getTransform(), p));
    }

    const auto image = readImage(W, H);
    int checked = 0, inside = 0, wrong = 0;
    for (int row = 0; row < H; ++row) {
        for (int col = 0; col < W; ++col) {
            const Vec2 centre(col + 0.5f, row + 0.5f);
            float edge = 1e9f;
            for (size_t i = 0; i < outline.size(); ++i) {
                edge = std::min(edge, segmentDistance(centre, outline[i],
                                                      outline[(i + 1) % outline.size()]));
            }
            if (edge <= 1.0f) continue;  // within 1 px of the outline: either is fine
            ++checked;
            const bool expectedInside = insideConvex(centre, outline);
            inside += expectedInside ? 1 : 0;
            if (isWhite(image[static_cast<size_t>(row) * W + col]) != expectedInside) ++wrong;
        }
    }
    EXPECT_GT(inside, 500);  // the square covers a real area of the image
    EXPECT_GT(checked - inside, 500);
    EXPECT_EQ(wrong, 0) << "pixels disagreeing with project() by more than 1 px";

    Framebuffer::unbind();
    renderer.shutdown();
}

// (c) The nearer of two overlapping objects wins, whichever is drawn first.
TEST(MeshObjectRenderTest, NearerObjectWinsRegardlessOfDrawOrder) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 64, H = 64;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();

    const ProjectorCamera cam = lookAtCamera(Vec3(0.2f, 0.1f, 2.0f), Vec3(0.0f), W, H);
    auto red = solidTexture(255, 0, 0);
    auto green = solidTexture(0, 255, 0);

    auto far = makeSquare(0.5f);    // z = 0, red
    auto near = makeSquare(0.15f);  // z = 0.6, nearer the camera, green
    near->setTransform(glm::translate(glm::mat4(1.0f), Vec3(0, 0, 0.6f)));
    const UUID farId = far->getId();
    const UUID nearId = near->getId();
    const Vec2 centre = projected(cam, glm::mat4(1.0f), Vec3(0.0f));
    const Vec2 farOnly = projected(cam, glm::mat4(1.0f), Vec3(-0.42f, 0.0f, 0.0f));
    std::unordered_map<UUID, Texture*> textures{{farId, red.get()}, {nearId, green.get()}};

    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(std::move(far));
    objects.push_back(std::move(near));
    for (int order = 0; order < 2; ++order) {
        if (order == 1) std::swap(objects[0], objects[1]);  // near first, then far
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderMeshObjects(objects, cam, textures);
        const auto image = readImage(W, H);
        EXPECT_TRUE(isGreen(image[static_cast<int>(centre.y) * W + static_cast<int>(centre.x)]))
            << "draw order " << order;
        EXPECT_TRUE(isRed(image[static_cast<int>(farOnly.y) * W + static_cast<int>(farOnly.x)]))
            << "draw order " << order;
    }

    Framebuffer::unbind();
    renderer.shutdown();
}

// (d) + WYSIWYG: a mesh that projects onto a canvas rectangle covers exactly the
// pixels a quad on that rectangle covers, under each output rotation and under the
// editor view transform. The framebuffer is the landscape physical output; the
// camera's image is the canvas (portrait at 90/270), as the editor lays it out.
class MeshOutputRotationTest : public ::testing::TestWithParam<Rotation> {};

TEST_P(MeshOutputRotationTest, MeshCoversSamePixelsAsQuad) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int FW = 64, FH = 32;  // physical output
    const bool portrait = GetParam() == Rotation::R90 || GetParam() == Rotation::R270;
    const int CW = portrait ? FH : FW;  // canvas = effective resolution
    const int CH = portrait ? FW : FH;

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(FW, FH));
    fbo.bind();
    renderer.setOutputRotation(GetParam());

    ProjectorCamera cam;  // identity pose: the ray of pixel (u, v) is ((u-cx)/fx, (v-cy)/fy, 1)
    cam.intrinsics = ProjectorIntrinsics::fromThrowRatio(1.5f, CW, CH);

    // An off-centre rectangle covering the canvas's top-left region.
    const float x0 = 0.125f, y0 = 0.0625f, x1 = 0.5f, y1 = 0.5f;
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeImageRect(cam, x0 * CW, y0 * CH, x1 * CW, y1 * CH));
    Quad quad;
    quad.getGeometry().setCorners({Vec2(x0, y0), Vec2(x1, y0), Vec2(x1, y1), Vec2(x0, y1)});

    for (int zoomed = 0; zoomed < 2; ++zoomed) {
        if (zoomed) {
            renderer.setViewTransform(glm::vec2(0.75f, 0.5f), glm::vec2(0.2f, 0.25f), 0.0f);
        }
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderQuad(quad, nullptr);
        const auto quadImage = readImage(FW, FH);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderMeshObjects(objects, cam, {});
        const auto meshImage = readImage(FW, FH);

        int covered = 0, differ = 0;
        for (size_t i = 0; i < quadImage.size(); ++i) {
            covered += isWhite(quadImage[i]) ? 1 : 0;
            differ += isWhite(quadImage[i]) != isWhite(meshImage[i]) ? 1 : 0;
        }
        EXPECT_GT(covered, 40) << "zoomed " << zoomed;
        EXPECT_LT(covered, FW * FH / 2) << "zoomed " << zoomed;
        // Edges fall on or near pixel boundaries; allow a stray edge pixel only.
        EXPECT_LE(differ, 2) << "zoomed " << zoomed;
    }

    Framebuffer::unbind();
    renderer.shutdown();
}

INSTANTIATE_TEST_SUITE_P(AllQuarterTurns, MeshOutputRotationTest,
                         ::testing::Values(Rotation::R0, Rotation::R90,
                                           Rotation::R180, Rotation::R270),
                         [](const ::testing::TestParamInfo<Rotation>& info) {
                             return "R" + std::to_string(static_cast<int>(info.param));
                         });

// Without UVs the XY bounds map onto the texture upright: the image's top row lands
// on the object's top (+Y) edge as seen from the front.
TEST(MeshObjectRenderTest, PlanarUvsKeepTheImageUpright) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 64, H = 64;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Top row red, bottom row green (first uploaded row = the image's top).
    const unsigned char px[16] = {255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    Texture texture;
    ASSERT_TRUE(texture.create(2, 2, 4, px));

    const ProjectorCamera cam = ProjectorCamera::framing(Vec3(-0.5f, -0.5f, 0.0f),
                                                         Vec3(0.5f, 0.5f, 0.0f),
                                                         ProjectorIntrinsics::fromThrowRatio(1.5f, W, H));
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.5f, /*withUvs=*/false));
    renderer.renderMeshObjects(objects, cam, {{objects[0]->getId(), &texture}});

    const auto image = readImage(W, H);
    const Vec2 upper = projected(cam, glm::mat4(1.0f), Vec3(0.0f, 0.3f, 0.0f));
    const Vec2 lower = projected(cam, glm::mat4(1.0f), Vec3(0.0f, -0.3f, 0.0f));
    EXPECT_LT(upper.y, lower.y);  // world +Y is up in the framed image
    EXPECT_TRUE(isRed(image[static_cast<int>(upper.y) * W + static_cast<int>(upper.x)]));
    EXPECT_TRUE(isGreen(image[static_cast<int>(lower.y) * W + static_cast<int>(lower.x)]));

    Framebuffer::unbind();
    renderer.shutdown();
}

// An unchanged mesh is uploaded once; replacing its MeshData re-uploads it (and the
// old buffers are freed), so the new geometry shows.
TEST(MeshObjectRenderTest, MeshUploadsOnceAndRebuildsWhenReplaced) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 64, H = 64;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();

    const ProjectorCamera cam = lookAtCamera(Vec3(0.0f, 0.0f, 2.0f), Vec3(0.0f), W, H);
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.1f));
    const Vec2 edge = projected(cam, glm::mat4(1.0f), Vec3(0.35f, 0.0f, 0.0f));
    auto whiteAtEdge = [&] {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderMeshObjects(objects, cam, {});
        return isWhite(readImage(W, H)[static_cast<int>(edge.y) * W + static_cast<int>(edge.x)]);
    };

    EXPECT_FALSE(whiteAtEdge());
    const long vaos = GlObjectStats::vertexArrays();
    const long buffers = GlObjectStats::vertexBuffers();
    EXPECT_FALSE(whiteAtEdge());
    EXPECT_EQ(GlObjectStats::vertexArrays(), vaos);  // no re-upload for an unchanged mesh
    EXPECT_EQ(GlObjectStats::vertexBuffers(), buffers);

    objects[0]->setMesh(makeSquare(0.45f)->getMesh());  // replaced: same counts, new data
    EXPECT_TRUE(whiteAtEdge());
    EXPECT_EQ(GlObjectStats::vertexArrays(), vaos);  // old buffers freed, new ones made
    EXPECT_EQ(GlObjectStats::vertexBuffers(), buffers);

    Framebuffer::unbind();
    renderer.shutdown();
}

// TD-15: an in-place edit of an interior vertex keeps the storage, the sizes and the
// first and last vertices, which is all the old change check looked at. The mesh
// revision still changes, so the GPU copy is rebuilt and the edit shows.
TEST(MeshObjectRenderTest, InPlaceInteriorVertexEditIsPickedUp) {
    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 64, H = 64;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();

    const ProjectorCamera cam = lookAtCamera(Vec3(0.0f, 0.0f, 2.0f), Vec3(0.0f), W, H);
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(std::make_unique<MeshObject>());
    {
        // A fan around the centre vertex (index 1, neither first nor last) over the
        // square [-0.1, 0.1]^2.
        MeshData& mesh = objects[0]->editMesh();
        mesh.positions = {Vec3(-0.1f, 0.1f, 0), Vec3(0.0f, 0.0f, 0), Vec3(0.1f, 0.1f, 0),
                          Vec3(0.1f, -0.1f, 0), Vec3(-0.1f, -0.1f, 0)};
        mesh.indices = {1, 0, 4, 1, 4, 3, 1, 3, 2, 1, 2, 0};
    }
    const Vec2 probe = projected(cam, glm::mat4(1.0f), Vec3(0.3f, 0.0f, 0.0f));
    auto whiteAtProbe = [&] {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderMeshObjects(objects, cam, {});
        return isWhite(readImage(W, H)[static_cast<int>(probe.y) * W + static_cast<int>(probe.x)]);
    };

    EXPECT_FALSE(whiteAtProbe());
    const MeshData& before = objects[0]->getMesh();
    const Vec3* storage = before.positions.data();
    const Vec3 first = before.positions.front();
    const Vec3 last = before.positions.back();
    const uint64_t revision = objects[0]->getMeshRevision();
    const long vaos = GlObjectStats::vertexArrays();
    const long buffers = GlObjectStats::vertexBuffers();

    objects[0]->editMesh().positions[1] = Vec3(0.4f, 0.0f, 0.0f);  // pull the centre out

    const MeshData& after = objects[0]->getMesh();
    ASSERT_EQ(after.positions.data(), storage);  // what the old check compared: unchanged
    ASSERT_EQ(after.positions.front(), first);
    ASSERT_EQ(after.positions.back(), last);
    EXPECT_NE(objects[0]->getMeshRevision(), revision);
    EXPECT_TRUE(whiteAtProbe());
    EXPECT_EQ(GlObjectStats::vertexArrays(), vaos);  // rebuilt, the old buffers freed
    EXPECT_EQ(GlObjectStats::vertexBuffers(), buffers);

    // Drawing again without an edit does not upload again.
    EXPECT_TRUE(whiteAtProbe());
    EXPECT_EQ(GlObjectStats::vertexArrays(), vaos);

    Framebuffer::unbind();
    renderer.shutdown();
}

// One orientation convention for every source (GUI QA 2026-09-26: stills showed upside
// down on quads while video was upright). Frames are stored top row first, so a still
// loaded from disk equals the buffer a video or procedural source hands over, and it
// shows upright on a quad and, through OBJ texture coordinates, on an object.
TEST(MeshObjectRenderTest, StillImagesShowUprightOnQuadsAndObjects) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "qm_orientation_test";
    fs::create_directories(dir);

    // 2x2 image, top row red, bottom row green, in the order a video frame stores it.
    const std::vector<uint8_t> topRowFirst = {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0};
    const fs::path ppm = dir / "top_red.ppm";
    {
        std::ofstream file(ppm, std::ios::binary);
        file << "P6\n2 2\n255\n";
        file.write(reinterpret_cast<const char*>(topRowFirst.data()),
                   static_cast<std::streamsize>(topRowFirst.size()));
    }
    // A unit square facing +Z with OBJ's usual texture coordinates (v = 0 at the bottom).
    const fs::path obj = dir / "square.obj";
    {
        std::ofstream file(obj, std::ios::binary);
        file << "v -0.5 -0.5 0\nv 0.5 -0.5 0\nv 0.5 0.5 0\nv -0.5 0.5 0\n"
                "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                "f 1/1 2/2 3/3 4/4\n";
    }

    StaticImageSource still(ppm);
    ASSERT_TRUE(still.initialize());
    const FrameData* frame = still.getCurrentFrame();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->channels, 3);
    EXPECT_EQ(frame->pixels, topRowFirst);  // same memory order as a video frame

    auto mesh = loadObj(obj);
    ASSERT_TRUE(mesh.isSuccess()) << mesh.error().message;
    std::error_code ec;
    fs::remove_all(dir, ec);

    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";

    constexpr int W = 64, H = 64;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();
    Texture texture;
    ASSERT_TRUE(texture.create(frame->width, frame->height, frame->channels,
                               frame->pixels.data()));

    // A quad covering the whole output: red on top.
    Quad quad;
    quad.getGeometry().setCorners({Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)});
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.renderQuad(quad, &texture);
    auto image = readImage(W, H);
    EXPECT_TRUE(isRed(image[(H / 4) * W + W / 2]));
    EXPECT_TRUE(isGreen(image[(3 * H / 4) * W + W / 2]));

    // The OBJ square seen from the front: red on its top (+Y) half.
    const ProjectorCamera cam = ProjectorCamera::framing(Vec3(-0.5f, -0.5f, 0.0f),
                                                         Vec3(0.5f, 0.5f, 0.0f),
                                                         ProjectorIntrinsics::fromThrowRatio(1.5f, W, H));
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(std::make_unique<MeshObject>());
    objects[0]->setMesh(mesh.value());
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.renderMeshObjects(objects, cam, {{objects[0]->getId(), &texture}});
    image = readImage(W, H);
    const Vec2 upper = projected(cam, glm::mat4(1.0f), Vec3(0.0f, 0.3f, 0.0f));
    const Vec2 lower = projected(cam, glm::mat4(1.0f), Vec3(0.0f, -0.3f, 0.0f));
    EXPECT_TRUE(isRed(image[static_cast<int>(upper.y) * W + static_cast<int>(upper.x)]));
    EXPECT_TRUE(isGreen(image[static_cast<int>(lower.y) * W + static_cast<int>(lower.x)]));

    Framebuffer::unbind();
    renderer.shutdown();
}

// TD-16a measurement, not run by ctest (--gtest_also_run_disabled_tests): frame times
// while a 2M-triangle OBJ loads. "Synchronous" is what import and project load did
// before, one loadObj on the render thread; the frame loop is what they do now, the
// parse on ObjLoadQueue's worker and the finished mesh adopted at a frame boundary,
// then drawn (its upload lands in that frame).
TEST(MeshObjectRenderTest, DISABLED_LargeMeshLoadFrameTimes) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    const fs::path file = fs::temp_directory_path() / "qm_td16_2m.obj";
    constexpr int N = 1000;  // N x N quads = 2,000,000 triangles
    {
        std::ofstream out(file, std::ios::binary);
        std::string line;
        for (int y = 0; y <= N; ++y) {
            for (int x = 0; x <= N; ++x) {
                line = "v " + std::to_string(x * 0.001f) + ' ' + std::to_string(y * 0.001f) + " 0\n";
                out << line;
            }
        }
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const int a = y * (N + 1) + x + 1;
                out << "f " << a << ' ' << a + 1 << ' ' << a + N + 2 << ' ' << a + N + 1 << '\n';
            }
        }
    }

    const auto s0 = Clock::now();
    auto sync = loadObj(file);
    const double syncMs = ms(Clock::now() - s0);
    ASSERT_TRUE(sync.isSuccess()) << sync.error().message;
    ASSERT_EQ(sync.value().triangleCount(), 2u * N * N);
    sync = Err<MeshData>("released");

    GLContext ctx;
    if (!ctx.ok) GTEST_SKIP() << "No GL 3.3 context available";
    constexpr int W = 256, H = 256;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    Framebuffer fbo;
    ASSERT_TRUE(fbo.resize(W, H));
    fbo.bind();
    const ProjectorCamera cam = ProjectorCamera::framing(
        Vec3(0.0f), Vec3(1.0f, 1.0f, 0.0f), ProjectorIntrinsics::fromThrowRatio(1.5f, W, H));
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.2f));  // something on screen meanwhile
    objects.push_back(std::make_unique<MeshObject>());

    ObjLoadQueue queue;
    auto frame = [&] {
        for (auto& done : queue.takeCompleted()) {
            if (done.mesh) objects[1]->setMesh(std::move(done.mesh.value()));
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.renderMeshObjects(objects, cam, {});
        renderer.endFrame();
        glFinish();
    };
    double idleMax = 0.0;
    for (int i = 0; i < 30; ++i) {
        const auto t = Clock::now();
        frame();
        idleMax = std::max(idleMax, ms(Clock::now() - t));
    }

    queue.request(file);
    double loadingMax = 0.0, adoptFrameMs = 0.0;
    std::vector<double> loading;
    int frames = 0;
    const auto l0 = Clock::now();
    while (objects[1]->getMesh().empty() && ms(Clock::now() - l0) < 600000.0) {
        const auto t = Clock::now();
        frame();
        const double f = ms(Clock::now() - t);
        ++frames;
        if (objects[1]->getMesh().empty()) {
            loadingMax = std::max(loadingMax, f);
            loading.push_back(f);
        } else {
            adoptFrameMs = f;
        }
    }
    const double loadMs = ms(Clock::now() - l0);
    double afterMax = 0.0;
    for (int i = 0; i < 30; ++i) {
        const auto t = Clock::now();
        frame();
        afterMax = std::max(afterMax, ms(Clock::now() - t));
    }
    std::sort(loading.begin(), loading.end());
    const double p99 = loading.empty() ? 0.0 : loading[loading.size() * 99 / 100];
    std::printf("[TD-16a] 2M-triangle OBJ, Debug:\n"
                "  synchronous loadObj on the render thread: %.0f ms (one frame)\n"
                "  worker: %d frames over %.0f ms; while parsing max %.1f ms, p99 %.2f ms "
                "(idle max %.1f ms); adopt+upload frame %.1f ms; max after %.1f ms\n",
                syncMs, frames, loadMs, loadingMax, p99, idleMax, adoptFrameMs, afterMax);
    EXPECT_FALSE(objects[1]->getMesh().empty());
    EXPECT_LT(loadingMax, syncMs);

    Framebuffer::unbind();
    renderer.shutdown();
    std::error_code ec;
    fs::remove(file, ec);
}
