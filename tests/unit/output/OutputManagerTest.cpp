#include <gtest/gtest.h>
#include "output/OutputManager.hpp"
#include "core/GlObjectStats.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "scene/CalibrationSession.hpp"
#include "scene/MeshObject.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Scene.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

namespace quadmapper {
namespace test {

// Tests for OutputManager (non-GLFW tests)
// GLFW-dependent functionality requires integration tests with an OpenGL context

TEST(OutputManagerTest, DefaultConstructorCreatesValidManager) {
    OutputManager manager;

    EXPECT_FALSE(manager.isInitialized());
    EXPECT_FALSE(manager.isInPerformMode());
    EXPECT_EQ(manager.getOutputCount(), 0);
}

TEST(OutputManagerTest, InitializeWithNullWindowFails) {
    OutputManager manager;

    auto result = manager.initialize(nullptr);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_FALSE(manager.isInitialized());
}

TEST(OutputManagerTest, CreateOutputWithoutInitReturnsOutputButNotCreated) {
    OutputManager manager;
    // Manager not initialized, so GLFW window won't be created

    OutputWindow* output = manager.createOutput("Test Output");

    // Output pointer should be valid (data structure created)
    EXPECT_NE(output, nullptr);
    EXPECT_EQ(output->getName(), "Test Output");
    EXPECT_EQ(manager.getOutputCount(), 1);

    // But GLFW window should not be created
    EXPECT_FALSE(output->isCreated());
}

TEST(OutputManagerTest, CreateOutputAutoGeneratesUniqueName) {
    OutputManager manager;

    OutputWindow* output1 = manager.createOutput();
    OutputWindow* output2 = manager.createOutput();
    OutputWindow* output3 = manager.createOutput();

    EXPECT_EQ(output1->getName(), "Output 1");
    EXPECT_EQ(output2->getName(), "Output 2");
    EXPECT_EQ(output3->getName(), "Output 3");
}

TEST(OutputManagerTest, CreateOutputWithCustomName) {
    OutputManager manager;

    OutputWindow* output = manager.createOutput("Projector A");

    EXPECT_EQ(output->getName(), "Projector A");
}

TEST(OutputManagerTest, GetOutputById) {
    OutputManager manager;
    OutputWindow* created = manager.createOutput("Test");
    UUID id = created->getId();

    OutputWindow* retrieved = manager.getOutput(id);

    EXPECT_EQ(retrieved, created);
    EXPECT_EQ(retrieved->getName(), "Test");
}

TEST(OutputManagerTest, GetOutputByIdNotFound) {
    OutputManager manager;
    manager.createOutput("Test");
    UUID unknownId = UUID::generate();

    OutputWindow* retrieved = manager.getOutput(unknownId);

    EXPECT_EQ(retrieved, nullptr);
}

TEST(OutputManagerTest, GetOutputConstVersion) {
    OutputManager manager;
    OutputWindow* created = manager.createOutput("Test");
    UUID id = created->getId();

    const OutputManager& constManager = manager;
    const OutputWindow* retrieved = constManager.getOutput(id);

    EXPECT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->getName(), "Test");
}

TEST(OutputManagerTest, DeleteOutput) {
    OutputManager manager;
    OutputWindow* output = manager.createOutput("Test");
    UUID id = output->getId();

    EXPECT_EQ(manager.getOutputCount(), 1);

    manager.deleteOutput(id);

    EXPECT_EQ(manager.getOutputCount(), 0);
    EXPECT_EQ(manager.getOutput(id), nullptr);
}

TEST(OutputManagerTest, DeleteNonexistentOutputIsNoOp) {
    OutputManager manager;
    manager.createOutput("Test");
    UUID unknownId = UUID::generate();

    manager.deleteOutput(unknownId);

    EXPECT_EQ(manager.getOutputCount(), 1);
}

TEST(OutputManagerTest, GetOutputsReturnsAllOutputs) {
    OutputManager manager;
    manager.createOutput("A");
    manager.createOutput("B");
    manager.createOutput("C");

    const auto& outputs = manager.getOutputs();

    EXPECT_EQ(outputs.size(), 3);
}

TEST(OutputManagerTest, PerformModeInitiallyFalse) {
    OutputManager manager;

    EXPECT_FALSE(manager.isInPerformMode());
}

TEST(OutputManagerTest, EnterPerformModeFailsWithNoOutputs) {
    OutputManager manager;

    auto result = manager.enterPerformMode();

    EXPECT_FALSE(result.isSuccess());
    EXPECT_FALSE(manager.isInPerformMode());
}

// An export running refuses perform mode for every caller, and says why.
TEST(OutputManagerTest, APerformRefusalKeepsPerformModeOffAndIsShown) {
    OutputManager manager;
    std::string notice;
    manager.setNoticeCallback([&notice](const std::string& message) { notice = message; });
    manager.setPerformRefusal([] { return std::string("an export is running"); });

    auto result = manager.enterPerformMode();

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().message, "an export is running");
    EXPECT_EQ(notice, "an export is running");
    EXPECT_FALSE(manager.isInPerformMode());

    manager.setPerformRefusal([] { return std::string(); });  // nothing to refuse for
    EXPECT_NE(manager.enterPerformMode().error().message, "an export is running");
}

TEST(OutputManagerTest, ExitPerformModeSucceedsWhenNotInPerformMode) {
    OutputManager manager;

    auto result = manager.exitPerformMode();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(manager.isInPerformMode());
}

TEST(OutputManagerTest, GetMainWindowNullWhenNotInitialized) {
    OutputManager manager;

    EXPECT_EQ(manager.getMainWindow(), nullptr);
}

TEST(OutputManagerTest, GetMonitorCountReturnsZeroWhenNotInitialized) {
    OutputManager manager;

    // Monitors list is empty when not initialized
    EXPECT_EQ(manager.getMonitorCount(), 0);
}

TEST(OutputManagerTest, GetMonitorsReturnsEmptyWhenNotInitialized) {
    OutputManager manager;

    const auto& monitors = manager.getMonitors();

    EXPECT_TRUE(monitors.empty());
}

TEST(OutputManagerTest, ShutdownClearsOutputs) {
    OutputManager manager;
    manager.createOutput("A");
    manager.createOutput("B");

    EXPECT_EQ(manager.getOutputCount(), 2);

    manager.shutdown();

    EXPECT_EQ(manager.getOutputCount(), 0);
    EXPECT_FALSE(manager.isInitialized());
}

TEST(OutputManagerTest, OutputFramesAreSafeWhenNotInitialized) {
    OutputManager manager;
    OutputWindow* output = manager.createOutput("Test");
    ASSERT_NE(output, nullptr);

    // Should not crash with no main window and no created outputs (no GL context
    // here): nothing is drawn and nothing is presented.
    std::vector<std::unique_ptr<Scene>> scenes;
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;  // never initialized
    manager.renderOutputFrames(scenes, textures, renderer);
    manager.presentOutputs();
    EXPECT_EQ(manager.outputFrame(output->getId()), nullptr);
}

TEST(OutputManagerTest, SceneForOutputRoutesByTheOutputsSceneId) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    const Scene* a = scenes[0].get();
    OutputWindow outA, outB, outC, outD;
    outA.setSceneId(a->getId());
    outB.setSceneId(a->getId());          // several outputs may show one scene (ADR-047)
    outD.setSceneId(UUID::generate());    // a scene that no longer exists

    EXPECT_EQ(OutputManager::sceneForOutput(scenes, outA), a);
    EXPECT_EQ(OutputManager::sceneForOutput(scenes, outB), a);
    EXPECT_EQ(OutputManager::sceneForOutput(scenes, outC), nullptr);  // shows nothing
    EXPECT_EQ(OutputManager::sceneForOutput(scenes, outD), nullptr);
}

// Routing a scene gives an output without slices one slice of the whole canvas; an
// output's slices keep their place on the picture when it changes to a scene with
// another canvas size; routing nothing leaves the slices for the next scene.
TEST(OutputManagerTest, ShowSceneGivesAFullCanvasSliceAndRescalesSlices) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->setCanvasSize(IVec2(1920, 1080));
    scenes[1]->setCanvasSize(IVec2(3840, 2160));
    OutputWindow output;

    OutputManager::showScene(output, scenes[0].get(), scenes);
    EXPECT_EQ(output.getSceneId(), scenes[0]->getId());
    ASSERT_EQ(output.getSlices().size(), 1u);
    EXPECT_EQ(output.getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}));
    EXPECT_TRUE(isIdentityWarp(output.getSlices()[0].warp));
    EXPECT_FALSE(output.getSlices()[0].id.isNull());

    output.getSlices()[0].input = CanvasRect{960.0f, 0.0f, 960.0f, 1080.0f};  // right half
    output.getSlices()[0].blend[SLICE_EDGE_LEFT].width = 100.0f;
    OutputManager::showScene(output, scenes[1].get(), scenes);
    ASSERT_EQ(output.getSlices().size(), 1u);
    EXPECT_EQ(output.getSlices()[0].input, (CanvasRect{1920.0f, 0.0f, 1920.0f, 2160.0f}));
    EXPECT_FLOAT_EQ(output.getSlices()[0].blend[SLICE_EDGE_LEFT].width, 200.0f);

    OutputManager::showScene(output, nullptr, scenes);
    EXPECT_TRUE(output.getSceneId().isNull());
    EXPECT_EQ(output.getSlices().size(), 1u);
}

// A lone output's default slice keeps the canvas at the output's size through a
// resolution or rotation change; a blended group's canvas does not move.
TEST(OutputManagerTest, CanvasFollowsALoneOutputsSize) {
    OutputManager manager;
    OutputWindow* lone = manager.createOutput("Lone");
    OutputWindow* left = manager.createOutput("Left");
    OutputWindow* right = manager.createOutput("Right");
    ASSERT_NE(lone, nullptr);
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    scenes[1]->setCanvasSize(IVec2(3840, 1080));
    OutputManager::showScene(*lone, scenes[0].get(), scenes);
    OutputManager::showScene(*left, scenes[1].get(), scenes);
    OutputManager::showScene(*right, scenes[1].get(), scenes);
    EXPECT_FALSE(manager.followOutputSizes(scenes));  // first sight: sizes recorded

    lone->setRotation(Rotation::R90);
    left->setResolution(IVec2(1280, 720));
    EXPECT_TRUE(manager.followOutputSizes(scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(1080, 1920));
    EXPECT_EQ(lone->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1080.0f, 1920.0f}));
    EXPECT_EQ(scenes[1]->getCanvasSize(), IVec2(3840, 1080));  // two outputs show it

    // A slice that was edited (a blend zone) keeps its canvas too.
    lone->getSlices()[0].blend[SLICE_EDGE_RIGHT].width = 50.0f;
    lone->setRotation(Rotation::R0);
    EXPECT_FALSE(manager.followOutputSizes(scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(1080, 1920));
}

// Slices remember the canvas they were made for, so an output that showed no scene
// in between (Inspector "(no scene)", a Scenes-panel toggle, a deleted scene) still
// rescales them onto the next scene's canvas.
TEST(OutputManagerTest, SlicesFollowTheNextSceneAfterShowingNone) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->setCanvasSize(IVec2(1920, 1080));
    scenes[1]->setCanvasSize(IVec2(1280, 800));
    OutputWindow output;
    OutputManager::showScene(output, scenes[0].get(), scenes);
    OutputManager::showScene(output, nullptr, scenes);
    OutputManager::showScene(output, scenes[1].get(), scenes);
    ASSERT_EQ(output.getSlices().size(), 1u);
    EXPECT_EQ(output.getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1280.0f, 800.0f}));

    // An edited slice (the right half) keeps its place on the picture as well.
    output.getSlices()[0].input = CanvasRect{640.0f, 0.0f, 640.0f, 800.0f};
    OutputManager::showScene(output, nullptr, scenes);
    OutputManager::showScene(output, scenes[0].get(), scenes);
    EXPECT_EQ(output.getSlices()[0].input, (CanvasRect{960.0f, 0.0f, 960.0f, 1080.0f}));
}

TEST(OutputManagerTest, SlicesFollowTheNextSceneAfterTheirSceneIsDeleted) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->setCanvasSize(IVec2(1920, 1080));
    scenes[1]->setCanvasSize(IVec2(1280, 800));
    OutputWindow output;
    OutputManager::showScene(output, scenes[0].get(), scenes);
    scenes.erase(scenes.begin());  // the output's sceneId now names no scene
    OutputManager::showScene(output, scenes[0].get(), scenes);
    ASSERT_EQ(output.getSlices().size(), 1u);
    EXPECT_EQ(output.getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1280.0f, 800.0f}));
}

// Routing a scene onto an output alone, through its default slice, gives the canvas
// the output's effective resolution: nothing is resampled, and the editor has the
// projector's aspect. A group, or an edited slice, keeps the canvas.
TEST(OutputManagerTest, RoutingOntoALoneOutputAdoptsItsResolution) {
    OutputManager manager;
    OutputWindow* lone = manager.createOutput("Lone");
    OutputWindow* left = manager.createOutput("Left");
    OutputWindow* right = manager.createOutput("Right");
    lone->setResolution(IVec2(1280, 800));
    lone->setRotation(Rotation::R90);
    std::vector<std::unique_ptr<Scene>> scenes;
    for (int i = 0; i < 3; ++i) scenes.push_back(std::make_unique<Scene>());
    scenes[1]->setCanvasSize(IVec2(3200, 1080));

    EXPECT_TRUE(manager.routeScene(*lone, scenes[0].get(), scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(800, 1280));
    EXPECT_EQ(lone->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 800.0f, 1280.0f}));
    EXPECT_FALSE(manager.routeScene(*lone, scenes[0].get(), scenes));  // already its size

    manager.routeScene(*left, scenes[1].get(), scenes);   // alone for now: adopts 1920x1080
    EXPECT_EQ(scenes[1]->getCanvasSize(), IVec2(1920, 1080));
    scenes[1]->setCanvasSize(IVec2(3200, 1080));           // the wide canvas, both sliced
    left->getSlices()[0].input = CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f};
    left->setSlicesCanvas(scenes[1]->getCanvasSize());
    EXPECT_FALSE(manager.routeScene(*right, scenes[1].get(), scenes));
    EXPECT_EQ(scenes[1]->getCanvasSize(), IVec2(3200, 1080)) << "shown by two outputs";

    // An edited slice keeps its canvas.
    lone->getSlices()[0].blend[SLICE_EDGE_LEFT].width = 20.0f;
    scenes[2]->setCanvasSize(IVec2(800, 1280));
    EXPECT_FALSE(manager.routeScene(*lone, scenes[2].get(), scenes));
    lone->setResolution(IVec2(1920, 1080));
    lone->setRotation(Rotation::R0);
    EXPECT_FALSE(manager.routeScene(*lone, scenes[2].get(), scenes));
    EXPECT_EQ(scenes[2]->getCanvasSize(), IVec2(800, 1280));
}

