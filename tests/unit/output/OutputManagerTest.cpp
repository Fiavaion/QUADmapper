#include <gtest/gtest.h>
#include "output/OutputManager.hpp"

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

TEST(OutputManagerTest, RenderToOutputsIsSafeWhenNotInitialized) {
    OutputManager manager;
    manager.createOutput("Test");

    // Should not crash with no enabled/created outputs (no GL context here).
    std::vector<std::unique_ptr<Scene>> scenes;
    std::unordered_map<UUID, Texture*> textures;
    manager.renderToOutputs(scenes, textures);
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

// ---- Saved monitor identity (QM-087) -----------------------------------------

namespace {
MonitorInfo monitorAt(int index, const std::string& name, IVec2 position) {
    MonitorInfo info;
    info.index = index;
    info.name = name;
    info.position = position;
    return info;
}
}  // namespace

TEST(ResolveMonitorTest, NameAndPositionWinOverStaleIndex) {
    // Displays re-enumerated: the projector moved from index 1 to index 0.
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Epson", IVec2(2560, 0)),
                                         monitorAt(1, "Dell", IVec2(0, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 1, "Epson", IVec2(2560, 0)), 0);
}

TEST(ResolveMonitorTest, PositionDisambiguatesIdenticalNames) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Generic PnP", IVec2(0, 0)),
                                         monitorAt(1, "Generic PnP", IVec2(1920, 0)),
                                         monitorAt(2, "Generic PnP", IVec2(3840, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 0, "Generic PnP", IVec2(3840, 0)), 2);
}

TEST(ResolveMonitorTest, NameOnlyWhenPositionChanged) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 0, "Epson", IVec2(2560, 0)), 1);
}

TEST(ResolveMonitorTest, NameOnlyPrefersSavedIndexAmongSameNames) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Generic PnP", IVec2(0, 0)),
                                         monitorAt(1, "Generic PnP", IVec2(1920, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 1, "Generic PnP", IVec2(-500, 0)), 1);
}

TEST(ResolveMonitorTest, IndexWhenNameUnknownOrMissing) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0)),
                                         monitorAt(1, "Epson", IVec2(1920, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 1, "Barco", IVec2(0, 0)), 1);
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 1, "", IVec2(0, 0)), 1);  // pre-QM-087 file
}

TEST(ResolveMonitorTest, FallsBackToMonitorZero) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 3, "Epson", IVec2(1920, 0)), 0);
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, 3, "", IVec2(0, 0)), 0);
    EXPECT_EQ(OutputManager::resolveMonitor({}, 1, "Epson", IVec2(0, 0)), 0);
}

TEST(ResolveMonitorTest, UnassignedStaysUnassigned) {
    std::vector<MonitorInfo> monitors = {monitorAt(0, "Dell", IVec2(0, 0))};
    EXPECT_EQ(OutputManager::resolveMonitor(monitors, -1, "", IVec2(0, 0)), -1);
}

} // namespace test
} // namespace quadmapper
