#include <gtest/gtest.h>
#include "output/OutputManager.hpp"
#include "app/OutputPersistence.hpp"

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

} // namespace test
} // namespace quadmapper