// A canvas the operator set (Scenes panel, Scene::isCanvasLocked) is never replaced
// by a projector's resolution: not by routing, not when the output's size changes
// (ADR-047 Amendment 3). Unlocked, the same scene adopts as before.
TEST(OutputManagerTest, ALockedCanvasIsNeverAdopted) {
    OutputManager manager;
    OutputWindow* lone = manager.createOutput("Lone");
    lone->setResolution(IVec2(1280, 800));
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes[0]->setCanvasSize(IVec2(2560, 1440));
    scenes[0]->setCanvasLocked(true);

    EXPECT_FALSE(manager.routeScene(*lone, scenes[0].get(), scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(2560, 1440));
    EXPECT_TRUE(isDefaultSlicing(lone->getSlices(), IVec2(2560, 1440)));

    EXPECT_FALSE(manager.followOutputSizes(scenes));  // first sight
    lone->setResolution(IVec2(1920, 1080));
    EXPECT_FALSE(manager.followOutputSizes(scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(2560, 1440));

    // A locked canvas of the output's own size stays when the output changes.
    scenes[0]->setCanvasSize(IVec2(1920, 1080));
    OutputManager::showScene(*lone, scenes[0].get(), scenes);
    lone->setResolution(IVec2(1280, 720));
    EXPECT_FALSE(manager.followOutputSizes(scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(1920, 1080));

    // Unlocked, routing adopts again.
    scenes[0]->setCanvasLocked(false);
    EXPECT_TRUE(manager.routeScene(*lone, scenes[0].get(), scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(1280, 720));
}

// F11 puts an output on its monitor's mode and exit restores the windowed size. A
// lone default canvas follows both, but neither is a change to save: no "unsaved
// changes" prompt for entering and leaving perform mode.
TEST(OutputManagerTest, FullscreenEnterAndExitFollowButDoNotModifyTheShow) {
    OutputManager manager;
    OutputWindow* output = manager.createOutput("P1");
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    OutputManager::showScene(*output, scenes[0].get(), scenes);  // 1920x1080 both
    EXPECT_FALSE(manager.followOutputSizes(scenes));

    output->setFullscreen(true);  // what goFullscreen does, with the monitor's mode
    output->setResolution(IVec2(1280, 800));
    EXPECT_FALSE(manager.followOutputSizes(scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(1280, 800)) << "followed all the same";

    output->setFullscreen(false);  // goWindowed
    output->setResolution(IVec2(1920, 1080));
    EXPECT_FALSE(manager.followOutputSizes(scenes));
    EXPECT_EQ(scenes[0]->getCanvasSize(), IVec2(1920, 1080));

    output->setResolution(IVec2(1280, 720));  // the operator's own edit is saved
    EXPECT_TRUE(manager.followOutputSizes(scenes));
}

TEST(OutputManagerTest, CreateMultipleOutputsAndDelete) {
    OutputManager manager;

    OutputWindow* out1 = manager.createOutput("Output 1");
    OutputWindow* out2 = manager.createOutput("Output 2");
    OutputWindow* out3 = manager.createOutput("Output 3");

    UUID id2 = out2->getId();

    EXPECT_EQ(manager.getOutputCount(), 3);

    manager.deleteOutput(id2);

    EXPECT_EQ(manager.getOutputCount(), 2);
    EXPECT_NE(manager.getOutput(out1->getId()), nullptr);
    EXPECT_EQ(manager.getOutput(id2), nullptr);
    EXPECT_NE(manager.getOutput(out3->getId()), nullptr);
}

// MonitorInfo struct tests
TEST(MonitorInfoTest, DefaultValues) {
    MonitorInfo info;

    EXPECT_TRUE(info.name.empty());
    EXPECT_EQ(info.position.x, 0);
    EXPECT_EQ(info.position.y, 0);
    EXPECT_EQ(info.resolution.x, 0);
    EXPECT_EQ(info.resolution.y, 0);
    EXPECT_EQ(info.refreshRate, 60);
    EXPECT_FALSE(info.isPrimary);
    EXPECT_EQ(info.index, -1);
}

TEST(MonitorInfoTest, SetValues) {
    MonitorInfo info;
    info.name = "DELL U2723QE";
    info.position = IVec2(1920, 0);
    info.resolution = IVec2(3840, 2160);
    info.refreshRate = 144;
    info.isPrimary = true;
    info.index = 1;

    EXPECT_EQ(info.name, "DELL U2723QE");
    EXPECT_EQ(info.position.x, 1920);
    EXPECT_EQ(info.position.y, 0);
    EXPECT_EQ(info.resolution.x, 3840);
    EXPECT_EQ(info.resolution.y, 2160);
    EXPECT_EQ(info.refreshRate, 144);
    EXPECT_TRUE(info.isPrimary);
    EXPECT_EQ(info.index, 1);
}

// ============================================================================
// Phase 4 QM-022: Monitor Assignment Tests
// ============================================================================

TEST(MonitorInfoTest, GetDisplayStringFormat) {
    MonitorInfo info;
    info.name = "Test Monitor";
    info.resolution = IVec2(1920, 1080);
    info.refreshRate = 60;
    info.isPrimary = false;

    std::string display = info.getDisplayString();

    EXPECT_NE(display.find("Test Monitor"), std::string::npos);
    EXPECT_NE(display.find("1920x1080"), std::string::npos);
    EXPECT_NE(display.find("60Hz"), std::string::npos);
}

TEST(MonitorInfoTest, GetDisplayStringIncludesPrimaryLabel) {
    MonitorInfo info;
    info.name = "Primary Monitor";
    info.resolution = IVec2(2560, 1440);
    info.refreshRate = 144;
    info.isPrimary = true;

    std::string display = info.getDisplayString();

    EXPECT_NE(display.find("[Primary]"), std::string::npos);
}

TEST(MonitorInfoTest, GetDisplayStringNoPrimaryLabel) {
    MonitorInfo info;
    info.name = "Secondary Monitor";
    info.resolution = IVec2(1920, 1080);
    info.refreshRate = 60;
    info.isPrimary = false;

    std::string display = info.getDisplayString();

    EXPECT_EQ(display.find("[Primary]"), std::string::npos);
}

TEST(OutputManagerTest, GetMonitorReturnsNullForInvalidIndex) {
    OutputManager manager;

    EXPECT_EQ(manager.getMonitor(-1), nullptr);
    EXPECT_EQ(manager.getMonitor(0), nullptr);  // No monitors when not initialized
    EXPECT_EQ(manager.getMonitor(100), nullptr);
}

TEST(OutputManagerTest, GetPrimaryMonitorNullWhenEmpty) {
    OutputManager manager;

    EXPECT_EQ(manager.getPrimaryMonitor(), nullptr);
}

TEST(OutputManagerTest, MonitorChangeDetectionInitiallyDisabled) {
    OutputManager manager;

    EXPECT_FALSE(manager.isMonitorChangeDetectionEnabled());
}

TEST(OutputManagerTest, EnableDisableMonitorChangeDetection) {
    OutputManager manager;

    manager.enableMonitorChangeDetection();
    EXPECT_TRUE(manager.isMonitorChangeDetectionEnabled());

    manager.disableMonitorChangeDetection();
    EXPECT_FALSE(manager.isMonitorChangeDetectionEnabled());
}

TEST(OutputManagerTest, EnableMonitorChangeDetectionIdempotent) {
    OutputManager manager;

    manager.enableMonitorChangeDetection();
    manager.enableMonitorChangeDetection();  // Should not cause issues

    EXPECT_TRUE(manager.isMonitorChangeDetectionEnabled());
}

TEST(OutputManagerTest, DisableMonitorChangeDetectionIdempotent) {
    OutputManager manager;

    manager.disableMonitorChangeDetection();
    manager.disableMonitorChangeDetection();  // Should not cause issues

    EXPECT_FALSE(manager.isMonitorChangeDetectionEnabled());
}

TEST(OutputManagerTest, SetMonitorChangeCallbackAcceptsNull) {
    OutputManager manager;

    // Should not crash
    manager.setMonitorChangeCallback(nullptr);
}

TEST(OutputManagerTest, SetMonitorChangeCallbackAcceptsLambda) {
    OutputManager manager;
    bool callbackCalled = false;

    manager.setMonitorChangeCallback([&callbackCalled]() {
        callbackCalled = true;
    });

    // Callback won't be called without GLFW initialization, but should accept it
    EXPECT_FALSE(callbackCalled);  // Not called yet
}

// ============================================================================
// Phase 4 QM-023: Perform Mode Tests
// ============================================================================

TEST(OutputManagerTest, TogglePerformModeFailsWithNoOutputs) {
    OutputManager manager;

    auto result = manager.togglePerformMode();

    // Should fail because there are no outputs
    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputManagerTest, TogglePerformModeCreatesOutputsButNotInPerformMode) {
    OutputManager manager;
    manager.createOutput("Test");

    // Without GLFW init, outputs aren't "created" (no GLFW window)
    // So togglePerformMode should still fail
    auto result = manager.togglePerformMode();

    // Should fail because outputs aren't actually created
    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputManagerTest, HandleEscapeKeyWhenNotInPerformMode) {
    OutputManager manager;

    // Should not crash when not in perform mode
    manager.handleEscapeKey();

    EXPECT_FALSE(manager.isInPerformMode());
}

// TD-13: Esc/F11 on an output window never leave perform mode inside the key callback
// (that would clear the running callback); they leave a request for the main loop.
TEST(OutputManagerTest, OutputWindowEscAndF11OnlyRequestThePerformExit) {
    OutputManager manager;
    EXPECT_FALSE(manager.isPerformExitPending());

    manager.handleOutputWindowKey(GLFW_KEY_ESCAPE, GLFW_RELEASE, 0);
    manager.handleOutputWindowKey(GLFW_KEY_ESCAPE, GLFW_REPEAT, 0);
    EXPECT_FALSE(manager.isPerformExitPending());  // presses only

    manager.handleOutputWindowKey(GLFW_KEY_ESCAPE, GLFW_PRESS, 0);
    EXPECT_TRUE(manager.isPerformExitPending());
    manager.applyPendingPerformExit();
    EXPECT_FALSE(manager.isPerformExitPending());
    EXPECT_FALSE(manager.isInPerformMode());

    manager.handleOutputWindowKey(GLFW_KEY_F11, GLFW_PRESS, 0);
    EXPECT_TRUE(manager.isPerformExitPending());
    manager.applyPendingPerformExit();
    EXPECT_FALSE(manager.isPerformExitPending());
}

TEST(OutputManagerTest, OutputWindowBlackoutAndSceneKeysStillActAtOnce) {
    OutputManager manager;
    int taken = 0;
    manager.setSceneKeyCallback([&taken](int scene) { taken = scene; });

    manager.handleOutputWindowKey(GLFW_KEY_B, GLFW_PRESS, GLFW_MOD_CONTROL);
    EXPECT_TRUE(manager.isBlackout());
    manager.handleOutputWindowKey(GLFW_KEY_3, GLFW_PRESS, 0);
    EXPECT_EQ(taken, 3);
    EXPECT_FALSE(manager.isPerformExitPending());
}

TEST(OutputManagerTest, HandleF11KeyWhenNoOutputs) {
    OutputManager manager;

    // Should not crash even with no outputs
    manager.handleF11Key();

    EXPECT_FALSE(manager.isInPerformMode());
}

TEST(OutputManagerTest, EnterPerformModeWithBorderlessTrue) {
    OutputManager manager;
    manager.createOutput("Test");

    // Without initialization, this will fail but shouldn't crash
    auto result = manager.enterPerformMode(true);

    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputManagerTest, EnterPerformModeWithBorderlessFalse) {
    OutputManager manager;
    manager.createOutput("Test");

    // Without initialization, this will fail but shouldn't crash
    auto result = manager.enterPerformMode(false);

    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputManagerTest, ExitPerformModeSucceedsWhenAlreadyExited) {
    OutputManager manager;

    auto result = manager.exitPerformMode();

    // Should succeed because we're already not in perform mode
    EXPECT_TRUE(result.isSuccess());
}

TEST(OutputManagerTest, IdentifyNeedsAnOutputOnScreen) {
    OutputManager manager;  // not initialized: no output can go fullscreen
    manager.createOutput("Test");
    EXPECT_FALSE(manager.identifyOutputs());
    EXPECT_FALSE(manager.isIdentifying());
    EXPECT_FALSE(manager.isInPerformMode());
    manager.retireIdentify();  // nothing to end
    EXPECT_FALSE(manager.isInPerformMode());
}

// Identify's number: a dark panel, the frame's four edges, then one quad per lit
// segment, all inside the frame and the digits on the panel.
TEST(IdentifyOverlayTest, PanelEdgesAndSevenSegmentDigits) {
    auto bounds = [](const Quad& q) {
        const auto& c = q.getGeometry().getCorners();
        return std::array<float, 4>{c[0].x, c[0].y, c[2].x, c[2].y};  // x0 y0 x1 y1
    };
    const IVec2 hd(1920, 1080);
    for (const auto& [number, segments] : {std::pair{1, 2}, std::pair{8, 7}, std::pair{12, 7}}) {
        const std::vector<Quad> quads = makeIdentifyOverlay(number, hd);
        ASSERT_EQ(quads.size(), 1u + 4u + static_cast<size_t>(segments)) << number;
        EXPECT_EQ(quads[0].getColorCorrection().rgbGain, Vec3(0.0f)) << "the panel is dark";
        EXPECT_LT(quads[0].getOpacity(), 1.0f);
        const auto panel = bounds(quads[0]);
        for (size_t i = 0; i < quads.size(); ++i) {
            const auto b = bounds(quads[i]);
            EXPECT_GE(b[0], 0.0f);
            EXPECT_GE(b[1], 0.0f);
            EXPECT_LE(b[2], 1.0f);
            EXPECT_LE(b[3], 1.0f);
            EXPECT_LT(b[0], b[2]);
            EXPECT_LT(b[1], b[3]);
            if (i >= 5) {  // a segment
                EXPECT_TRUE(quads[i].getColorCorrection().isIdentity()) << "white";
                EXPECT_GE(b[0], panel[0]);
                EXPECT_GE(b[1], panel[1]);
                EXPECT_LE(b[2], panel[2]);
                EXPECT_LE(b[3], panel[3]);
            }
        }
    }
    // A portrait (rotated) frame still fits the number.
    for (const Quad& q : makeIdentifyOverlay(88, IVec2(1080, 1920))) {
        const auto b = bounds(q);
        EXPECT_GE(b[0], 0.0f);
        EXPECT_LE(b[2], 1.0f);
    }
}

TEST(OutputManagerTest, EnterPerformModeIdempotent) {
    OutputManager manager;

    // Calling when already in perform mode (or not) should be safe
    // Without outputs, it will fail, but calling multiple times shouldn't crash
    manager.enterPerformMode();
    manager.enterPerformMode();

    EXPECT_FALSE(manager.isInPerformMode());
}

// ---- Desired vs resolved monitor identity (QM-090) ---------------------------

namespace {
MonitorInfo monitorAt(int index, const std::string& name, IVec2 position) {
    MonitorInfo info;
    info.index = index;
    info.name = name;
    info.position = position;
    info.resolution = IVec2(1920, 1080);
    return info;
}

const std::string kGeneric = "Generic PnP Monitor";
constexpr int kNoEditor = -1;
}  // namespace

// TD-14c: the Output panel and the Inspector show the same monitor choice, and an
// unresolved output is never shown as monitor 0.
TEST(DescribeMonitorChoiceTest, ResolvedChosenAutomaticAndUnresolved) {
    const std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                               monitorAt(1, "Epson", IVec2(1920, 0))};
    const std::string epson = monitors[1].getDisplayString();
    OutputWindow output;

    output.setMonitor(1);  // automatic, resolved to the projector
    EXPECT_TRUE(output.isMonitorAutomatic());
    EXPECT_EQ(OutputManager::describeMonitorChoice(monitors, output), epson + " (automatic)");

    output.setDesiredMonitor({"Epson", IVec2(1920, 0), 1});  // chosen
    EXPECT_FALSE(output.isMonitorAutomatic());
    EXPECT_EQ(OutputManager::describeMonitorChoice(monitors, output), epson);

    output.setMonitor(-1);  // chosen display unplugged
    EXPECT_EQ(OutputManager::describeMonitorChoice(monitors, output), "Epson (not connected)");

    output.setDesiredMonitor({});  // automatic with nothing free
    EXPECT_EQ(OutputManager::describeMonitorChoice(monitors, output), "Not connected");

    output.setMonitor(5);  // stale index past the list
    EXPECT_EQ(OutputManager::describeMonitorChoice(monitors, output), "Not connected");

    MonitorIdentity savedIndexOnly;  // a pre-identity file chose index 0 by number
    savedIndexOnly.savedIndex = 0;
    output.setDesiredMonitor(savedIndexOnly);
    EXPECT_FALSE(output.isMonitorAutomatic());
}

// One display (a laptop): an Automatic output previews on it at F11, and says so.
TEST(DescribeMonitorChoiceTest, AutomaticWithOnlyTheEditorsDisplayNamesIt) {
    const std::vector<MonitorInfo> laptop = {monitorAt(0, "Laptop", IVec2(0, 0))};
    OutputWindow output;
    output.setMonitor(-1);  // resolution skips the editor's display
    EXPECT_EQ(OutputManager::describeMonitorChoice(laptop, output, /*previewsOnOnlyScreen=*/true),
              laptop[0].getDisplayString() + " (automatic: the only display)");
    EXPECT_EQ(OutputManager::describeMonitorChoice(laptop, output), "Not connected")
        << "another output takes the screen";

    output.setDesiredMonitor({"Epson", IVec2(1920, 0), 1});  // a projector chosen, unplugged
    EXPECT_EQ(OutputManager::describeMonitorChoice(laptop, output, true), "Epson (not connected)");
}

TEST(OnlyScreenPreviewTest, FirstEnabledAutomaticOutputOnTheOnlyDisplay) {
    const std::vector<MonitorInfo> laptop = {monitorAt(0, "Laptop", IVec2(0, 0))};
    std::vector<std::unique_ptr<OutputWindow>> outputs;
    auto add = [&](bool enabled, const MonitorIdentity& desired, int resolved) {
        auto output = std::make_unique<OutputWindow>();
        output->setEnabled(enabled);
        output->setDesiredMonitor(desired);
        output->setMonitor(resolved);
        outputs.push_back(std::move(output));
        return outputs.back().get();
    };
    add(true, {"Epson", IVec2(1920, 0), 1}, -1);  // a chosen projector, unplugged: never
    add(false, {}, -1);                             // disabled
    const OutputWindow* automatic = add(true, {}, -1);
    add(true, {}, -1);                              // a second Automatic one
    EXPECT_EQ(OutputManager::onlyScreenPreview(laptop, outputs), automatic);

    const std::vector<MonitorInfo> two = {monitorAt(0, "Laptop", IVec2(0, 0)),
                                          monitorAt(1, "Epson", IVec2(1920, 0))};
    EXPECT_EQ(OutputManager::onlyScreenPreview(two, outputs), nullptr) << "a projector is there";

    add(true, {"Laptop", IVec2(0, 0), 0}, 0);  // the screen chosen on purpose for one
    EXPECT_EQ(OutputManager::onlyScreenPreview(laptop, outputs), nullptr) << "already taken";
}

TEST(ResolveMonitorTest, ExactNameAndPositionWinOverStaleIndex) {
    // Displays re-enumerated: the projector moved from index 1 to index 0.
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Epson", IVec2(2560, 0)),
                                         monitorAt(1, "Dell", IVec2(0, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {"Epson", IVec2(2560, 0), 1}, 1, {});
    EXPECT_EQ(r.index, 0);
    EXPECT_EQ(r.match, MonitorMatch::Exact);
}

TEST(ResolveMonitorTest, PositionDisambiguatesIdenticalGenericNames) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, kGeneric, IVec2(0, 0)),
                                         monitorAt(1, kGeneric, IVec2(1920, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {kGeneric, IVec2(1920, 0), 1}, 0, {});
    EXPECT_EQ(r.index, 1);
    EXPECT_EQ(r.match, MonitorMatch::Exact);
}

// The QM-087 bug: the laptop and the projector are both "Generic PnP Monitor".
// With the projector unplugged, the name alone must not send the show to the
// laptop (the editor's screen).
TEST(ResolveMonitorTest, GenericNameNeverFallsBackToTheEditor) {
    std::vector<MonitorInfo> laptopOnly = {monitorAt(0, kGeneric, IVec2(0, 0))};
    auto r = OutputManager::resolveMonitor(laptopOnly, {kGeneric, IVec2(1920, 0), 1}, 0, {});
    EXPECT_EQ(r.index, -1);
    EXPECT_EQ(r.match, MonitorMatch::Unresolved);
}

TEST(ResolveMonitorTest, SharedNameIsNotUsedToMatch) {
    // Two other generic displays; the saved one (at 3840) is gone. The name is not
    // unique, so resolution falls through to the first free display, not a name hit.
    std::vector<MonitorInfo> monitors = {monitorAt(0, kGeneric, IVec2(0, 0)),
                                         monitorAt(1, kGeneric, IVec2(1920, 0)),
                                         monitorAt(2, "Dell", IVec2(-1920, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {kGeneric, IVec2(3840, 0), 5}, 2, {});
    EXPECT_EQ(r.index, 0);
    EXPECT_EQ(r.match, MonitorMatch::FirstFree);
}

TEST(ResolveMonitorTest, PositionOnlyWhenProjectorRenamed) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Barco", IVec2(1920, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {"Epson", IVec2(1920, 0), 1}, 0, {});
    EXPECT_EQ(r.index, 1);
    EXPECT_EQ(r.match, MonitorMatch::Position);
}

TEST(ResolveMonitorTest, PositionOnlySkipsEditorAndTaken) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Barco", IVec2(1920, 0))};
    auto editor = OutputManager::resolveMonitor(monitors, {"Epson", IVec2(0, 0), -1}, 0, {});
    EXPECT_NE(editor.match, MonitorMatch::Position);
    EXPECT_EQ(editor.index, 1);  // first free instead
    auto taken = OutputManager::resolveMonitor(monitors, {"Epson", IVec2(1920, 0), -1}, 0, {1});
    EXPECT_EQ(taken.index, -1);
}

TEST(ResolveMonitorTest, UniqueNameWhenPositionChanged) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(2560, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {"Epson", IVec2(1920, 0), 1}, 0, {});
    EXPECT_EQ(r.index, 1);
    EXPECT_EQ(r.match, MonitorMatch::UniqueName);
}

TEST(ResolveMonitorTest, UniqueNameSkipsTheEditor) {
    // The editor window was dragged onto the projector: its name is unique, but an
    // automatic step must still not pick the editor's screen.
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Epson", IVec2(0, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {"Epson", IVec2(1920, 0), -1}, 0, {});
    EXPECT_EQ(r.match, MonitorMatch::Unresolved);
}

TEST(ResolveMonitorTest, SavedIndexForPreIdentityFiles) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0)),
                                         monitorAt(2, "Barco", IVec2(3840, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {"", IVec2(0, 0), 2}, 0, {});
    EXPECT_EQ(r.index, 2);
    EXPECT_EQ(r.match, MonitorMatch::SavedIndex);
}

TEST(ResolveMonitorTest, SavedIndexSkipsEditorAndTaken) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0)),
                                         monitorAt(2, "Barco", IVec2(3840, 0))};
    auto editor = OutputManager::resolveMonitor(monitors, {"", IVec2(0, 0), 0}, 0, {});
    EXPECT_EQ(editor.index, 1);
    EXPECT_EQ(editor.match, MonitorMatch::FirstFree);
    auto taken = OutputManager::resolveMonitor(monitors, {"", IVec2(0, 0), 1}, 0, {1});
    EXPECT_EQ(taken.index, 2);
    EXPECT_EQ(taken.match, MonitorMatch::FirstFree);
}

