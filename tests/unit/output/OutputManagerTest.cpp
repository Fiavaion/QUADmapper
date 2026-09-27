#include <gtest/gtest.h>
#include "output/OutputManager.hpp"
#include "app/OutputPersistence.hpp"
#include "core/GlObjectStats.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "scene/MeshObject.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Scene.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
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

TEST(OutputManagerTest, SceneForOutputRoutesByAssignedOutputId) {
    std::vector<std::unique_ptr<Scene>> scenes;
    auto a = std::make_unique<Scene>();
    auto b = std::make_unique<Scene>();
    UUID outA = UUID::generate();
    UUID outB = UUID::generate();
    a->setAssignedOutputId(outA);
    // b intentionally left unassigned
    Scene* aPtr = a.get();
    scenes.push_back(std::move(a));
    scenes.push_back(std::move(b));

    EXPECT_EQ(OutputManager::sceneForOutput(scenes, outA), aPtr);   // routed
    EXPECT_EQ(OutputManager::sceneForOutput(scenes, outB), nullptr); // nothing assigned
    EXPECT_EQ(OutputManager::sceneForOutput(scenes, UUID::null()), nullptr);
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

// ---- Save writes the desired identity (QM-090) --------------------------------

TEST(OutputPersistenceTest, UnresolvedOutputSavesItsDesiredIdentity) {
    OutputManager manager;  // no GLFW: no monitors connected at all
    OutputWindow* output = manager.createOutput("Projector 1");
    ASSERT_NE(output, nullptr);
    output->setDesiredMonitor({"EPSON PJ", IVec2(1920, 0), 1});

    manager.resolveOutputs();
    EXPECT_EQ(output->getMonitor(), -1);

    Project::OutputData saved = outputToData(*output);
    EXPECT_EQ(saved.monitor, 1);
    EXPECT_EQ(saved.monitorName, "EPSON PJ");
    EXPECT_EQ(saved.monitorPosition, IVec2(1920, 0));
}

TEST(OutputPersistenceTest, FallbackIsNotWrittenBackToTheProject) {
    // The show opened with the projector unplugged fell back to a spare display.
    std::vector<MonitorInfo> monitors = {monitorAt(0, kGeneric, IVec2(0, 0)),
                                         monitorAt(1, "Dell", IVec2(-1920, 0))};
    OutputWindow output;
    output.setDesiredMonitor({kGeneric, IVec2(1920, 0), 1});
    auto r = OutputManager::resolveMonitor(monitors, output.getDesiredMonitor(), 0, {});
    ASSERT_EQ(r.match, MonitorMatch::SavedIndex);
    output.setMonitor(r.index);

    Project::OutputData saved = outputToData(output);
    EXPECT_EQ(saved.monitor, 1);
    EXPECT_EQ(saved.monitorName, kGeneric);
    EXPECT_EQ(saved.monitorPosition, IVec2(1920, 0));  // the projector, not the Dell

    OutputWindow reloaded;
    applyOutputData(saved, reloaded);
    EXPECT_EQ(reloaded.getDesiredMonitor().name, kGeneric);
    EXPECT_EQ(reloaded.getDesiredMonitor().position, IVec2(1920, 0));
    EXPECT_EQ(reloaded.getDesiredMonitor().savedIndex, 1);
}

TEST(OutputPersistenceTest, ExplicitChoiceBecomesTheDesiredIdentity) {
    OutputManager manager;
    OutputWindow* output = manager.createOutput("Projector 1");
    ASSERT_NE(output, nullptr);
    output->setDesiredMonitor({"EPSON PJ", IVec2(1920, 0), 1});

    manager.assignMonitor(*output, -1);  // "Automatic"
    EXPECT_TRUE(output->getDesiredMonitor().name.empty());
    EXPECT_EQ(output->getDesiredMonitor().savedIndex, -1);
    EXPECT_EQ(outputToData(*output).monitor, -1);
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
    MeshData& data = cube->getMesh();
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
    scene.setAssignedOutputId(output->getId());
    std::unordered_map<UUID, Texture*> textures;
    buildFeatureScene(scene, {mediaA.get(), mediaB.get(), mediaC.get()}, textures);
    Vec3 objectsMin, objectsMax;
    ASSERT_TRUE(scene.visibleObjectBounds(objectsMin, objectsMax));
    const IVec2 effective = output->getEffectiveResolution();
    output->getProjector() = ProjectorCamera::fitToOutput(
        ProjectorCamera{}, effective.x, effective.y, &objectsMin, &objectsMax);

    // New path, part 1: the editor context draws the output's frame.
    Renderer editorRenderer;
    ASSERT_TRUE(editorRenderer.initialize());
    manager.renderOutputFrames(scenes, textures, editorRenderer);
    const Framebuffer* frame = manager.outputFrame(output->getId());
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->width(), w);
    EXPECT_EQ(frame->height(), h);
    glFinish();

    std::vector<unsigned char> direct, copied;
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
        if (!param.blackout) {
            glDisable(GL_DEPTH_TEST);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            for (const Quad* quad : scene.getQuadsByZOrder()) {
                outputRenderer.renderQuad(*quad, textures.at(quad->getId()), false);
            }
            EXPECT_NE(readBound(w, h), direct) << "the 3D object drew nothing";
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
    EXPECT_EQ(differing, 0u) << "of " << direct.size() / 4 << " pixels";
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
                      ReadbackCase{Rotation::R90, true}),
    [](const ::testing::TestParamInfo<ReadbackCase>& info) {
        return "R" + std::to_string(static_cast<int>(info.param.rotation)) +
               (info.param.blackout ? "_Blackout" : "");
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

} // namespace test
} // namespace quadmapper
