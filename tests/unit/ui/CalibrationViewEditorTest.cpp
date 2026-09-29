// Drives the ui ViewportEditor, so it lives apart from editor/SceneTakeRoutingTest.cpp
// (ADR-051 E4).
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/SliceEditing.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "scene/MeshObject.hpp"
#include "ui/panels/ViewportEditor.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace quadmapper;

// ---------------------------------------------------------------------------
// The calibration view on the editor canvas (TD-29, ADR-047 Amendment 6): the real
// ViewportEditor::render, in a hidden GL 3.3 window with an ImGui frame, while an output
// is calibrated. The canvas must show that output's frame as the projector gets it
// (OutputManager::outputFrame, un-turned), wherever the view's zoom and pan put it, and
// a press on it must land on the projector pixel it shows. Skips without GL.
// ---------------------------------------------------------------------------

namespace {

// Hidden editor window with a GL 3.3 core context and an ImGui context, as Application
// has when it draws the canvas, RAII; ok == false means no GL.
struct EditorWindow {
    GLFWwindow* win = nullptr;
    ImGuiContext* imgui = nullptr;
    bool ok = false;
    EditorWindow() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        win = glfwCreateWindow(400, 300, "calibration-view-editor", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        imgui = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(400.0f, 300.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ok = true;
    }
    ~EditorWindow() {
        if (imgui) ImGui::DestroyContext(imgui);
        if (win) { glfwDestroyWindow(win); glfwTerminate(); }
    }
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;
};

// A texture with detail every few texels, sampled nearest (so a misplaced view shows).
std::unique_ptr<Texture> checkerTexture(int size, int seed) {
    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            unsigned char* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = static_cast<unsigned char>(((x / 4) * 60 + seed * 40) & 255);
            p[1] = static_cast<unsigned char>(((y / 4) * 70 + seed * 90) & 255);
            p[2] = static_cast<unsigned char>((((x / 4) ^ (y / 4)) * 50 + seed) & 255);
            p[3] = 255;
        }
    }
    auto texture = std::make_unique<Texture>();
    texture->create(size, size, 4, px.data());
    texture->setFilterMode(false);
    return texture;
}

std::vector<unsigned char> readBoundPixels(int w, int h) {
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

// One calibrated output: its turn, the scene's canvas, its one slice (input rectangle,
// warp, left blend width), the view's zoom and pan, and its window size (the
// resolution stays 320x180).
struct ViewCase {
    const char* name;
    Rotation rotation;
    IVec2 canvas;
    CanvasRect input;
    std::array<Vec2, 4> warp;
    float blendLeft;
    float zoom;
    Vec2 pan;
    IVec2 windowSize;
};

const std::array<Vec2, 4> KEYSTONE = {Vec2(0.08f, 0.04f), Vec2(0.97f, 0.0f), Vec2(0.88f, 1.0f),
                                      Vec2(0.0f, 0.93f)};
const std::array<Vec2, 4> UNWARPED = {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f),
                                      Vec2(0.0f, 1.0f)};

std::vector<ViewCase> viewCases() {
    return {
        {"R0 default slicing", Rotation::R0, IVec2(320, 180), CanvasRect{0, 0, 320, 180},
         UNWARPED, 0.0f, 1.0f, Vec2(0.0f), IVec2(320, 180)},
        {"R0 offset keystoned slice of a two-frame canvas, zoom 1.4 and pan", Rotation::R0,
         IVec2(640, 180), CanvasRect{200, 18, 320, 144}, KEYSTONE, 0.0f, 1.4f,
         Vec2(0.06f, -0.04f), IVec2(320, 180)},
        {"R90 blended keystoned slice, zoom 0.8 and pan", Rotation::R90, IVec2(300, 320),
         CanvasRect{60, 0, 216, 320}, KEYSTONE, 60.0f, 0.8f, Vec2(-0.03f, 0.02f),
         IVec2(320, 180)},
        {"R270 keystoned, window 256x200 unlike the resolution", Rotation::R270,
         IVec2(180, 320), CanvasRect{0, 0, 180, 320}, KEYSTONE, 0.0f, 1.2f,
         Vec2(0.02f, 0.03f), IVec2(256, 200)},
        {"R180 offset blended slice", Rotation::R180, IVec2(640, 180),
         CanvasRect{300, 0, 320, 180}, KEYSTONE, 40.0f, 1.0f, Vec2(0.0f), IVec2(320, 180)},
    };
}