TEST(ResolveMonitorTest, FirstFreeSkipsEditorAndTaken) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0)),
                                         monitorAt(2, "Barco", IVec2(3840, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {}, 1, {0});
    EXPECT_EQ(r.index, 2);
    EXPECT_EQ(r.match, MonitorMatch::FirstFree);
}

TEST(ResolveMonitorTest, ExplicitChoiceOfTheEditorMonitorIsHonoured) {
    // The user picked the editor's own screen in the Output panel.
    std::vector<MonitorInfo> monitors = {monitorAt(0, kGeneric, IVec2(0, 0)),
                                         monitorAt(1, kGeneric, IVec2(1920, 0))};
    auto r = OutputManager::resolveMonitor(monitors, {kGeneric, IVec2(0, 0), 0}, 0, {1});
    EXPECT_EQ(r.index, 0);
    EXPECT_EQ(r.match, MonitorMatch::Exact);
}

TEST(ResolveMonitorTest, UnresolvedWhenOnlyTheEditorIsConnected) {
    std::vector<MonitorInfo> laptopOnly = {monitorAt(0, "Dell", IVec2(0, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(laptopOnly, {}, 0, {}).match,
              MonitorMatch::Unresolved);
    EXPECT_EQ(OutputManager::resolveMonitor(laptopOnly, {"", IVec2(0, 0), 0}, 0, {}).index, -1);
    EXPECT_EQ(OutputManager::resolveMonitor({}, {"Epson", IVec2(0, 0), 1}, kNoEditor, {}).index,
              -1);
}

TEST(ResolveMonitorsTest, StrongerMatchClaimsItsMonitorFirst) {
    // Output A's projector is gone and would fall back to the first free display;
    // output B, later in the list, really belongs on that display. B keeps it.
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0)),
                                         monitorAt(2, "Barco", IVec2(3840, 0))};
    auto results = OutputManager::resolveMonitors(
        monitors, {{"Christie", IVec2(-1920, 0), 7}, {"Epson", IVec2(1920, 0), 1}}, 0);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[1].index, 1);
    EXPECT_EQ(results[1].match, MonitorMatch::Exact);
    EXPECT_EQ(results[0].index, 2);
    EXPECT_EQ(results[0].match, MonitorMatch::FirstFree);
}

TEST(ResolveMonitorsTest, TwoAutomaticOutputsTakeDifferentDisplays) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0))};
    auto results = OutputManager::resolveMonitors(monitors, {{}, {}}, 0);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].index, 1);
    EXPECT_EQ(results[1].index, -1);  // never doubled up on the projector or the editor
}

TEST(ResolveMonitorTest, MonitorContainingPoint) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0))};
    EXPECT_EQ(OutputManager::monitorContaining(monitors, IVec2(960, 540)), 0);
    EXPECT_EQ(OutputManager::monitorContaining(monitors, IVec2(1920, 0)), 1);
    EXPECT_EQ(OutputManager::monitorContaining(monitors, IVec2(-32000, -32000)), -1);
}

// ---- Swap pacing (QM-090) ----------------------------------------------------

TEST(VsyncHolderTest, FirstNonMinimisedOutputPacesTheLoop) {
    EXPECT_EQ(OutputManager::vsyncHolder({}), -1);
    EXPECT_EQ(OutputManager::vsyncHolder({false}), 0);
    EXPECT_EQ(OutputManager::vsyncHolder({false, false}), 0);
    // A minimised output's swap does not block; it must never hold vsync.
    EXPECT_EQ(OutputManager::vsyncHolder({true, false}), 1);
    EXPECT_EQ(OutputManager::vsyncHolder({true}), -1);
    EXPECT_EQ(OutputManager::vsyncHolder({true, true}), -1);
}

// TD-12: the lowest known refresh rate among the presented outputs paces the loop, so no
// display is sent more frames than it can show; a tie goes to list order. Unknown
// rates (0) hold vsync only when no rate is known.
TEST(VsyncHolderTest, TheLowestRefreshRatePacesTheLoop) {
    EXPECT_EQ(OutputManager::vsyncHolder({false, false}, {165, 60}), 1);   // measured rig
    EXPECT_EQ(OutputManager::vsyncHolder({false, false}, {60, 165}), 0);
    EXPECT_EQ(OutputManager::vsyncHolder({false, false, false}, {50, 60, 60}), 0);  // not the majority
    EXPECT_EQ(OutputManager::vsyncHolder({false, false, false}, {60, 60, 50}), 2);
    EXPECT_EQ(OutputManager::vsyncHolder({false, false, false}, {60, 50, 50}), 1);  // tie: list order
    EXPECT_EQ(OutputManager::vsyncHolder({false, false, false}, {60, 60, 60}), 0);
    EXPECT_EQ(OutputManager::vsyncHolder({false, false, false}, {144, 0, 120}), 2);  // unknown skipped
    EXPECT_EQ(OutputManager::vsyncHolder({true, false, false}, {50, 60, 165}), 1);  // minimised: never
    EXPECT_EQ(OutputManager::vsyncHolder({false, false}, {0, 144}), 1);   // a known rate wins
    EXPECT_EQ(OutputManager::vsyncHolder({false, false}, {0, 0}), 0);
    EXPECT_EQ(OutputManager::vsyncHolder({false, false}, {60}), 0);       // missing = unknown
    EXPECT_EQ(OutputManager::vsyncHolder({true, true}, {60, 60}), -1);
}

// ---- Scene take keys on output windows (US-008) ----------------------------------

TEST(SceneTakeKeyTest, PlainDigitPressesTakeTheirScene) {
    for (int n = 1; n <= 9; ++n) {
        EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_0 + n, GLFW_PRESS, 0), n);
        EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_KP_0 + n, GLFW_PRESS, 0), n);
    }
    // Caps Lock / Num Lock state is not a modifier.
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_4, GLFW_PRESS, GLFW_MOD_CAPS_LOCK), 4);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_4, GLFW_PRESS, GLFW_MOD_NUM_LOCK), 4);
}

TEST(SceneTakeKeyTest, OtherKeysModifiersAndRepeatsDoNot) {
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_0, GLFW_PRESS, 0), 0);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_A, GLFW_PRESS, 0), 0);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_ESCAPE, GLFW_PRESS, 0), 0);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_KP_0, GLFW_PRESS, 0), 0);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_KP_DECIMAL, GLFW_PRESS, 0), 0);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_KP_ENTER, GLFW_PRESS, 0), 0);
    // AZERTY's "4" key is still GLFW_KEY_4 (physical); the apostrophe key is not a take.
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_APOSTROPHE, GLFW_PRESS, 0), 0);
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_MINUS, GLFW_PRESS, 0), 0);
    for (int key : {GLFW_KEY_1, GLFW_KEY_KP_1}) {
        EXPECT_EQ(OutputManager::sceneTakeNumber(key, GLFW_RELEASE, 0), 0);
        EXPECT_EQ(OutputManager::sceneTakeNumber(key, GLFW_REPEAT, 0), 0);
        for (int mod : {GLFW_MOD_SHIFT, GLFW_MOD_CONTROL, GLFW_MOD_ALT, GLFW_MOD_SUPER}) {
            EXPECT_EQ(OutputManager::sceneTakeNumber(key, GLFW_PRESS, mod), 0) << key << " " << mod;
        }
    }
}

