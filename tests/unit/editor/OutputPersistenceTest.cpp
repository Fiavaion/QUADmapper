// An output as the .qmap saves it (editor/OutputPersistence.hpp): the desired monitor
// identity, never a fallback, and its routing only while its scene exists. Moved out
// of output/OutputManagerTest.cpp (ADR-051 E8), so the output tests need no editor code.

#include "editor/OutputPersistence.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/Scene.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace quadmapper {
namespace test {

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
}  // namespace

// ---- Save writes the desired identity (QM-090) --------------------------------

TEST(OutputPersistenceTest, UnresolvedOutputSavesItsDesiredIdentity) {
    OutputManager manager;  // no GLFW: no monitors connected at all
    OutputWindow* output = manager.createOutput("Projector 1");
    ASSERT_NE(output, nullptr);
    output->setDesiredMonitor({"EPSON PJ", IVec2(1920, 0), 1});

    manager.resolveOutputs();
    EXPECT_EQ(output->getMonitor(), -1);

    Project::OutputData saved = outputToData(*output, {});
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

    Project::OutputData saved = outputToData(output, {});
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
    EXPECT_EQ(outputToData(*output, {}).monitor, -1);
}

// An output that shows no scene, or a deleted one, is saved with neither: its slices
// are in the pixels of a canvas the file would not name.
TEST(OutputPersistenceTest, UnroutedOutputSavesNoSceneAndNoSlices) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    OutputWindow shows, none, deleted;
    OutputManager::showScene(shows, scenes[0].get(), scenes);
    OutputManager::showScene(none, scenes[0].get(), scenes);
    OutputManager::showScene(none, nullptr, scenes);
    OutputManager::showScene(deleted, scenes[1].get(), scenes);
    scenes.pop_back();

    const Project::OutputData a = outputToData(shows, scenes);
    EXPECT_EQ(a.sceneId, scenes[0]->getId());
    EXPECT_EQ(a.slices.size(), 1u);
    for (const OutputWindow* o : {&none, &deleted}) {
        const Project::OutputData d = outputToData(*o, scenes);
        EXPECT_TRUE(d.sceneId.isNull());
        EXPECT_TRUE(d.slices.empty());
    }
}

}  // namespace test
}  // namespace quadmapper
