// The Project tab's "(modified)" label (editor/UnsavedIndicator.hpp). Moved out of
// project/ProjectTest.cpp (ADR-051 E8), so the project tests need no editor code.

#include "editor/UnsavedIndicator.hpp"
#include "project/Project.hpp"
#include "project/ProjectSerializer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace quadmapper {
namespace test {

// The "(modified)" label follows the unsaved-changes rule (the show no longer
// serializes to what was saved or loaded): a show just opened reads unmodified, and
// so does an edit undone back to the loaded state.
TEST(UnsavedIndicatorTest, AnOpenedShowAndAnUndoneEditAreNotModified) {
    auto loaded = ProjectSerializer::load(std::filesystem::path(QM_PROJECT_FIXTURES_DIR) /
                                          "v3.4-canvas-slices.qmap");
    ASSERT_TRUE(loaded.isSuccess());
    Project project = std::move(loaded.value());
    const std::string saved = ProjectSerializer().serialize(project);
    auto dirty = [&] { return ProjectSerializer().serialize(project) != saved; };

    UnsavedIndicator label;
    EXPECT_FALSE(label.shown(10.0, dirty)) << "just opened";

    ASSERT_FALSE(project.getScenes().empty());
    ASSERT_FALSE(project.getScenes()[0].quads.empty());
    QuadGeometry& geometry = project.getScenes()[0].quads[0].getGeometry();
    const Vec2 corner = geometry.getCorners()[0];
    geometry.setCorner(0, corner + Vec2(0.05f, 0.0f));  // an edit
    label.invalidate();
    EXPECT_TRUE(label.shown(10.1, dirty)) << "edited";

    geometry.setCorner(0, corner);  // undone
    label.invalidate();
    EXPECT_FALSE(label.shown(10.2, dirty)) << "undone back to the loaded state";
}

// Serializing every frame costs too much: without invalidate() the answer is kept
// for REFRESH_SECONDS, then asked again.
TEST(UnsavedIndicatorTest, RefreshesAtMostEveryHalfSecondUnlessInvalidated) {
    UnsavedIndicator label;
    int asked = 0;
    bool dirty = false;
    auto check = [&] { ++asked; return dirty; };
    EXPECT_FALSE(label.shown(1.0, check));
    dirty = true;
    EXPECT_FALSE(label.shown(1.2, check));
    EXPECT_EQ(asked, 1);
    EXPECT_TRUE(label.shown(1.0 + UnsavedIndicator::REFRESH_SECONDS, check));
    EXPECT_EQ(asked, 2);
    dirty = false;
    label.invalidate();
    EXPECT_FALSE(label.shown(1.6, check));
    EXPECT_EQ(asked, 3);
}

}  // namespace test
}  // namespace quadmapper