TEST(OutputManagerTest, DeleteOutputReportsTheRemovedId) {
    OutputManager manager;
    OutputWindow* a = manager.createOutput("A");
    OutputWindow* b = manager.createOutput("B");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const UUID aId = a->getId();
    std::vector<UUID> removed;
    manager.setOutputRemovedCallback([&](const UUID& id) { removed.push_back(id); });

    manager.deleteOutput(UUID::generate());  // unknown id: nothing removed, no call
    EXPECT_TRUE(removed.empty());
    manager.deleteOutput(aId);
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], aId);
    EXPECT_EQ(manager.getOutput(aId), nullptr);  // already gone when reported
}

// ---- Master blackout (ADR-048 §3) ------------------------------------------------

TEST(BlackoutTest, StartsOffAndToggles) {
    OutputManager manager;
    EXPECT_FALSE(manager.isBlackout());
    manager.toggleBlackout();
    EXPECT_TRUE(manager.isBlackout());
    manager.setBlackout(true);
    EXPECT_TRUE(manager.isBlackout());
    manager.toggleBlackout();
    EXPECT_FALSE(manager.isBlackout());
}

TEST(BlackoutTest, ShortcutIsCtrlBPressOnly) {
    EXPECT_TRUE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_PRESS, GLFW_MOD_CONTROL));
    EXPECT_TRUE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_PRESS,
                                             GLFW_MOD_CONTROL | GLFW_MOD_NUM_LOCK));
    EXPECT_FALSE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_PRESS, 0));
    EXPECT_FALSE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_REPEAT, GLFW_MOD_CONTROL));
    EXPECT_FALSE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_RELEASE, GLFW_MOD_CONTROL));
    EXPECT_FALSE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_PRESS,
                                              GLFW_MOD_CONTROL | GLFW_MOD_SHIFT));
    EXPECT_FALSE(OutputManager::isBlackoutKey(GLFW_KEY_B, GLFW_PRESS,
                                              GLFW_MOD_CONTROL | GLFW_MOD_ALT));
    EXPECT_FALSE(OutputManager::isBlackoutKey(GLFW_KEY_N, GLFW_PRESS, GLFW_MOD_CONTROL));
    // It never collides with a scene-take key.
    EXPECT_EQ(OutputManager::sceneTakeNumber(GLFW_KEY_B, GLFW_PRESS, GLFW_MOD_CONTROL), 0);
}

// ---------------------------------------------------------------------------
// Render once per output (ADR-054): GPU readback proving the new path shows the
// projector exactly what the old one did. Old path: a Renderer in the output
// window's own context draws the scene straight into that window's framebuffer
// (here an FBO of the same size and format in that context). New path:
// renderOutputFrames draws the frame in the editor context, and the output context
// copies it with FrameCopy. The two images must match byte for byte.
// Skips where no GL 3.3 context is available (headless CI).
// ---------------------------------------------------------------------------

namespace {

// Hidden editor window with a GL 3.3 core context, RAII; ok == false means no GL.
struct EditorGL {
    GLFWwindow* win = nullptr;
    bool ok = false;
    EditorGL() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        win = glfwCreateWindow(64, 64, "render-once-editor", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~EditorGL() {
        if (win) { glfwDestroyWindow(win); glfwTerminate(); }
    }
    EditorGL(const EditorGL&) = delete;
    EditorGL& operator=(const EditorGL&) = delete;
};

// A texture with detail in every texel (so any sampling difference shows).
std::unique_ptr<Texture> patternTexture(int size, int seed) {
    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            unsigned char* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = static_cast<unsigned char>((x * 37 + seed * 11) & 255);
            p[1] = static_cast<unsigned char>((y * 53 + seed * 29) & 255);
            p[2] = static_cast<unsigned char>(((x ^ y) * 17 + seed) & 255);
            p[3] = 255;
        }
    }
    auto texture = std::make_unique<Texture>();
    texture->create(size, size, 4, px.data());
    return texture;
}

std::vector<unsigned char> readBound(int w, int h) {
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

// Quads with every surface option an output draws, overlapping so blending and
// z-order matter, and one 3D cube in front of the projector.
void buildFeatureScene(Scene& scene, const std::vector<Texture*>& media,
                       std::unordered_map<UUID, Texture*>& textures) {
    QuadManager& qm = scene.getQuadManager();

    Quad* keystone = qm.createQuad("Keystone");
    keystone->getGeometry().setCorners({Vec2(0.03f, 0.06f), Vec2(0.46f, 0.02f),
                                        Vec2(0.42f, 0.52f), Vec2(0.08f, 0.44f)});
    keystone->getTexture().uvRotation = 0.3f;
    keystone->getTexture().cropRect = NormalizedRect{0.1f, 0.05f, 0.8f, 0.7f};

    Quad* bezier = qm.createQuad("Bezier");
    bezier->getGeometry().setCorners({Vec2(0.40f, 0.45f), Vec2(0.95f, 0.50f),
                                      Vec2(0.90f, 0.95f), Vec2(0.45f, 0.88f)});
    bezier->getGeometry().setBezierEnabled(true);
    std::array<Vec2, 8> offsets;
    for (int k = 0; k < 8; ++k) {
        offsets[static_cast<size_t>(k)] = Vec2((k % 2 ? 0.04f : -0.05f), (k % 3 ? 0.03f : -0.04f));
    }
    bezier->getGeometry().setBezierOffsets(offsets);
    ColorCorrection cc;
    cc.brightness = 0.06f;
    cc.contrast = 1.3f;
    cc.gamma = 0.8f;
    cc.rgbGain = Vec3(1.2f, 0.9f, 1.05f);
    bezier->setColorCorrection(cc);
    bezier->setOpacity(0.7f);

    Quad* mesh = qm.createQuad("Mesh");
    mesh->getGeometry().setCorners({Vec2(0.05f, 0.50f), Vec2(0.55f, 0.55f),
                                    Vec2(0.50f, 0.98f), Vec2(0.03f, 0.92f)});
    mesh->getGeometry().setMeshMode(MeshMode::MESH_EDIT);
    mesh->getGeometry().setUniformGrid(4);
    mesh->getGeometry().setMeshVertex(2, 2, Vec2(0.05f, -0.04f));
    mesh->getGeometry().setMeshVertex(1, 3, Vec2(-0.03f, 0.02f));
    SoftEdge edge;
    edge.left = 0.2f;
    edge.right = 0.15f;
    edge.top = 0.1f;
    edge.gamma = 2.2f;
    mesh->setSoftEdge(edge);
    mesh->getTexture().flipH = true;

    const std::vector<Quad*> quads{keystone, bezier, mesh};
    for (size_t i = 0; i < quads.size(); ++i) {
        quads[i]->setZOrder(static_cast<int>(i));
        textures[quads[i]->getId()] = media[i % media.size()];
    }

    // A cube 0.3 across at the origin; the output's projector is fitted onto it, and
    // the quads leave the top right of the canvas for it.
    MeshObject* cube = scene.createObject("Cube");
    MeshData& data = cube->editMesh();
    const float h = 0.15f;
    for (int c = 0; c < 8; ++c) {
        data.positions.push_back(Vec3((c & 1) ? h : -h, (c & 2) ? h : -h, (c & 4) ? h : -h));
    }
    data.indices = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                    2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    cube->setTransform(glm::rotate(glm::mat4(1.0f), 0.6f, Vec3(0.3f, 1.0f, 0.2f)));
    textures[cube->getId()] = media.back();
}

struct ReadbackCase {
    Rotation rotation;
    bool blackout;
    bool quadsOnly = false;  // the scene without its 3D object: every pixel must match
};

class RenderOnceReadbackTest : public ::testing::TestWithParam<ReadbackCase> {};

}  // namespace

TEST_P(RenderOnceReadbackTest, OutputPixelsMatchTheDirectPath) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";
    const ReadbackCase param = GetParam();

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Readback");
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(output->isCreated());
    output->setResolution(IVec2(160, 96));
    ASSERT_TRUE(output->applyResolution().isSuccess());
    output->setRotation(param.rotation);
    manager.setBlackout(param.blackout);
    int w = 0, h = 0;
    glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
    ASSERT_GT(w, 0);
    ASSERT_GT(h, 0);

    // Media and the scene, made in the editor context (textures are shared).
    glfwMakeContextCurrent(gl.win);
    auto mediaA = patternTexture(64, 1);
    auto mediaB = patternTexture(48, 2);
    auto mediaC = patternTexture(32, 3);
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    // An old show as migrated (ADR-047 §7): the canvas is the output's effective
    // size, shown through one whole-canvas slice with no warp and no blend. The
    // extra canvas pass must not change one pixel on the projector (gate a).
    const bool quarterTurn = param.rotation == Rotation::R90 || param.rotation == Rotation::R270;
    scene.setCanvasSize(quarterTurn ? IVec2(h, w) : IVec2(w, h));
    OutputManager::showScene(*output, &scene, scenes);
    std::unordered_map<UUID, Texture*> textures;
    buildFeatureScene(scene, {mediaA.get(), mediaB.get(), mediaC.get()}, textures);
    Vec3 objectsMin, objectsMax;
    ASSERT_TRUE(scene.visibleObjectBounds(objectsMin, objectsMax));
    const IVec2 effective = output->getEffectiveResolution();
    output->getProjector() = ProjectorCamera::fitToOutput(
        ProjectorCamera{}, effective.x, effective.y, &objectsMin, &objectsMax);
    if (param.quadsOnly) scene.deleteObject(scene.getObjects().front()->getId());

    // New path, part 1: the editor context draws the output's frame.
    Renderer editorRenderer;
    ASSERT_TRUE(editorRenderer.initialize());
    manager.renderOutputFrames(scenes, textures, editorRenderer);
    const Framebuffer* frame = manager.outputFrame(output->getId());
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->width(), w);
    EXPECT_EQ(frame->height(), h);
    glFinish();

    std::vector<unsigned char> direct, copied, quadsOnly;
    {
        output->makeContextCurrent();

        // Old path: the output context's own Renderer draws straight into its
        // framebuffer (presentToOutputs + drawOutputScene before ADR-054).
        Renderer outputRenderer;
        ASSERT_TRUE(outputRenderer.initialize());
        Framebuffer window;
        ASSERT_TRUE(window.resize(w, h));
        window.bind();
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        outputRenderer.setOutputRotation(output->getRotation());
        if (!param.blackout) {
            outputRenderer.renderMeshObjects(scene.getObjects(), output->getProjector(), textures);
            for (const Quad* quad : scene.getQuadsByZOrder()) {
                outputRenderer.renderQuad(*quad, textures.at(quad->getId()), false);
            }
        }
        glEnable(GL_DEPTH_TEST);
        direct = readBound(w, h);

        // The cube must be in the picture, or the comparison would not cover 3D.
        if (!param.blackout && !param.quadsOnly) {
            glDisable(GL_DEPTH_TEST);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            for (const Quad* quad : scene.getQuadsByZOrder()) {
                outputRenderer.renderQuad(*quad, textures.at(quad->getId()), false);
            }
            quadsOnly = readBound(w, h);
            EXPECT_NE(quadsOnly, direct) << "the 3D object drew nothing";
        }

        // New path, part 2: the output context copies the frame into its framebuffer.
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);  // anything the copy fails to cover shows
        glClear(GL_COLOR_BUFFER_BIT);
        FrameCopy copy;
        ASSERT_TRUE(copy.copy(*frame, w, h));
        copied = readBound(w, h);

        copy.destroy();
        window.destroy();
        outputRenderer.shutdown();
    }
    glfwMakeContextCurrent(gl.win);

    ASSERT_EQ(direct.size(), copied.size());
    size_t differing = 0;
    size_t lit = 0;
    size_t translucent = 0;
    size_t directTranslucent = 0;
    for (size_t i = 0; i < direct.size(); i += 4) {
        if (direct[i] != copied[i] || direct[i + 1] != copied[i + 1] ||
            direct[i + 2] != copied[i + 2]) {
            ++differing;
        }
        // The frame is opaque: the preview draws it blended by its alpha, so alpha
        // below 1 where a quad is translucent would darken the preview.
        if (copied[i + 3] != 255) {
            ++translucent;
        }
        if (direct[i + 3] != 255) {
            ++directTranslucent;  // plain SRC_ALPHA blending leaves alpha below 1 there
        }
        if (direct[i] > 8 || direct[i + 1] > 8 || direct[i + 2] > 8) {
            ++lit;
        }
    }
    // ADR-047 gate a. Wherever no 3D object shows, the canvas pass changes no pixel.
    // Where a translucent quad lies over a 3D object the old path blended the quad
    // straight onto the object, while the new one blends it into the 8-bit canvas
    // first and composites that over the object: two roundings instead of one, at
    // most 2 levels apart. Objects cannot go through the shared canvas, as each
    // projector sees them from its own camera (ADR-047 §4).
    size_t differingOffObjects = 0;
    size_t differingOnObjects = 0;
    int worstOnObjects = 0;
    for (size_t i = 0; i < direct.size(); i += 4) {
        int delta = 0;
        for (size_t c = 0; c < 3; ++c) {
            delta = std::max(delta, std::abs(int(direct[i + c]) - int(copied[i + c])));
        }
        if (delta == 0) continue;
        const bool onObject = !quadsOnly.empty() &&
                              (quadsOnly[i] != direct[i] || quadsOnly[i + 1] != direct[i + 1] ||
                               quadsOnly[i + 2] != direct[i + 2]);
        if (onObject) {
            ++differingOnObjects;
            worstOnObjects = std::max(worstOnObjects, delta);
        } else {
            ++differingOffObjects;
        }
    }
    EXPECT_EQ(differing, differingOffObjects + differingOnObjects);
    if (param.quadsOnly || param.blackout) EXPECT_EQ(differing, 0u) << "byte-identical";
    EXPECT_EQ(differingOffObjects, 0u) << "of " << direct.size() / 4 << " pixels";
    EXPECT_LE(worstOnObjects, 2) << differingOnObjects << " pixels differ over the 3D object";
    std::printf("[ readback ] %zu of %zu pixels identical; %zu over the 3D object differ by <= %d\n",
                direct.size() / 4 - differing, direct.size() / 4, differingOnObjects, worstOnObjects);
    EXPECT_EQ(translucent, 0u) << "frame pixels with alpha below 255";
    if (param.blackout) {
        EXPECT_EQ(lit, 0u);  // the projector is black, and so is its frame
    } else {
        EXPECT_GT(lit, direct.size() / 4 / 3);  // the scene covers much of the frame
        EXPECT_GT(directTranslucent, 0u) << "the scene must blend, or alpha is not covered";
    }

    editorRenderer.shutdown();
    mediaA.reset();
    mediaB.reset();
    mediaC.reset();
    manager.shutdown();
}

INSTANTIATE_TEST_SUITE_P(
    RotationsAndBlackout, RenderOnceReadbackTest,
    ::testing::Values(ReadbackCase{Rotation::R0, false}, ReadbackCase{Rotation::R90, false},
                      ReadbackCase{Rotation::R180, false}, ReadbackCase{Rotation::R270, false},
                      ReadbackCase{Rotation::R90, true},
                      ReadbackCase{Rotation::R0, false, true}, ReadbackCase{Rotation::R90, false, true},
                      ReadbackCase{Rotation::R180, false, true}, ReadbackCase{Rotation::R270, false, true}),
    [](const ::testing::TestParamInfo<ReadbackCase>& info) {
        return "R" + std::to_string(static_cast<int>(info.param.rotation)) +
               (info.param.blackout ? "_Blackout" : "") + (info.param.quadsOnly ? "_QuadsOnly" : "");
    });