// The editor with one output calibrated: two textured quads and a cube in the active
// scene, the output showing it through the case's slice, its projector fitted to the
// cube, and the output frames drawn once (the view draws from their canvas).
struct CalibrationViewRig {
    GLFWwindow* win;
    EditorState es;
    ViewportEditor viewport;  // owned by EditorUi in the app (ADR-051 E5)
    OutputManager om;
    OutputWindow* output = nullptr;
    Renderer renderer;
    std::unique_ptr<Texture> mediaA, mediaB;
    std::unordered_map<UUID, Texture*> textures;
    Framebuffer editor{Framebuffer::Depth::With};
    int fbWidth = 0, fbHeight = 0, winWidth = 0, winHeight = 0;
    bool ok = false;

    CalibrationViewRig(GLFWwindow* window, const ViewCase& c) : win(window) {
        if (!om.initialize(win).isSuccess()) return;
        es.outputResolver.setOutputManager(&om);
        output = om.createOutput("Calibrated");
        if (!output) return;
        output->setResolution(IVec2(320, 180));
        output->setRotation(c.rotation);
        glfwSetWindowSize(output->getGLFWwindow(), c.windowSize.x, c.windowSize.y);
        glfwMakeContextCurrent(win);

        Scene& scene = es.activeScene();
        scene.setCanvasSize(c.canvas);
        mediaA = checkerTexture(32, 1);
        mediaB = checkerTexture(32, 2);
        Quad* a = scene.getQuadManager().createQuad("A");
        a->getGeometry().setCorners({Vec2(0.30f, 0.10f), Vec2(0.70f, 0.15f), Vec2(0.65f, 0.60f),
                                     Vec2(0.28f, 0.55f)});
        Quad* b = scene.getQuadManager().createQuad("B");
        b->getGeometry().setCorners({Vec2(0.45f, 0.50f), Vec2(0.95f, 0.45f), Vec2(0.90f, 0.95f),
                                     Vec2(0.50f, 0.92f)});
        textures[a->getId()] = mediaA.get();
        textures[b->getId()] = mediaB.get();
        MeshObject* cube = scene.createObject("Cube");
        MeshData& data = cube->editMesh();
        const float h = 0.15f;
        for (int k = 0; k < 8; ++k) {
            data.positions.push_back(Vec3((k & 1) ? h : -h, (k & 2) ? h : -h, (k & 4) ? h : -h));
        }
        data.indices = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                        2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
        cube->setTransform(glm::rotate(glm::mat4(1.0f), 0.6f, Vec3(0.3f, 1.0f, 0.2f)));

        OutputManager::showScene(*output, &scene, es.scenes);
        Slice slice = makeFullCanvasSlice(c.canvas);
        slice.input = c.input;
        slice.warp.setCorners({c.warp[0], c.warp[1], c.warp[2], c.warp[3]});
        slice.blend[SLICE_EDGE_LEFT].width = c.blendLeft;
        output->getSlices() = {slice};
        const IVec2 frame = output->getEffectiveResolution();
        Vec3 lo, hi;
        if (!scene.visibleObjectBounds(lo, hi)) return;
        output->getProjector() =
            ProjectorCamera::fitToOutput(ProjectorCamera{}, frame.x, frame.y, &lo, &hi);

        if (!renderer.initialize()) return;
        om.renderOutputFrames(es.scenes, textures, renderer);
        es.beginCalibration(*output);

        glfwGetFramebufferSize(win, &fbWidth, &fbHeight);
        glfwGetWindowSize(win, &winWidth, &winHeight);
        if (!editor.resize(fbWidth, fbHeight)) return;
        es.canvasRegionPos = Vec2(20.0f, 30.0f);
        es.canvasRegionSize = Vec2(static_cast<float>(winWidth) - 40.0f,
                                   static_cast<float>(winHeight) - 50.0f);
        es.canvasZoom = c.zoom;
        es.canvasPan = c.pan;
        ok = true;
    }
    ~CalibrationViewRig() {
        glfwMakeContextCurrent(win);
        renderer.shutdown();
        mediaA.reset();
        mediaB.reset();
        om.shutdown();
    }
    CalibrationViewRig(const CalibrationViewRig&) = delete;
    CalibrationViewRig& operator=(const CalibrationViewRig&) = delete;