// An output with no assigned scene shows black, and a deleted output's frame goes.
TEST(RenderOnceTest, UnroutedOutputIsBlackAndDeletedOutputLosesItsFrame) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Unrouted");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 32, 32);
    glfwMakeContextCurrent(gl.win);

    const long framebuffersBefore = GlObjectStats::framebuffers();
    const long texturesBefore = GlObjectStats::textures();
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.front()->getQuadManager().createQuad("Not routed");
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);

    const Framebuffer* frame = manager.outputFrame(output->getId());
    ASSERT_NE(frame, nullptr);
    std::vector<unsigned char> pixels;
    {
        Framebuffer readTarget;
        ASSERT_TRUE(readTarget.resize(frame->width(), frame->height()));
        readTarget.bind();
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        FrameCopy copy;
        ASSERT_TRUE(copy.copy(*frame, frame->width(), frame->height()));
        pixels = readBound(frame->width(), frame->height());
        Framebuffer::unbind();
    }
    for (size_t i = 0; i < pixels.size(); i += 4) {
        ASSERT_EQ(pixels[i] | pixels[i + 1] | pixels[i + 2], 0) << "pixel " << i / 4;
    }

    manager.deleteOutput(output->getId());
    EXPECT_EQ(manager.outputFrame(output->getId()), nullptr);
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffersBefore);
    EXPECT_EQ(GlObjectStats::textures(), texturesBefore + 1);  // the renderer's white default

    renderer.shutdown();
    manager.shutdown();
}

// The frames are drawn after the panels, so the Preview tab has already recorded a
// frame's texture when a resize (F11, a resolution change) happens in the same frame:
// the texture must keep its name, or ImGui would sample a deleted one.
TEST(RenderOnceTest, ResizedFrameKeepsItsTextureName) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Resized");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 32, 32);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);
    const Framebuffer* frame = manager.outputFrame(output->getId());
    ASSERT_NE(frame, nullptr);
    const GLuint texture = frame->colorTexture();
    const long framebuffers = GlObjectStats::framebuffers();
    const long allTextures = GlObjectStats::textures();

    glfwSetWindowSize(output->getGLFWwindow(), 48, 40);
    int w = 0, h = 0;
    glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
    ASSERT_TRUE(w != 32 || h != 32) << "the window did not resize";
    manager.renderOutputFrames(scenes, textures, renderer);
    frame = manager.outputFrame(output->getId());
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->width(), w);
    EXPECT_EQ(frame->height(), h);
    EXPECT_EQ(frame->colorTexture(), texture);
    EXPECT_EQ(glIsTexture(texture), GL_TRUE);
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffers);
    EXPECT_EQ(GlObjectStats::textures(), allTextures);

    renderer.shutdown();
    manager.shutdown();
}

// Inspector "Destroy Window" then "Create Window": the copier's framebuffer object is
// freed in the dying context, and the new window makes its own instead of binding the
// dead context's name (GL_INVALID_OPERATION, and nothing shown).
TEST(RenderOnceTest, DestroyedOutputWindowFreesItsCopierAndARecreatedOneWorks) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Recreated");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 32, 32);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);
    const Framebuffer* frame = manager.outputFrame(output->getId());
    ASSERT_NE(frame, nullptr);
    glFinish();

    auto copyInOutputContext = [&] {
        output->makeContextCurrent();
        while (glGetError() != GL_NO_ERROR) {}
        const bool copied = output->frameCopy().copy(*frame, frame->width(), frame->height());
        const GLenum error = glGetError();
        glfwMakeContextCurrent(gl.win);
        return copied && error == GL_NO_ERROR;
    };

    const long framebuffersBefore = GlObjectStats::framebuffers();
    ASSERT_TRUE(copyInOutputContext());
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffersBefore + 1);

    output->destroy();
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffersBefore) << "copier not freed";
    EXPECT_EQ(glfwGetCurrentContext(), gl.win) << "the caller's context must stay current";

    ASSERT_TRUE(output->create(gl.win).isSuccess());
    EXPECT_TRUE(copyInOutputContext());
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffersBefore + 1);

    renderer.shutdown();
    manager.shutdown();
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffersBefore - 1) << "frame and copier freed";
}

// ---------------------------------------------------------------------------
// Scene canvas -> output slices (ADR-047): readback of the composed frames.
// ---------------------------------------------------------------------------

namespace {

// An output frame's pixels (RGBA, bottom-up rows), read in the editor context.
std::vector<unsigned char> readFrame(const Framebuffer& frame) {
    Framebuffer target;
    if (!target.resize(frame.width(), frame.height())) return {};
    target.bind();
    FrameCopy copy;
    copy.copy(frame, frame.width(), frame.height());
    std::vector<unsigned char> pixels = readBound(frame.width(), frame.height());
    Framebuffer::unbind();
    return pixels;
}

// A quad over the whole canvas.
Quad* fullCanvasQuad(Scene& scene) {
    Quad* quad = scene.getQuadManager().createQuad("Full");
    quad->getGeometry().setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f),
                                    Vec2(0.0f, 1.0f)});
    return quad;
}

}  // namespace

// Gate b (ADR-047 §8), on the GPU: two outputs show one canvas, overlapping by half a
// frame with facing gamma-2.2 blend zones. Full-white content: the two projectors'
// light (pixel value ^ 2.2) sums to 1 within 2% at every column of the overlap, and
// each projector is at full white outside it.
TEST(CanvasSliceRenderTest, TwoOutputOverlapSumsToFlatLinearLight) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* left = manager.createOutput("Left");
    OutputWindow* right = manager.createOutput("Right");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    for (OutputWindow* o : {left, right}) glfwSetWindowSize(o->getGLFWwindow(), 256, 32);
    int w = 0, h = 0, rw = 0, rh = 0;
    glfwGetFramebufferSize(left->getGLFWwindow(), &w, &h);
    glfwGetFramebufferSize(right->getGLFWwindow(), &rw, &rh);
    ASSERT_GT(w, 16);
    ASSERT_EQ(w, rw);
    ASSERT_EQ(h, rh);
    glfwMakeContextCurrent(gl.win);

    const int overlap = w / 2;
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    scene.setCanvasSize(IVec2(2 * w - overlap, h));
    fullCanvasQuad(scene);  // no texture: the renderer's white
    OutputManager::showScene(*left, &scene, scenes);
    OutputManager::showScene(*right, &scene, scenes);
    Slice& a = left->getSlices()[0];
    Slice& b = right->getSlices()[0];
    a.input = CanvasRect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
    b.input = CanvasRect{static_cast<float>(w - overlap), 0.0f, static_cast<float>(w), static_cast<float>(h)};
    a.blend[SLICE_EDGE_RIGHT] = BlendEdge{static_cast<float>(overlap), 2.2f, 2.0f};
    b.blend[SLICE_EDGE_LEFT] = BlendEdge{static_cast<float>(overlap), 2.2f, 2.0f};

    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);
    ASSERT_NE(manager.sceneCanvas(scene.getId()), nullptr);
    const std::vector<unsigned char> lp = readFrame(*manager.outputFrame(left->getId()));
    const std::vector<unsigned char> rp = readFrame(*manager.outputFrame(right->getId()));
    ASSERT_EQ(lp.size(), static_cast<size_t>(w) * h * 4);
    ASSERT_EQ(rp.size(), lp.size());

    const int row = h / 2;
    auto red = [&](const std::vector<unsigned char>& px, int x) {
        return static_cast<int>(px[(static_cast<size_t>(row) * w + x) * 4]);
    };
    double worst = 0.0;
    for (int x = w - overlap; x < w; ++x) {  // canvas column x
        const double la = std::pow(red(lp, x) / 255.0, 2.2);
        const double lb = std::pow(red(rp, x - (w - overlap)) / 255.0, 2.2);
        worst = std::max(worst, std::abs(la + lb - 1.0));
        EXPECT_NEAR(la + lb, 1.0, 0.02) << "canvas column " << x;
    }
    std::printf("[ blend ] overlap %d px: worst |light sum - 1| = %.4f\n", overlap, worst);
    EXPECT_EQ(red(lp, 2), 255);           // left alone
    EXPECT_EQ(red(rp, w - 3), 255);       // right alone
    EXPECT_LT(red(lp, w - 1), 60);        // the left fades out at its edge...
    EXPECT_LT(red(rp, 0), 60);            // ...and the right in
    for (size_t i = 3; i < lp.size(); i += 4) ASSERT_EQ(lp[i], 255) << "the frame stays opaque";

    renderer.shutdown();
    manager.shutdown();
}

// The canvas is drawn once per shown scene and released when no output shows the
// scene any more, when its last output is removed, and at shutdown (stress_smoke's
// GL object counts stay flat).
TEST(CanvasSliceRenderTest, CanvasLivesWhileAnOutputShowsItsScene) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* a = manager.createOutput("A");
    OutputWindow* b = manager.createOutput("B");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    for (OutputWindow* o : {a, b}) glfwSetWindowSize(o->getGLFWwindow(), 32, 32);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    scene.setCanvasSize(IVec2(64, 32));
    fullCanvasQuad(scene);
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    const long before = GlObjectStats::framebuffers();

    OutputManager::showScene(*a, &scene, scenes);
    OutputManager::showScene(*b, &scene, scenes);
    manager.renderOutputFrames(scenes, textures, renderer);
    EXPECT_EQ(GlObjectStats::framebuffers(), before + 3) << "two frames, one canvas for both";

    OutputManager::showScene(*b, nullptr, scenes);
    manager.renderOutputFrames(scenes, textures, renderer);
    EXPECT_NE(manager.sceneCanvas(scene.getId()), nullptr);  // a still shows it
    EXPECT_EQ(GlObjectStats::framebuffers(), before + 3);

    OutputManager::showScene(*a, nullptr, scenes);
    manager.renderOutputFrames(scenes, textures, renderer);
    EXPECT_EQ(manager.sceneCanvas(scene.getId()), nullptr);
    EXPECT_EQ(GlObjectStats::framebuffers(), before + 2) << "canvas of an unshown scene released";

    OutputManager::showScene(*a, &scene, scenes);
    manager.renderOutputFrames(scenes, textures, renderer);
    EXPECT_EQ(GlObjectStats::framebuffers(), before + 3);
    manager.deleteOutput(a->getId());
    EXPECT_EQ(manager.sceneCanvas(scene.getId()), nullptr) << "its last output went";
    EXPECT_EQ(GlObjectStats::framebuffers(), before + 1);

    OutputManager::showScene(*b, &scene, scenes);
    manager.renderOutputFrames(scenes, textures, renderer);
    renderer.shutdown();
    manager.shutdown();
    EXPECT_EQ(GlObjectStats::framebuffers(), before);
}

// The blend test pattern (ADR-047 §3) replaces the scene on the canvas while it is on,
// and reaches every output through its slices: black grid cells with white bands
// across the middle, whatever the scene holds; off, the scene is back.
TEST(CanvasSliceRenderTest, BlendTestPatternReplacesTheSceneOnTheOutputs) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Pattern");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 256, 144);
    int w = 0, h = 0;
    glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
    ASSERT_GE(w, 128);
    ASSERT_GE(h, 72);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    scene.setCanvasSize(IVec2(w, h));
    fullCanvasQuad(scene);  // the show: all white
    OutputManager::showScene(*output, &scene, scenes);
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());

    auto red = [&](const std::vector<unsigned char>& px, int x, int y) {
        return static_cast<int>(px[(static_cast<size_t>(y) * w + x) * 4]);
    };
    const int cellX = w / 32;  // the middle of the first grid cell (16 columns, 9 rows)
    const int cellY = h / 18;

    manager.renderOutputFrames(scenes, textures, renderer);
    std::vector<unsigned char> px = readFrame(*manager.outputFrame(output->getId()));
    ASSERT_EQ(px.size(), static_cast<size_t>(w) * h * 4);
    EXPECT_EQ(red(px, cellX, cellY), 255) << "pattern off: the scene";

    manager.setBlendTestPattern(true);
    manager.renderOutputFrames(scenes, textures, renderer);
    px = readFrame(*manager.outputFrame(output->getId()));
    EXPECT_EQ(red(px, cellX, cellY), 0) << "a grid cell is black";
    EXPECT_EQ(red(px, cellX, h / 2), 255) << "the horizontal band is white";
    EXPECT_EQ(red(px, w / 2, cellY), 255) << "the vertical band is white";
    for (size_t i = 3; i < px.size(); i += 4) ASSERT_EQ(px[i], 255) << "the frame stays opaque";
    int whiteLines = 0;  // grid lines cross the first row of cells
    for (int x = 0; x < w; ++x) whiteLines += red(px, x, cellY) > 128 ? 1 : 0;
    EXPECT_GT(whiteLines, w / 16) << "grid lines and the band";

    manager.setBlendTestPattern(false);
    manager.renderOutputFrames(scenes, textures, renderer);
    px = readFrame(*manager.outputFrame(output->getId()));
    EXPECT_EQ(red(px, cellX, cellY), 255) << "off again: the scene is back";

    renderer.shutdown();
    manager.shutdown();
}

// A slice shows its input rectangle of the canvas, through its warp: a canvas whose
// left half is white, shown by a slice of that half only, fills the whole frame; the
// same slice squeezed into the left half of the output by its warp leaves the right
// half black.
TEST(CanvasSliceRenderTest, SliceShowsItsInputThroughItsWarp) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Warped");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 64, 32);
    int w = 0, h = 0;
    glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    scene.setCanvasSize(IVec2(2 * w, h));
    Quad* quad = scene.getQuadManager().createQuad("Left half");
    quad->getGeometry().setCorners({Vec2(0.0f, 0.0f), Vec2(0.5f, 0.0f), Vec2(0.5f, 1.0f),
                                    Vec2(0.0f, 1.0f)});
    OutputManager::showScene(*output, &scene, scenes);
    output->getSlices()[0].input = CanvasRect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};

    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    auto pixelAt = [&](int x) {
        manager.renderOutputFrames(scenes, textures, renderer);
        const auto px = readFrame(*manager.outputFrame(output->getId()));
        return static_cast<int>(px[(static_cast<size_t>(h / 2) * w + x) * 4]);
    };
    EXPECT_EQ(pixelAt(2), 255);
    EXPECT_EQ(pixelAt(w - 3), 255) << "the left half of the canvas fills the frame";

    output->getSlices()[0].warp.setCorners({Vec2(0.0f, 0.0f), Vec2(0.5f, 0.0f), Vec2(0.5f, 1.0f),
                                            Vec2(0.0f, 1.0f)});
    EXPECT_EQ(pixelAt(2), 255);
    EXPECT_EQ(pixelAt(w - 3), 0) << "warped into the left half of the output";

    renderer.shutdown();
    manager.shutdown();
}

// The black-level lift outside the blend zones is L + (1 - L) * (the canvas over the
// frame below): a 3D object under a lifted slice is lifted, not replaced by black.
TEST(CanvasSliceRenderTest, BlackLevelLiftKeepsObjectsUnderTheSlice) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Lifted");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 96, 64);
    int w = 0, h = 0;
    glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
    glfwMakeContextCurrent(gl.win);

    auto media = patternTexture(32, 4);
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    scene.setCanvasSize(IVec2(w, h));
    OutputManager::showScene(*output, &scene, scenes);
    std::unordered_map<UUID, Texture*> textures;
    // The feature scene's cube, with no quads over it: the canvas is transparent.
    buildFeatureScene(scene, {media.get()}, textures);
    std::vector<UUID> quadIds;
    for (const auto& quad : scene.getQuadManager().getQuads()) quadIds.push_back(quad->getId());
    for (const UUID& id : quadIds) scene.getQuadManager().deleteQuad(id);
    Vec3 lo, hi;
    ASSERT_TRUE(scene.visibleObjectBounds(lo, hi));
    output->getProjector() = ProjectorCamera::fitToOutput(ProjectorCamera{}, w, h, &lo, &hi);

    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);
    const std::vector<unsigned char> plain = readFrame(*manager.outputFrame(output->getId()));
    const float level = 0.1f;
    output->getSlices()[0].blackLevel = level;
    manager.renderOutputFrames(scenes, textures, renderer);
    const std::vector<unsigned char> lifted = readFrame(*manager.outputFrame(output->getId()));
    ASSERT_EQ(plain.size(), lifted.size());

    size_t onObject = 0;
    int worst = 0;
    for (size_t i = 0; i < plain.size(); i += 4) {
        if (plain[i] > 40 || plain[i + 1] > 40 || plain[i + 2] > 40) ++onObject;
        for (size_t c = 0; c < 3; ++c) {
            const float expected = 255.0f * level + (1.0f - level) * plain[i + c];
            worst = std::max(worst, static_cast<int>(std::lround(std::abs(lifted[i + c] - expected))));
        }
    }
    EXPECT_GT(onObject, plain.size() / 4 / 20) << "the cube must be in the picture";
    EXPECT_LE(worst, 1) << "lifted = L + (1 - L) * frame, levels off at worst";

    renderer.shutdown();
    media.reset();
    manager.shutdown();
}

// The slice shader and its C++ mirror (sliceBlendGain, outsideBlendZones) agree at
// every pixel of a slice with four different blend zones and a black level: a swapped
// edge, a wrong ramp or a broken black-level branch shows as tens of levels.
TEST(CanvasSliceRenderTest, ShaderMatchesSliceBlendGainOnEveryEdge) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Edges");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 64, 48);
    int w = 0, h = 0;
    glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
    ASSERT_GT(w, 40);
    ASSERT_GT(h, 30);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    Scene& scene = *scenes.front();
    scene.setCanvasSize(IVec2(w, h));
    fullCanvasQuad(scene)->setOpacity(0.5f);  // grey, so the black level shows
    OutputManager::showScene(*output, &scene, scenes);
    Slice& slice = output->getSlices()[0];
    slice.blend[SLICE_EDGE_LEFT] = BlendEdge{9.0f, 2.2f, 2.0f};
    slice.blend[SLICE_EDGE_RIGHT] = BlendEdge{13.0f, 1.8f, 3.0f};
    slice.blend[SLICE_EDGE_TOP] = BlendEdge{7.0f, 2.4f, 1.0f};
    slice.blend[SLICE_EDGE_BOTTOM] = BlendEdge{11.0f, 2.0f, 1.5f};
    slice.blackLevel = 0.08f;

    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);
    const std::vector<unsigned char> canvas = readFrame(*manager.sceneCanvas(scene.getId()));
    const std::vector<unsigned char> frame = readFrame(*manager.outputFrame(output->getId()));
    ASSERT_EQ(frame.size(), static_cast<size_t>(w) * h * 4);
    const float content = canvas[(static_cast<size_t>(h / 2) * w + w / 2) * 4] / 255.0f;
    ASSERT_NEAR(content, 0.5f, 0.01f);

    int worst = 0;
    size_t lifted = 0;
    for (int row = 0; row < h; ++row) {
        const int y = h - 1 - row;  // frames are bottom-up; slices are y down
        for (int x = 0; x < w; ++x) {
            const Vec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            const bool outside = outsideBlendZones(slice, p);
            const float expected = outside ? slice.blackLevel + (1.0f - slice.blackLevel) * content
                                           : content * sliceBlendGain(slice, p);
            if (outside) ++lifted;
            const int got = frame[(static_cast<size_t>(row) * w + x) * 4];
            const int delta = static_cast<int>(std::lround(std::abs(got - 255.0f * expected)));
            worst = std::max(worst, delta);
            EXPECT_LE(delta, 2) << "pixel " << x << ", " << y;
        }
    }
    EXPECT_GT(lifted, 0u);
    std::printf("[ slice ] shader vs sliceBlendGain: worst %d levels over %d pixels\n", worst, w * h);

    renderer.shutdown();
    manager.shutdown();
}

// Only quads are drawn into a scene canvas, so it has no depth buffer (16 MB a scene
// at 1080p); an output frame, which 3D objects are drawn into, keeps one.
TEST(CanvasSliceRenderTest, CanvasIsColourOnly) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    OutputManager manager;
    ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
    OutputWindow* output = manager.createOutput("Depth");
    ASSERT_NE(output, nullptr);
    glfwSetWindowSize(output->getGLFWwindow(), 32, 32);
    glfwMakeContextCurrent(gl.win);

    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.front()->setCanvasSize(IVec2(64, 32));
    fullCanvasQuad(*scenes.front());
    OutputManager::showScene(*output, scenes.front().get(), scenes);
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());
    manager.renderOutputFrames(scenes, textures, renderer);

    auto depthType = [](const Framebuffer& target) {
        target.bind();
        GLint type = -1;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        Framebuffer::unbind();
        return type;
    };
    ASSERT_NE(manager.sceneCanvas(scenes.front()->getId()), nullptr);
    EXPECT_EQ(depthType(*manager.sceneCanvas(scenes.front()->getId())), GL_NONE);
    EXPECT_EQ(depthType(*manager.outputFrame(output->getId())), GL_RENDERBUFFER);

    renderer.shutdown();
    manager.shutdown();
}

// ---------------------------------------------------------------------------
// The calibration view (TD-29): the calibrated output's frame, drawn by
// renderSceneFrame as the output frames are. A pick on it maps to a projector pixel
// by canvasToPixel, whatever the slicing.
// ---------------------------------------------------------------------------

namespace {

// One slicing case: the scene's canvas size (in frames) and the slices of the output
// the view shows (`partner` gives a second output showing the scene too, for a pair).
struct CalibrationViewCase {
    const char* name;
    int canvasFrames;  // canvas width in frame widths
    std::vector<Slice> (*slices)(IVec2 canvas, int w, int h);
    std::vector<Slice> (*partner)(IVec2 canvas, int w, int h);
};

// Where the view shows things, one case: the lit pixels of the objects alone, and the
// lit pixels of the marker quad alone, as frame-normalised points (+y down).
struct CalibrationViewShot {
    std::vector<unsigned char> objects;
    std::vector<Vec2> markerPoints;
};

// The marker quad on the canvas, normalised (inside every case's slice input).
constexpr float MARKER_X0 = 0.48f, MARKER_X1 = 0.72f, MARKER_Y0 = 0.40f, MARKER_Y1 = 0.50f;

}  // namespace

TEST(CalibrationViewTest, PicksMapToProjectorPixelsWhateverTheSlicing) {
    EditorGL gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    const std::vector<CalibrationViewCase> cases = {
        {"(a) default slicing", 1,
         [](IVec2 canvas, int, int) { return std::vector<Slice>{makeFullCanvasSlice(canvas)}; },
         nullptr},
        {"(b) offset and scaled slice", 2,
         [](IVec2, int w, int h) {
             Slice s = makeFullCanvasSlice(IVec2(1, 1));
             s.input = CanvasRect{0.5f * w, 0.1f * h, 1.0f * w, 0.8f * h};
             return std::vector<Slice>{s};
         },
         nullptr},
        {"(c) warped slice", 2,
         [](IVec2 canvas, int, int) {
             Slice s = makeFullCanvasSlice(canvas);
             s.warp.setCorners({Vec2(0.10f, 0.05f), Vec2(0.95f, 0.0f), Vec2(0.85f, 1.0f),
                                Vec2(0.0f, 0.90f)});
             return std::vector<Slice>{s};
         },
         nullptr},
        {"(d) right of a blended, keystoned pair", 2,
         [](IVec2, int w, int h) {
             Slice s = makeFullCanvasSlice(IVec2(1, 1));
             s.input = CanvasRect{0.8f * w, 0.0f, 1.2f * w, 1.0f * h};
             s.blend[SLICE_EDGE_LEFT].width = 0.4f * w;
             s.warp.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.06f), Vec2(0.97f, 0.95f),
                                Vec2(0.02f, 1.0f)});
             return std::vector<Slice>{s};
         },
         [](IVec2, int w, int h) {
             Slice s = makeFullCanvasSlice(IVec2(1, 1));
             s.input = CanvasRect{0.0f, 0.0f, 1.2f * w, 1.0f * h};
             s.blend[SLICE_EDGE_RIGHT].width = 0.4f * w;
             return std::vector<Slice>{s};
         }},
    };

    // Ground truth: the cube's corners as the projector projects them (the bounding
    // box of its silhouette, in projector pixels).
    Vec2 truthMin(1e9f), truthMax(-1e9f);
    ProjectorCamera projector;
    std::vector<CalibrationViewShot> shots;
    int w = 0, h = 0;

    for (const CalibrationViewCase& c : cases) {
        SCOPED_TRACE(c.name);
        OutputManager manager;
        ASSERT_TRUE(manager.initialize(gl.win).isSuccess());
        OutputWindow* output = manager.createOutput("Calibrated");
        ASSERT_NE(output, nullptr);
        glfwSetWindowSize(output->getGLFWwindow(), 96, 64);
        glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
        OutputWindow* partner = c.partner ? manager.createOutput("Partner") : nullptr;
        glfwMakeContextCurrent(gl.win);

        auto media = patternTexture(32, 4);
        std::vector<std::unique_ptr<Scene>> scenes;
        scenes.push_back(std::make_unique<Scene>());
        Scene& scene = *scenes.front();
        const IVec2 canvas(c.canvasFrames * w, h);
        scene.setCanvasSize(canvas);
        std::unordered_map<UUID, Texture*> textures;
        buildFeatureScene(scene, {media.get()}, textures);  // the cube
        std::vector<UUID> quadIds;
        for (const auto& quad : scene.getQuadManager().getQuads()) quadIds.push_back(quad->getId());
        for (const UUID& id : quadIds) scene.getQuadManager().deleteQuad(id);
        Quad* marker = scene.getQuadManager().createQuad("Marker");
        marker->getGeometry().setCorners({Vec2(MARKER_X0, MARKER_Y0), Vec2(MARKER_X1, MARKER_Y0),
                                          Vec2(MARKER_X1, MARKER_Y1), Vec2(MARKER_X0, MARKER_Y1)});
        OutputManager::showScene(*output, &scene, scenes);
        output->getSlices() = c.slices(canvas, w, h);
        if (partner) {
            OutputManager::showScene(*partner, &scene, scenes);
            partner->getSlices() = c.partner(canvas, w, h);
        }
        Vec3 lo, hi;
        ASSERT_TRUE(scene.visibleObjectBounds(lo, hi));
        output->getProjector() = ProjectorCamera::fitToOutput(ProjectorCamera{}, w, h, &lo, &hi);
        projector = output->getProjector();
        truthMin = Vec2(1e9f);
        truthMax = Vec2(-1e9f);
        const MeshObject& cube = *scene.getObjects().front();
        for (const Vec3& local : cube.getMesh().positions) {
            Vec2 pixel;
            ASSERT_TRUE(projector.project(Vec3(cube.getTransform() * glm::vec4(local, 1.0f)), pixel));
            truthMin = glm::min(truthMin, pixel);
            truthMax = glm::max(truthMax, pixel);
        }

        Renderer renderer;
        ASSERT_TRUE(renderer.initialize());
        // The view as the editor draws it: the canvases from the last output frames,
        // then the calibrated output's frame over black.
        auto view = [&]() {
            manager.renderOutputFrames(scenes, textures, renderer);
            Framebuffer target(Framebuffer::Depth::With);
            EXPECT_TRUE(target.resize(w, h));
            target.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            manager.renderSceneFrame(*output, scene, IVec2(w, h), textures, renderer);
            renderer.endFrame();
            std::vector<unsigned char> px = readBound(w, h);
            Framebuffer::unbind();
            return px;
        };
        CalibrationViewShot shot;
        marker->setVisible(false);
        shot.objects = view();
        marker->setVisible(true);
        const std::vector<unsigned char> both = view();
        scene.getObjects().front()->setVisible(false);
        const std::vector<unsigned char> quads = view();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;  // bottom-up rows
                if (quads[i] + quads[i + 1] + quads[i + 2] > 60) {
                    shot.markerPoints.emplace_back((x + 0.5f) / w, (h - 1 - y + 0.5f) / h);
                }
            }
        }

        // Draw order: the canvas lies over the objects. Where the opaque marker covers
        // the cube (two pixels inside the marker, on the cube, outside the slice's blend
        // zones), the view with both is the marker alone; where the canvas is transparent
        // (nothing of the marker within two pixels), it is the objects alone.
        const Slice& slice = output->getSlices().front();
        auto lit = [&](const std::vector<unsigned char>& px, int x, int y) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            return px[i] + px[i + 1] + px[i + 2] > 0;
        };
        auto outsideBlendZones = [&](int x, int y) {
            const Vec2 uv = slice.warp.surfaceParamsAt(Vec2((x + 0.5f) / w, (h - 1 - y + 0.5f) / h));
            if (uv.x < 0.0f) return false;
            const float dLeft = uv.x * slice.input.width, dTop = uv.y * slice.input.height;
            return dLeft > slice.blend[SLICE_EDGE_LEFT].width + 1.0f &&
                   slice.input.width - dLeft > slice.blend[SLICE_EDGE_RIGHT].width + 1.0f &&
                   dTop > slice.blend[SLICE_EDGE_TOP].width + 1.0f &&
                   slice.input.height - dTop > slice.blend[SLICE_EDGE_BOTTOM].width + 1.0f;
        };
        int coveredCube = 0, coveredWrong = 0, uncoveredWrong = 0;
        for (int y = 2; y < h - 2; ++y) {
            for (int x = 2; x < w - 2; ++x) {
                int markerAround = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) markerAround += lit(quads, x + dx, y + dy) ? 1 : 0;
                }
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                int diffQuads = 0, diffObjects = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    diffQuads = std::max(diffQuads, std::abs(both[i + ch] - quads[i + ch]));
                    diffObjects = std::max(diffObjects, std::abs(both[i + ch] - shot.objects[i + ch]));
                }
                if (markerAround == 25 && lit(shot.objects, x, y)) {
                    if (!outsideBlendZones(x, y)) continue;
                    ++coveredCube;
                    if (diffQuads > 1) ++coveredWrong;
                } else if (markerAround == 0 && diffObjects > 1) {
                    ++uncoveredWrong;
                }
            }
        }
        EXPECT_GT(coveredCube, 10) << "the marker must lie over the cube";
        EXPECT_EQ(coveredWrong, 0) << "the objects show through the canvas";
        EXPECT_EQ(uncoveredWrong, 0) << "the transparent canvas changed the objects";

        // 2D: the view shows the canvas through this output's slice. Every lit pixel,
        // taken back through the slice's warp (surfaceParamsAt) and input rectangle,
        // lands on the marker quad of the canvas, within a canvas pixel.
        EXPECT_GT(shot.markerPoints.size(), 10u) << "the marker must be in the view";
        float worst = 0.0f;
        for (const Vec2& f : shot.markerPoints) {
            const Vec2 uv = slice.warp.surfaceParamsAt(f);
            ASSERT_GE(uv.x, 0.0f) << "a lit pixel outside the slice's warp";
            const Vec2 onCanvas(slice.input.x + uv.x * slice.input.width,
                                slice.input.y + uv.y * slice.input.height);
            const float dx = std::max({MARKER_X0 * canvas.x - onCanvas.x, onCanvas.x - MARKER_X1 * canvas.x, 0.0f});
            const float dy = std::max({MARKER_Y0 * canvas.y - onCanvas.y, onCanvas.y - MARKER_Y1 * canvas.y, 0.0f});
            worst = std::max(worst, std::max(dx, dy));
        }
        EXPECT_LE(worst, 1.0f) << "canvas pixels off the marker";
        shots.push_back(std::move(shot));

        renderer.shutdown();
        media.reset();
        manager.shutdown();
    }

    // Where the view shows the cube, against the ground truth.
    const ProjectorIntrinsics& k = projector.intrinsics;
    ASSERT_EQ(k.width, w);
    ASSERT_EQ(k.height, h);
    for (size_t s = 0; s < shots.size(); ++s) {
        SCOPED_TRACE(cases[s].name);
        // 3D: the objects are drawn in projector space, never through the slices, so
        // they are the same picture whatever the slicing.
        EXPECT_EQ(shots[s].objects, shots.front().objects) << "slicing moved the 3D objects";
        // Picks at the silhouette's edges on the view map (canvasToPixel) to the
        // projected bounding box, within a pixel.
        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                const auto& px = shots[s].objects;
                if (px[i] + px[i + 1] + px[i + 2] <= 30) continue;
                const int top = h - 1 - y;
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, top); maxY = std::max(maxY, top);
            }
        }
        ASSERT_GE(maxX, 0) << "the cube must be in the view";
        const Vec2 pickedMin = canvasToPixel(Vec2(static_cast<float>(minX) / w,
                                                  static_cast<float>(minY) / h), k);
        const Vec2 pickedMax = canvasToPixel(Vec2(static_cast<float>(maxX + 1) / w,
                                                  static_cast<float>(maxY + 1) / h), k);
        EXPECT_NEAR(pickedMin.x, truthMin.x, 1.0f);
        EXPECT_NEAR(pickedMin.y, truthMin.y, 1.0f);
        EXPECT_NEAR(pickedMax.x, truthMax.x, 1.0f);
        EXPECT_NEAR(pickedMax.y, truthMax.y, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Scene crossfades (ADR-047 §5, QM-121): readback of the mixed frames.
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<Texture> solidTexture(unsigned char r, unsigned char g, unsigned char b) {
    std::vector<unsigned char> px(4 * 4 * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    auto texture = std::make_unique<Texture>();
    texture->create(4, 4, 4, px.data());
    return texture;
}

// One output and scenes of one solid colour each, filling their canvas.
struct FadeRig {
    EditorGL gl;
    OutputManager manager;
    OutputWindow* output = nullptr;
    int w = 0, h = 0;
    std::vector<std::unique_ptr<Scene>> scenes;
    std::vector<std::unique_ptr<Texture>> media;
    std::unordered_map<UUID, Texture*> textures;
    Renderer renderer;
    bool ok = false;

    FadeRig() {
        if (!gl.ok || !manager.initialize(gl.win).isSuccess()) return;
        output = manager.createOutput("Faded");
        if (!output) return;
        glfwSetWindowSize(output->getGLFWwindow(), 64, 32);
        glfwGetFramebufferSize(output->getGLFWwindow(), &w, &h);
        glfwMakeContextCurrent(gl.win);
        ok = renderer.initialize();
    }
    ~FadeRig() {
        if (!ok) return;
        renderer.shutdown();
        media.clear();
        manager.shutdown();
    }

    Scene* addScene(unsigned char r, unsigned char g, unsigned char b, IVec2 canvas) {
        scenes.push_back(std::make_unique<Scene>());
        Scene* scene = scenes.back().get();
        scene->setCanvasSize(canvas);
        media.push_back(solidTexture(r, g, b));
        textures[fullCanvasQuad(*scene)->getId()] = media.back().get();
        return scene;
    }

    // The output's frame at frame-clock time `t` (fades retired first, as a frame does).
    std::vector<unsigned char> frameAt(double t) {
        manager.setFrameTime(t);
        manager.retireSceneFades(scenes);
        manager.renderOutputFrames(scenes, textures, renderer);
        return readFrame(*manager.outputFrame(output->getId()));
    }

    std::array<int, 3> centre(const std::vector<unsigned char>& px) const {
        const size_t i = (static_cast<size_t>(h / 2) * w + w / 2) * 4;
        return {px[i], px[i + 1], px[i + 2]};
    }
};

int worstDifference(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    int worst = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return a.size() == b.size() ? worst : 256;
}

}  // namespace

// Gate (ADR-047 §8): a 1 s crossfade on the GPU. At t = 0 the frame is the old scene
// byte for byte, at 0.5 the two mixed, at 1 the new scene byte-identical to a cut;
// stepped at 60 Hz every frame differs from the one before and the mix is monotonic.
// The canvas of the scene faded out is released when the fade ends.
TEST(SceneFadeRenderTest, OneSecondCrossfadeMixesTheCanvasesMonotonically) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* a = rig.addScene(240, 0, 0, IVec2(rig.w, rig.h));
    Scene* b = rig.addScene(0, 0, 240, IVec2(rig.w, rig.h));

    // References: each scene alone, routed as a cut.
    rig.manager.routeScene(*rig.output, b, rig.scenes);
    const std::vector<unsigned char> cutToB = rig.frameAt(0.0);
    rig.manager.routeScene(*rig.output, a, rig.scenes);
    const std::vector<unsigned char> onlyA = rig.frameAt(0.0);
    ASSERT_EQ(rig.centre(onlyA), (std::array<int, 3>{240, 0, 0}));
    ASSERT_EQ(rig.centre(cutToB), (std::array<int, 3>{0, 0, 240}));

    const long framebuffers = GlObjectStats::framebuffers();
    rig.manager.setFrameTime(100.0);
    rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
    EXPECT_EQ(worstDifference(rig.frameAt(100.0), onlyA), 0) << "t = 0: the old scene exactly";
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffers + 1) << "both canvases drawn";
    ASSERT_NE(rig.manager.sceneCanvas(a->getId()), nullptr);

    const std::array<int, 3> half = rig.centre(rig.frameAt(100.5));
    EXPECT_NEAR(half[0], 120, 1);
    EXPECT_EQ(half[1], 0);
    EXPECT_NEAR(half[2], 120, 1);

    std::vector<std::array<int, 3>> sequence;
    for (int frame = 0; frame <= 60; ++frame) {
        sequence.push_back(rig.centre(rig.frameAt(100.0 + frame / 60.0)));
    }
    for (size_t i = 1; i < sequence.size(); ++i) {
        EXPECT_LT(sequence[i][0], sequence[i - 1][0]) << "frame " << i << " repeats or goes back";
        EXPECT_GT(sequence[i][2], sequence[i - 1][2]) << "frame " << i;
    }
    EXPECT_TRUE(rig.manager.sceneFades().empty());
    EXPECT_EQ(worstDifference(rig.frameAt(101.0), cutToB), 0) << "t = 1: identical to a cut";
    EXPECT_EQ(rig.manager.sceneCanvas(a->getId()), nullptr) << "the faded-out canvas is released";
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffers);
}