    // The projector's own frame, as it is sent to the output window (turned).
    std::vector<unsigned char> projectorFrame(int& width, int& height) const {
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        om.outputFrame(output->getId())->bind();
        std::vector<unsigned char> px = readBoundPixels(width, height);
        Framebuffer::unbind();
        return px;
    }

    // One editor frame over the editor's grey background, with the mouse at `pointer`;
    // returns the editor framebuffer.
    std::vector<unsigned char> frame(const ViewportEditor::Pointer& pointer) {
        editor.bind();
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        ImGui::NewFrame();
        EditorContext ctx{es, nullptr, nullptr, nullptr, &om, &renderer, win};
        viewport.render(ctx, pointer);
        ImGui::EndFrame();
        std::vector<unsigned char> px = readBoundPixels(fbWidth, fbHeight);
        Framebuffer::unbind();
        return px;
    }

    // Where the view shows the point `f` of the output frame ([0,1], +y down), in window
    // coordinates: the view transform of the editor's last frame.
    Vec2 windowPoint(const Vec2& f) const {
        return es.canvasRegionPos +
               (es.lastViewOffset + es.lastViewScale * f) * es.canvasRegionSize;
    }
};

// The frame point `f` (+y down, un-turned) -> that point of the turned frame.
Vec2 turned(Rotation rotation, const Vec2& f) {
    switch (rotation) {
    case Rotation::R90: return Vec2(1.0f - f.y, f.x);
    case Rotation::R180: return Vec2(1.0f - f.x, 1.0f - f.y);
    case Rotation::R270: return Vec2(f.y, 1.0f - f.x);
    default: return f;
    }
}

}  // namespace

// The canvas shows the calibrated output's frame, whatever its slicing, turn and window
// size and the view's zoom and pan: the frame keeps its aspect on the canvas; every
// editor pixel inside it (through the view's scale and offset) matches the projector
// frame's pixel there, away from edges in the picture; the frame shows the quads and
// the cube; and the canvas region around it keeps the editor's background.
TEST(CalibrationViewEditorTest, TheCanvasShowsTheOutputFrameWhateverTheSlicing) {
    EditorWindow gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    for (const ViewCase& c : viewCases()) {
        SCOPED_TRACE(c.name);
        CalibrationViewRig rig(gl.win, c);
        ASSERT_TRUE(rig.ok);
        ASSERT_EQ(calibrationViewOutput(rig.es, &rig.om), rig.output);
        int fw = 0, fh = 0;
        const std::vector<unsigned char> truth = rig.projectorFrame(fw, fh);
        const std::vector<unsigned char> view = rig.frame(ViewportEditor::Pointer{});

        const int W = rig.fbWidth, H = rig.fbHeight;
        const float dpiX = static_cast<float>(W) / static_cast<float>(rig.winWidth);
        const float dpiY = static_cast<float>(H) / static_cast<float>(rig.winHeight);
        const Vec2 scale = rig.es.lastViewScale, offset = rig.es.lastViewOffset;
        const Vec2 regionPos = rig.es.canvasRegionPos, regionSize = rig.es.canvasRegionSize;
        const IVec2 frameSize = rig.output->getEffectiveResolution();
        EXPECT_NEAR((scale.x * regionSize.x) / (scale.y * regionSize.y),
                    static_cast<float>(frameSize.x) / static_cast<float>(frameSize.y), 1e-3f)
            << "the frame keeps its shape on the canvas";
        int compared = 0, lit = 0, mismatched = 0, around = 0, aroundBlack = 0;
        for (int gy = 0; gy < H; ++gy) {
            for (int gx = 0; gx < W; ++gx) {
                // The pixel's centre in window coordinates, then on the frame.
                const float lx = (static_cast<float>(gx) + 0.5f) / dpiX;
                const float ly = static_cast<float>(rig.winHeight) - (static_cast<float>(gy) + 0.5f) / dpiY;
                const Vec2 vn((lx - regionPos.x) / regionSize.x, (ly - regionPos.y) / regionSize.y);
                if (vn.x < 0.0f || vn.x > 1.0f || vn.y < 0.0f || vn.y > 1.0f) continue;
                const Vec2 f = (vn - offset) / scale;
                const unsigned char* a = &view[(static_cast<size_t>(gy) * W + gx) * 4];
                if (f.x < -0.02f || f.x > 1.02f || f.y < -0.02f || f.y > 1.02f) {
                    ++around;
                    if (a[0] < 8 && a[1] < 8 && a[2] < 8) ++aroundBlack;
                    continue;
                }
                if (f.x < 0.01f || f.x > 0.99f || f.y < 0.01f || f.y > 0.99f) continue;
                const Vec2 t = turned(c.rotation, f);
                const int px = std::min(static_cast<int>(t.x * static_cast<float>(fw)), fw - 1);
                const int py = std::min(static_cast<int>((1.0f - t.y) * static_cast<float>(fh)), fh - 1);
                const unsigned char* b = &truth[(static_cast<size_t>(py) * fw + px) * 4];
                // Skip edges in the projector frame, where the two samplings differ.
                bool edge = false;
                for (int dy = -2; dy <= 2 && !edge; ++dy) {
                    for (int dx = -2; dx <= 2 && !edge; ++dx) {
                        const int qx = std::clamp(px + dx, 0, fw - 1);
                        const int qy = std::clamp(py + dy, 0, fh - 1);
                        const unsigned char* q = &truth[(static_cast<size_t>(qy) * fw + qx) * 4];
                        edge = std::abs(q[0] - b[0]) + std::abs(q[1] - b[1]) + std::abs(q[2] - b[2]) > 30;
                    }
                }
                if (edge) continue;
                ++compared;
                if (b[0] + b[1] + b[2] > 30) ++lit;
                const int d = std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
                if (d > 24) ++mismatched;
            }
        }
        EXPECT_GT(compared, W * H / 10) << "the frame must fill much of the canvas";
        EXPECT_GT(lit, compared / 10) << "the frame must show the quads and the cube";
        EXPECT_LE(mismatched, compared / 100) << "of " << compared << " pixels compared";
        EXPECT_EQ(aroundBlack, 0) << "of " << around << " pixels around the frame";
    }
}