// The mix happens before the blend zones and black level: mid-fade, a slice with both
// shows exactly what it would show for one scene of the mixed colour. The scene fading
// out has a canvas of another size (the filtered path).
TEST(SceneFadeRenderTest, BlendZonesAndBlackLevelApplyToTheMix) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* a = rig.addScene(200, 40, 0, IVec2(rig.w * 2, rig.h * 2));
    Scene* b = rig.addScene(0, 120, 200, IVec2(rig.w, rig.h));
    Scene* mixed = rig.addScene(100, 80, 100, IVec2(rig.w, rig.h));
    rig.manager.routeScene(*rig.output, a, rig.scenes);
    rig.frameAt(0.0);

    rig.manager.takeScene(*rig.output, b, rig.scenes, 2.0);
    Slice& slice = rig.output->getSlices()[0];
    slice.blend[SLICE_EDGE_RIGHT] = BlendEdge{static_cast<float>(rig.w) / 2.0f, 2.2f, 2.0f};
    slice.blend[SLICE_EDGE_TOP] = BlendEdge{8.0f, 1.8f, 3.0f};
    slice.blackLevel = 0.1f;
    const std::vector<unsigned char> fading = rig.frameAt(1.0);  // mix 0.5
    ASSERT_NE(rig.manager.sceneFade(*rig.output), nullptr);

    OutputManager::showScene(*rig.output, mixed, rig.scenes);  // same size: slices unchanged
    const std::vector<unsigned char> reference = rig.frameAt(1.0);
    EXPECT_LE(worstDifference(fading, reference), 1);
}

// An output that showed nothing fades its first scene in from black.
TEST(SceneFadeRenderTest, AnOutputShowingNothingFadesInFromBlack) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* b = rig.addScene(0, 200, 0, IVec2(rig.w, rig.h));
    rig.manager.setFrameTime(0.0);
    rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
    EXPECT_EQ(rig.centre(rig.frameAt(0.0)), (std::array<int, 3>{0, 0, 0}));
    const std::array<int, 3> half = rig.centre(rig.frameAt(0.5));
    EXPECT_NEAR(half[1], 100, 1);
    EXPECT_EQ(rig.centre(rig.frameAt(1.0)), (std::array<int, 3>{0, 200, 0}));
}

// Blackout mid-fade: black while on (no canvas drawn), and the fade is where the wall
// clock says when it lifts.
TEST(SceneFadeRenderTest, BlackoutDuringAFadeIsBlackAndTheClockRunsOn) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* a = rig.addScene(240, 0, 0, IVec2(rig.w, rig.h));
    Scene* b = rig.addScene(0, 0, 240, IVec2(rig.w, rig.h));
    rig.manager.routeScene(*rig.output, a, rig.scenes);
    rig.frameAt(0.0);
    rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
    rig.manager.setBlackout(true);
    EXPECT_EQ(rig.centre(rig.frameAt(0.25)), (std::array<int, 3>{0, 0, 0}));
    rig.manager.setBlackout(false);
    const std::array<int, 3> later = rig.centre(rig.frameAt(0.75));
    EXPECT_NEAR(later[0], 60, 1);
    EXPECT_NEAR(later[2], 180, 1);
}

namespace {

// Worst per-channel difference between `got` and weights[i] x refs[i] summed, over the
// colour channels of every pixel.
int worstAgainstMix(const std::vector<unsigned char>& got,
                    const std::vector<std::pair<float, const std::vector<unsigned char>*>>& refs) {
    int worst = 0;
    for (const auto& ref : refs) {
        if (ref.second->size() != got.size()) return 256;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (i % 4 == 3) continue;
        float expected = 0.0f;
        for (const auto& ref : refs) expected += ref.first * (*ref.second)[i];
        worst = std::max(worst, static_cast<int>(std::lround(std::abs(got[i] - expected))));
    }
    return worst;
}

// Pixels the object covers in a frame (any colour channel lit).
size_t litPixels(const std::vector<unsigned char>& px) {
    size_t n = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i] + px[i + 1] + px[i + 2] > 30) ++n;
    }
    return n;
}

// A scene holding the feature scene's cube, textured with `texture` (by default a
// pattern), and no quads.
Scene* addCubeScene(FadeRig& rig, Texture* texture = nullptr) {
    rig.scenes.push_back(std::make_unique<Scene>());
    Scene* scene = rig.scenes.back().get();
    scene->setCanvasSize(IVec2(rig.w, rig.h));
    if (!texture) {
        if (rig.media.empty()) rig.media.push_back(patternTexture(32, 4));
        texture = rig.media.front().get();
    }
    buildFeatureScene(*scene, {texture}, rig.textures);
    std::vector<UUID> quadIds;
    for (const auto& quad : scene->getQuadManager().getQuads()) quadIds.push_back(quad->getId());
    for (const UUID& id : quadIds) scene->getQuadManager().deleteQuad(id);
    return scene;
}

// The output's projector fitted onto the cube.
void fitProjectorToCube(FadeRig& rig, const Scene& scene) {
    Vec3 lo, hi;
    ASSERT_TRUE(scene.visibleObjectBounds(lo, hi));
    rig.output->getProjector() = ProjectorCamera::fitToOutput(ProjectorCamera{}, rig.w, rig.h, &lo, &hi);
}

// Scenes filling their canvas with a patterned texture (every texel different).
Scene* addPatternScene(FadeRig& rig, int seed, IVec2 canvas) {
    rig.scenes.push_back(std::make_unique<Scene>());
    Scene* scene = rig.scenes.back().get();
    scene->setCanvasSize(canvas);
    scene->setCanvasLocked(true);
    rig.media.push_back(patternTexture(64, seed));
    rig.textures[fullCanvasQuad(*scene)->getId()] = rig.media.back().get();
    return scene;
}

}  // namespace

// 3D objects dissolve exactly (ADR-047 §5): the same cube in both scenes stays as it
// is through the fade (no ghosting of hidden faces, no dip), each scene's objects
// rendering opaque into their own layer. The layers are allocated once for the fade
// and released when it ends.
TEST(SceneFadeRenderTest, TheSameObjectInBothScenesIsConstantThroughAFade) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* a = addCubeScene(rig);
    Scene* b = addCubeScene(rig);
    rig.manager.routeScene(*rig.output, a, rig.scenes);
    fitProjectorToCube(rig, *a);
    const std::vector<unsigned char> plain = rig.frameAt(100.0);
    ASSERT_GT(litPixels(plain), 100u) << "the cube is in view";

    const long framebuffers = GlObjectStats::framebuffers();
    rig.manager.setFrameTime(100.0);
    rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
    long during = 0;
    for (double t : {0.25, 0.5, 0.75}) {
        const std::vector<unsigned char> mid = rig.frameAt(100.0 + t);
        ASSERT_NE(rig.manager.sceneFade(*rig.output), nullptr);
        EXPECT_LE(worstDifference(mid, plain), 2) << "mix " << t;
        if (during == 0) during = GlObjectStats::framebuffers();
        EXPECT_EQ(GlObjectStats::framebuffers(), during) << "no allocation per frame of a fade";
    }
    EXPECT_GT(during, framebuffers + 1) << "object layers while fading";
    EXPECT_LE(worstDifference(rig.frameAt(101.0), plain), 0);
    rig.frameAt(101.1);
    EXPECT_EQ(GlObjectStats::framebuffers(), framebuffers) << "layers and the old canvas released";
}

// An object fading in from black is its weight times its steady image, and one fading
// out to a scene without objects is (1 - weight) times it; both within 2 levels.
TEST(SceneFadeRenderTest, AnObjectFadesAsItsWeightTimesItsSteadyImage) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* cube = addCubeScene(rig);
    rig.scenes.push_back(std::make_unique<Scene>());
    Scene* empty = rig.scenes.back().get();
    empty->setCanvasSize(IVec2(rig.w, rig.h));
    rig.manager.routeScene(*rig.output, cube, rig.scenes);
    fitProjectorToCube(rig, *cube);
    const std::vector<unsigned char> plain = rig.frameAt(100.0);
    ASSERT_GT(litPixels(plain), 100u);

    OutputManager::showScene(*rig.output, nullptr, rig.scenes);
    rig.frameAt(100.0);
    rig.manager.setFrameTime(100.0);
    rig.manager.takeScene(*rig.output, cube, rig.scenes, 1.0);
    for (double t : {0.25, 0.5, 0.75}) {
        EXPECT_LE(worstAgainstMix(rig.frameAt(100.0 + t), {{static_cast<float>(t), &plain}}), 2)
            << "fading in from black, mix " << t;
    }
    rig.frameAt(102.0);
    rig.manager.setFrameTime(200.0);
    rig.manager.takeScene(*rig.output, empty, rig.scenes, 1.0);
    for (double t : {0.25, 0.5, 0.75}) {
        EXPECT_LE(worstAgainstMix(rig.frameAt(200.0 + t), {{static_cast<float>(1.0 - t), &plain}}), 2)
            << "fading out to a scene without objects, mix " << t;
    }
}

// The outgoing canvas is sampled through its own size and turn (patterned content, so
// a flipped, unturned or rescaled read shows): mid-fade the frame is A(1 - m) + B m of
// the two scenes' own frames, within 2 levels, when both canvases match (the texel
// path), when the outgoing one is turned differently, and when it is another size.
TEST(SceneFadeRenderTest, TheOutgoingCanvasIsSampledThroughItsOwnSizeAndTurn) {
    struct Case {
        const char* name;
        Rotation rotation;   // the faded output's rotation
        bool twinOutput;     // a second, unrotated output shows B throughout: B's canvas unturned
        IVec2 canvasA;       // in units of the output's effective size
    };
    const Case cases[] = {{"same size and turn", Rotation::R0, false, IVec2(1, 1)},
                          {"outgoing canvas turned, incoming not", Rotation::R90, true, IVec2(1, 1)},
                          {"outgoing canvas half the size", Rotation::R0, false, IVec2(2, 2)}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        FadeRig rig;
        if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
        OutputWindow* twin = nullptr;
        if (c.twinOutput) {
            twin = rig.manager.createOutput("Twin");
            ASSERT_NE(twin, nullptr);
            glfwSetWindowSize(twin->getGLFWwindow(), rig.w, rig.h);
            glfwMakeContextCurrent(rig.gl.win);
        }
        rig.output->setRotation(c.rotation);
        const bool quarter = c.rotation == Rotation::R90 || c.rotation == Rotation::R270;
        const IVec2 frameEffective = quarter ? IVec2(rig.h, rig.w) : IVec2(rig.w, rig.h);
        Scene* a = addPatternScene(rig, 1, IVec2(frameEffective.x / c.canvasA.x, frameEffective.y / c.canvasA.y));
        Scene* b = addPatternScene(rig, 2, frameEffective);
        if (twin) OutputManager::showScene(*twin, b, rig.scenes);

        OutputManager::showScene(*rig.output, b, rig.scenes);
        const std::vector<unsigned char> frameB = rig.frameAt(50.0);
        OutputManager::showScene(*rig.output, a, rig.scenes);
        const std::vector<unsigned char> frameA = rig.frameAt(60.0);
        ASSERT_GT(worstDifference(frameA, frameB), 60) << "the scenes differ";

        rig.manager.setFrameTime(100.0);
        rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
        for (double t : {0.25, 0.5}) {
            const std::vector<unsigned char> mid = rig.frameAt(100.0 + t);
            ASSERT_NE(rig.manager.sceneFade(*rig.output), nullptr);
            const float m = static_cast<float>(t);
            EXPECT_LE(worstAgainstMix(mid, {{1.0f - m, &frameA}, {m, &frameB}}), 2) << "mix " << t;
        }
        if (c.twinOutput) {
            EXPECT_NE(rig.manager.sceneCanvasRotation(a->getId()), rig.manager.sceneCanvasRotation(b->getId()))
                << "the case needs canvases in different turns";
        }
    }
}

// A take of a third scene mid-fade does not jump (ADR-047 §5): A -> B at mix 0.1, then
// C. Frame after frame at 60 Hz the picture is (1 - m2)((1 - w)A + wB) + m2 C with w
// frozen at 0.1, within 2 levels, both outgoing canvases drawn, and no frame differs
// from the one before by more than one frame's step of the fade.
TEST(SceneFadeRenderTest, AThirdTakeMidFadeDoesNotJump) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* a = addPatternScene(rig, 1, IVec2(rig.w, rig.h));
    Scene* b = addPatternScene(rig, 2, IVec2(rig.w, rig.h));
    Scene* c = addPatternScene(rig, 3, IVec2(rig.w, rig.h));
    std::vector<std::vector<unsigned char>> alone;
    for (Scene* s : {a, b, c}) {
        OutputManager::showScene(*rig.output, s, rig.scenes);
        alone.push_back(rig.frameAt(10.0));
    }
    // One frame's step: the largest change a 1 s fade makes in 1/60 s between any two
    // of the scenes, plus rounding.
    const int step = static_cast<int>(std::ceil(std::max({worstDifference(alone[0], alone[1]),
                                                          worstDifference(alone[0], alone[2]),
                                                          worstDifference(alone[1], alone[2])}) / 60.0)) + 2;

    OutputManager::showScene(*rig.output, a, rig.scenes);
    rig.frameAt(99.0);
    const long oneCanvas = GlObjectStats::framebuffers();
    const double start = 100.0;
    rig.manager.setFrameTime(start);
    rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
    std::vector<unsigned char> previous = rig.frameAt(start);
    float frozen = -1.0f;
    for (int frame = 1; frame <= 70; ++frame) {
        const double now = start + frame / 60.0;
        if (frame == 6) {  // A -> B at 0.1
            rig.manager.setFrameTime(now);
            rig.manager.takeScene(*rig.output, c, rig.scenes, 1.0);
            const SceneFade* fade = rig.manager.sceneFade(*rig.output);
            ASSERT_NE(fade, nullptr);
            ASSERT_TRUE(fade->nested());
            frozen = fade->fromNextWeight;
            EXPECT_NEAR(frozen, 0.1f, 1e-4f);
        }
        const std::vector<unsigned char> current = rig.frameAt(now);
        EXPECT_LE(worstDifference(current, previous), step) << "frame " << frame << " jumps";
        if (frame >= 6 && rig.manager.sceneFade(*rig.output)) {
            const float m2 = static_cast<float>((now - (start + 6 / 60.0)) / 1.0);
            EXPECT_LE(worstAgainstMix(current, {{(1.0f - m2) * (1.0f - frozen), &alone[0]},
                                                {(1.0f - m2) * frozen, &alone[1]},
                                                {m2, &alone[2]}}), 2)
                << "frame " << frame;
            EXPECT_EQ(GlObjectStats::framebuffers(), oneCanvas + 2) << "A and B keep rendering";
        }
        previous = current;
    }
    EXPECT_TRUE(rig.manager.sceneFades().empty());
    EXPECT_EQ(worstDifference(previous, alone[2]), 0) << "ends on C exactly";
    EXPECT_EQ(GlObjectStats::framebuffers(), oneCanvas);
}

// The same rule for 3D objects: a cube only in B, A -> B, a take of C (no objects) at
// B's 0.4. The cube stays at 0.4 at the take and fades with the frozen side.
TEST(SceneFadeRenderTest, ObjectsFollowAThirdTakeWithoutAJump) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* b = addCubeScene(rig);
    rig.scenes.push_back(std::make_unique<Scene>());
    Scene* a = rig.scenes.back().get();
    a->setCanvasSize(IVec2(rig.w, rig.h));
    rig.scenes.push_back(std::make_unique<Scene>());
    Scene* c = rig.scenes.back().get();
    c->setCanvasSize(IVec2(rig.w, rig.h));
    rig.manager.routeScene(*rig.output, b, rig.scenes);
    fitProjectorToCube(rig, *b);
    const std::vector<unsigned char> plain = rig.frameAt(10.0);
    ASSERT_GT(litPixels(plain), 100u);

    OutputManager::showScene(*rig.output, a, rig.scenes);
    rig.frameAt(99.0);
    rig.manager.setFrameTime(100.0);
    rig.manager.takeScene(*rig.output, b, rig.scenes, 1.0);
    const std::vector<unsigned char> before = rig.frameAt(100.4);
    EXPECT_LE(worstAgainstMix(before, {{0.4f, &plain}}), 2);
    rig.manager.takeScene(*rig.output, c, rig.scenes, 1.0);
    EXPECT_LE(worstDifference(rig.frameAt(100.4), before), 1) << "no jump at the take";
    EXPECT_LE(worstAgainstMix(rig.frameAt(100.9), {{0.2f, &plain}}), 2) << "fades with the frozen side";
}

// A quad in one scene and an object in its place in the next dissolve as the mix of
// the two scenes' frames, not the mixed canvas over the mixed objects: a full white
// quad crossfading to a white cube and no quad stays white where the cube is (it
// dipped to 191 at mix 0.5 when both were mixed separately).
TEST(SceneFadeRenderTest, AQuadFadingToAnObjectInItsPlaceDoesNotDip) {
    FadeRig rig;
    if (!rig.ok) GTEST_SKIP() << "No GL 3.3 context available";
    Scene* quad = rig.addScene(255, 255, 255, IVec2(rig.w, rig.h));
    rig.media.push_back(solidTexture(255, 255, 255));
    Scene* cube = addCubeScene(rig, rig.media.back().get());
    fitProjectorToCube(rig, *cube);
    OutputManager::showScene(*rig.output, cube, rig.scenes);
    const std::vector<unsigned char> frameCube = rig.frameAt(10.0);
    ASSERT_GT(litPixels(frameCube), 100u) << "the cube is in view";
    size_t white = 0;
    for (size_t i = 0; i < frameCube.size(); i += 4) {
        if (frameCube[i] == 255 && frameCube[i + 1] == 255 && frameCube[i + 2] == 255) ++white;
    }
    ASSERT_GT(white, 100u) << "the cube is full white";
    OutputManager::showScene(*rig.output, quad, rig.scenes);
    const std::vector<unsigned char> frameQuad = rig.frameAt(20.0);
    ASSERT_EQ(rig.centre(frameQuad), (std::array<int, 3>{255, 255, 255}));

    rig.manager.setFrameTime(100.0);
    rig.manager.takeScene(*rig.output, cube, rig.scenes, 1.0);
    for (double t : {0.25, 0.5, 0.75}) {
        const std::vector<unsigned char> mid = rig.frameAt(100.0 + t);
        ASSERT_NE(rig.manager.sceneFade(*rig.output), nullptr);
        const float m = static_cast<float>(t);
        EXPECT_LE(worstAgainstMix(mid, {{1.0f - m, &frameQuad}, {m, &frameCube}}), 2) << "mix " << t;
        for (size_t i = 0; i < mid.size(); i += 4) {
            if (frameCube[i] == 255 && frameCube[i + 1] == 255 && frameCube[i + 2] == 255) {
                ASSERT_GE(mid[i], 253) << "the cube dips at mix " << t << ", pixel " << i / 4;
            }
        }
    }
}

// ---- Projector auto-setup (owner, 2026-11-23) --------------------------------------

TEST(ProjectorAutoSetupTest, DisplaysWithoutOutputSkipTheEditorsAndTakenOnes) {
    const std::vector<MonitorInfo> monitors = {monitorAt(0, "Laptop", IVec2(0, 0)),
                                               monitorAt(1, "Epson", IVec2(1920, 0)),
                                               monitorAt(2, "Optoma", IVec2(3840, 0)),
                                               monitorAt(3, "BenQ", IVec2(5760, 0))};
    std::vector<std::unique_ptr<OutputWindow>> outputs;
    EXPECT_EQ(OutputManager::displaysWithoutOutput(monitors, outputs, 0),
              (std::vector<int>{1, 2, 3}));

    outputs.push_back(std::make_unique<OutputWindow>());
    outputs.back()->setMonitor(2);  // Optoma has an output
    outputs.push_back(std::make_unique<OutputWindow>());
    outputs.back()->setMonitor(-1);  // waiting for a display: takes none
    EXPECT_EQ(OutputManager::displaysWithoutOutput(monitors, outputs, 0), (std::vector<int>{1, 3}));
    EXPECT_EQ(OutputManager::displaysWithoutOutput(monitors, outputs, 3), (std::vector<int>{0, 1}))
        << "the editor on another screen";

    EXPECT_EQ(OutputManager::displaysWithoutOutput(monitors, outputs, 0, /*includeEditorScreen=*/true),
              (std::vector<int>{1, 3, 0})) << "startup: the editor's screen too, last";
    outputs.push_back(std::make_unique<OutputWindow>());
    outputs.back()->setMonitor(0);
    EXPECT_EQ(OutputManager::displaysWithoutOutput(monitors, outputs, 0, true), (std::vector<int>{1, 3}))
        << "the editor's screen already has an output";

    const std::vector<MonitorInfo> laptop = {monitorAt(0, "Laptop", IVec2(0, 0))};
    EXPECT_TRUE(OutputManager::displaysWithoutOutput(laptop, {}, 0).empty())
        << "a hotplug never gives the editor's screen an output";
    EXPECT_TRUE(OutputManager::displaysWithoutOutput(laptop, {}, 0, true).empty())
        << "alone, the editor's screen keeps the Automatic preview instead";
}

TEST(ProjectorAutoSetupTest, NextProjectorNameFillsTheLowestGap) {
    std::vector<std::unique_ptr<OutputWindow>> outputs;
    EXPECT_EQ(OutputManager::nextProjectorName(outputs), "Projector 1");
    for (const char* name : {"Projector 1", "Stage left", "Projector 3"}) {
        outputs.push_back(std::make_unique<OutputWindow>());
        outputs.back()->setName(name);
    }
    EXPECT_EQ(OutputManager::nextProjectorName(outputs), "Projector 2");
    outputs.push_back(std::make_unique<OutputWindow>());
    outputs.back()->setName("Projector 2");
    EXPECT_EQ(OutputManager::nextProjectorName(outputs), "Projector 4");
}

TEST(ProjectorAutoSetupTest, WithoutDisplaysNothingIsAddedOrAdopted) {
    OutputManager manager;  // not initialized: no monitors
    EXPECT_TRUE(manager.addOutputsForUnusedDisplays().empty());
    EXPECT_EQ(manager.getOutputCount(), 0u);
    OutputWindow* output = manager.createOutput("Projector 1");
    ASSERT_NE(output, nullptr);
    EXPECT_FALSE(manager.adoptDisplayResolution(*output));
    EXPECT_EQ(output->getResolution(), IVec2(1920, 1080));
    EXPECT_TRUE(manager.takeOutputsPlacedByHotplug().empty());
}

} // namespace test
} // namespace quadmapper