// A press on the calibration view lands on the projector pixel the view shows there:
// Add Point's second click places the point at that pixel, and a drag moves it by the
// projector pixels the mouse moved over, whatever the slicing, turn, zoom and pan.
TEST(CalibrationViewEditorTest, PicksOnTheCanvasLandOnTheProjectorPixelsShown) {
    EditorWindow gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    for (const ViewCase& c : viewCases()) {
        SCOPED_TRACE(c.name);
        CalibrationViewRig rig(gl.win, c);
        ASSERT_TRUE(rig.ok);
        rig.frame(ViewportEditor::Pointer{});  // the view transform for this output
        const ProjectorIntrinsics& k = rig.output->getProjector().intrinsics;
        const Vec2 size(static_cast<float>(k.width), static_cast<float>(k.height));
        auto pointerAt = [&](const Vec2& pixel, bool down) {
            return ViewportEditor::Pointer{rig.windowPoint(pixel / size), down};
        };
        const Vec2 target(0.42f * size.x + 0.3f, 0.57f * size.y + 0.6f);

        CalibrationSession& cal = rig.es.calibration;
        cal.pick = CalibrationSession::Pick::Pixel;
        cal.pendingWorld = Vec3(0.1f, 0.2f, 0.3f);
        rig.frame(pointerAt(target, true));
        rig.frame(pointerAt(target, false));
        const auto& points = rig.output->getCalibrationPoints();
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].world, Vec3(0.1f, 0.2f, 0.3f));
        EXPECT_NEAR(points[0].pixel.x, target.x, 0.01f);
        EXPECT_NEAR(points[0].pixel.y, target.y, 0.01f);

        const Vec2 moved = target + Vec2(12.0f, -7.0f);
        rig.frame(pointerAt(target, true));
        rig.frame(pointerAt(moved, true));
        rig.frame(pointerAt(moved, false));
        EXPECT_NEAR(points[0].pixel.x, moved.x, 0.01f);
        EXPECT_NEAR(points[0].pixel.y, moved.y, 0.01f);
    }
}
