#include <gtest/gtest.h>
#include "output/OutputWindow.hpp"

#include <GLFW/glfw3.h>

#include <array>
#include <memory>
#include <type_traits>

namespace quadmapper {
namespace test {

// Tests for OutputWindow properties and scene assignments (non-GLFW tests)
// GLFW-dependent functionality requires integration tests with an OpenGL context

TEST(OutputWindowTest, DefaultConstructorCreatesValidWindow) {
    OutputWindow window;

    EXPECT_FALSE(window.getId().isNull());
    EXPECT_EQ(window.getName(), "Output");
    EXPECT_TRUE(window.isEnabled());
    EXPECT_FALSE(window.isFullscreen());
    EXPECT_FALSE(window.isCreated());
}

TEST(OutputWindowTest, SetNameUpdatesName) {
    OutputWindow window;
    window.setName("Projector 1");

    EXPECT_EQ(window.getName(), "Projector 1");
}

TEST(OutputWindowTest, DefaultResolutionIs1080p) {
    OutputWindow window;

    EXPECT_EQ(window.getResolution().x, 1920);
    EXPECT_EQ(window.getResolution().y, 1080);
}

TEST(OutputWindowTest, SetResolutionUpdatesResolution) {
    OutputWindow window;
    window.setResolution(IVec2(3840, 2160));

    EXPECT_EQ(window.getResolution().x, 3840);
    EXPECT_EQ(window.getResolution().y, 2160);
}

TEST(OutputWindowTest, DefaultRotationIsR0) {
    OutputWindow window;

    EXPECT_EQ(window.getRotation(), Rotation::R0);
}

TEST(OutputWindowTest, SetRotationUpdatesRotation) {
    OutputWindow window;
    window.setRotation(Rotation::R90);

    EXPECT_EQ(window.getRotation(), Rotation::R90);
}

TEST(OutputWindowTest, DefaultMonitorIsNegativeOne) {
    OutputWindow window;

    EXPECT_EQ(window.getMonitor(), -1);
}

TEST(OutputWindowTest, SetMonitorUpdatesMonitor) {
    OutputWindow window;
    window.setMonitor(2);

    EXPECT_EQ(window.getMonitor(), 2);
}

TEST(OutputWindowTest, SetEnabledUpdatesState) {
    OutputWindow window;

    window.setEnabled(false);
    EXPECT_FALSE(window.isEnabled());

    window.setEnabled(true);
    EXPECT_TRUE(window.isEnabled());
}

TEST(OutputWindowTest, SetFullscreenUpdatesState) {
    OutputWindow window;

    window.setFullscreen(true);
    EXPECT_TRUE(window.isFullscreen());

    window.setFullscreen(false);
    EXPECT_FALSE(window.isFullscreen());
}

TEST(OutputWindowTest, NoAssignedScenesInitially) {
    OutputWindow window;

    EXPECT_TRUE(window.getAssignedScenes().empty());
}

TEST(OutputWindowTest, AssignSceneAddsToList) {
    OutputWindow window;
    UUID sceneId = UUID::generate();

    window.assignScene(sceneId);

    EXPECT_EQ(window.getAssignedScenes().size(), 1);
    EXPECT_TRUE(window.hasScene(sceneId));
}

TEST(OutputWindowTest, AssignSceneDoesNotDuplicate) {
    OutputWindow window;
    UUID sceneId = UUID::generate();

    window.assignScene(sceneId);
    window.assignScene(sceneId);  // Assign same scene again

    EXPECT_EQ(window.getAssignedScenes().size(), 1);
}

TEST(OutputWindowTest, AssignMultipleScenes) {
    OutputWindow window;
    UUID scene1 = UUID::generate();
    UUID scene2 = UUID::generate();
    UUID scene3 = UUID::generate();

    window.assignScene(scene1);
    window.assignScene(scene2);
    window.assignScene(scene3);

    EXPECT_EQ(window.getAssignedScenes().size(), 3);
    EXPECT_TRUE(window.hasScene(scene1));
    EXPECT_TRUE(window.hasScene(scene2));
    EXPECT_TRUE(window.hasScene(scene3));
}

TEST(OutputWindowTest, RemoveSceneRemovesFromList) {
    OutputWindow window;
    UUID sceneId = UUID::generate();

    window.assignScene(sceneId);
    EXPECT_TRUE(window.hasScene(sceneId));

    window.removeScene(sceneId);
    EXPECT_FALSE(window.hasScene(sceneId));
    EXPECT_TRUE(window.getAssignedScenes().empty());
}

TEST(OutputWindowTest, RemoveNonexistentSceneIsNoOp) {
    OutputWindow window;
    UUID scene1 = UUID::generate();
    UUID scene2 = UUID::generate();

    window.assignScene(scene1);
    window.removeScene(scene2);  // Try to remove scene that wasn't added

    EXPECT_EQ(window.getAssignedScenes().size(), 1);
    EXPECT_TRUE(window.hasScene(scene1));
}

TEST(OutputWindowTest, ClearScenesRemovesAll) {
    OutputWindow window;

    window.assignScene(UUID::generate());
    window.assignScene(UUID::generate());
    window.assignScene(UUID::generate());

    EXPECT_EQ(window.getAssignedScenes().size(), 3);

    window.clearScenes();

    EXPECT_TRUE(window.getAssignedScenes().empty());
}

TEST(OutputWindowTest, HasSceneReturnsFalseForUnknown) {
    OutputWindow window;
    UUID unknownId = UUID::generate();

    EXPECT_FALSE(window.hasScene(unknownId));
}

TEST(OutputWindowTest, PositionDefaultIs100_100) {
    OutputWindow window;

    EXPECT_EQ(window.getPosition().x, 100);
    EXPECT_EQ(window.getPosition().y, 100);
}

TEST(OutputWindowTest, SetPositionUpdatesPosition) {
    OutputWindow window;
    window.setPosition(IVec2(500, 300));

    EXPECT_EQ(window.getPosition().x, 500);
    EXPECT_EQ(window.getPosition().y, 300);
}

// TD-13: the GLFW window's user pointer is the OutputWindow's address, so moving one
// would leave it stale. Outputs are held by unique_ptr instead.
static_assert(!std::is_move_constructible_v<OutputWindow>, "OutputWindow must not move");
static_assert(!std::is_move_assignable_v<OutputWindow>, "OutputWindow must not move");
static_assert(!std::is_copy_constructible_v<OutputWindow>, "OutputWindow must not copy");

// TD-13: a key callback that clears itself while it runs (Esc leaving perform mode)
// must not destroy the function object it is executing. Drives the real GLFW key
// callback on a hidden window; skips where no window can be made (headless CI).
TEST(OutputWindowTest, KeyCallbackMayClearItselfWhileRunning) {
    if (!glfwInit()) GTEST_SKIP() << "GLFW unavailable";
    {
        OutputWindow window;
        if (!window.create(nullptr)) {
            glfwTerminate();
            GTEST_SKIP() << "no window could be created";
        }
        GLFWwindow* glfwWindow = window.getGLFWwindow();

        auto token = std::make_shared<int>(42);
        // Big enough that std::function keeps the lambda on the heap, where the debug
        // CRT overwrites freed memory: reading a destroyed capture would not see 42.
        std::array<int, 64> payload;
        payload.fill(42);
        int seenAfterClear = 0;
        int calls = 0;
        window.setKeyCallback([token, payload, &window, &seenAfterClear, &calls](int, int, int, int) {
            ++calls;
            window.setKeyCallback(nullptr);
            seenAfterClear = payload[63];  // the capture must still be alive here
        });
        EXPECT_EQ(glfwGetWindowUserPointer(glfwWindow), &window);
        EXPECT_EQ(token.use_count(), 2);

        // The installed GLFW callback, exactly as glfwPollEvents would call it.
        const GLFWkeyfun installed = glfwSetKeyCallback(glfwWindow, nullptr);
        glfwSetKeyCallback(glfwWindow, installed);
        ASSERT_NE(installed, nullptr);
        installed(glfwWindow, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);

        EXPECT_EQ(calls, 1);
        EXPECT_EQ(seenAfterClear, 42);
        EXPECT_FALSE(window.getKeyCallback());
        EXPECT_EQ(token.use_count(), 1);  // the running copy was released afterwards

        installed(glfwWindow, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);  // cleared: no call
        EXPECT_EQ(calls, 1);
    }
    glfwTerminate();
}

TEST(OutputWindowTest, IsVisibleFalseWhenNotCreated) {
    OutputWindow window;

    // Window not created, should return false
    EXPECT_FALSE(window.isVisible());
}

TEST(OutputWindowTest, GetGLFWwindowNullWhenNotCreated) {
    OutputWindow window;

    EXPECT_EQ(window.getGLFWwindow(), nullptr);
}

// Test that destroy is safe to call on uncreated window
TEST(OutputWindowTest, DestroyIsSafeWhenNotCreated) {
    OutputWindow window;

    // Should not crash
    window.destroy();
    EXPECT_FALSE(window.isCreated());
}

// Test that show/hide are safe to call on uncreated window
TEST(OutputWindowTest, ShowHideSafeWhenNotCreated) {
    OutputWindow window;

    // Should not crash
    window.show();
    window.hide();
    EXPECT_FALSE(window.isVisible());
}

// Test that makeContextCurrent/swapBuffers are safe when not created
TEST(OutputWindowTest, ContextOperationsSafeWhenNotCreated) {
    OutputWindow window;

    // Should not crash
    window.makeContextCurrent();
    window.swapBuffers();
}

// ============================================================================
// Phase 4 QM-021: Resolution/Rotation Controls Tests
// ============================================================================

TEST(OutputWindowTest, GetEffectiveResolutionNoRotation) {
    OutputWindow window;
    window.setResolution(IVec2(1920, 1080));
    window.setRotation(Rotation::R0);

    IVec2 effective = window.getEffectiveResolution();

    EXPECT_EQ(effective.x, 1920);
    EXPECT_EQ(effective.y, 1080);
}

TEST(OutputWindowTest, GetEffectiveResolutionRotation90) {
    OutputWindow window;
    window.setResolution(IVec2(1920, 1080));
    window.setRotation(Rotation::R90);

    IVec2 effective = window.getEffectiveResolution();

    // Width and height should be swapped
    EXPECT_EQ(effective.x, 1080);
    EXPECT_EQ(effective.y, 1920);
}

TEST(OutputWindowTest, GetEffectiveResolutionRotation180) {
    OutputWindow window;
    window.setResolution(IVec2(1920, 1080));
    window.setRotation(Rotation::R180);

    IVec2 effective = window.getEffectiveResolution();

    // No swap for 180 degrees
    EXPECT_EQ(effective.x, 1920);
    EXPECT_EQ(effective.y, 1080);
}

TEST(OutputWindowTest, GetEffectiveResolutionRotation270) {
    OutputWindow window;
    window.setResolution(IVec2(1920, 1080));
    window.setRotation(Rotation::R270);

    IVec2 effective = window.getEffectiveResolution();

    // Width and height should be swapped
    EXPECT_EQ(effective.x, 1080);
    EXPECT_EQ(effective.y, 1920);
}

TEST(OutputWindowTest, GetRotationDegreesReturnsCorrectValue) {
    OutputWindow window;

    window.setRotation(Rotation::R0);
    EXPECT_EQ(window.getRotationDegrees(), 0);

    window.setRotation(Rotation::R90);
    EXPECT_EQ(window.getRotationDegrees(), 90);

    window.setRotation(Rotation::R180);
    EXPECT_EQ(window.getRotationDegrees(), 180);

    window.setRotation(Rotation::R270);
    EXPECT_EQ(window.getRotationDegrees(), 270);
}

TEST(OutputWindowTest, SetRotationDegreesRoundsToNearestValid) {
    OutputWindow window;

    // 0-44 should round to 0
    window.setRotationDegrees(0);
    EXPECT_EQ(window.getRotation(), Rotation::R0);

    window.setRotationDegrees(44);
    EXPECT_EQ(window.getRotation(), Rotation::R0);

    // 45-134 should round to 90
    window.setRotationDegrees(45);
    EXPECT_EQ(window.getRotation(), Rotation::R90);

    window.setRotationDegrees(90);
    EXPECT_EQ(window.getRotation(), Rotation::R90);

    window.setRotationDegrees(134);
    EXPECT_EQ(window.getRotation(), Rotation::R90);

    // 135-224 should round to 180
    window.setRotationDegrees(135);
    EXPECT_EQ(window.getRotation(), Rotation::R180);

    window.setRotationDegrees(180);
    EXPECT_EQ(window.getRotation(), Rotation::R180);

    // 225-314 should round to 270
    window.setRotationDegrees(225);
    EXPECT_EQ(window.getRotation(), Rotation::R270);

    window.setRotationDegrees(270);
    EXPECT_EQ(window.getRotation(), Rotation::R270);

    // 315-359 should round back to 0
    window.setRotationDegrees(315);
    EXPECT_EQ(window.getRotation(), Rotation::R0);

    window.setRotationDegrees(359);
    EXPECT_EQ(window.getRotation(), Rotation::R0);
}

TEST(OutputWindowTest, SetRotationDegreesHandlesNegativeValues) {
    OutputWindow window;

    window.setRotationDegrees(-90);
    EXPECT_EQ(window.getRotation(), Rotation::R270);

    window.setRotationDegrees(-180);
    EXPECT_EQ(window.getRotation(), Rotation::R180);

    window.setRotationDegrees(-270);
    EXPECT_EQ(window.getRotation(), Rotation::R90);
}

TEST(OutputWindowTest, SetRotationDegreesHandlesLargeValues) {
    OutputWindow window;

    window.setRotationDegrees(360);
    EXPECT_EQ(window.getRotation(), Rotation::R0);

    window.setRotationDegrees(450);  // 360 + 90
    EXPECT_EQ(window.getRotation(), Rotation::R90);

    window.setRotationDegrees(720);  // 2 * 360
    EXPECT_EQ(window.getRotation(), Rotation::R0);
}

TEST(OutputWindowTest, ApplyResolutionFailsWhenNotCreated) {
    OutputWindow window;
    window.setResolution(IVec2(1280, 720));

    auto result = window.applyResolution();

    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputWindowTest, BorderlessDefaultFalse) {
    OutputWindow window;

    EXPECT_FALSE(window.isBorderless());
}

TEST(OutputWindowTest, GoBorderlessFailsWhenNotCreated) {
    OutputWindow window;

    auto result = window.goBorderlessFullscreen();

    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputWindowTest, GoFullscreenFailsWhenNotCreated) {
    OutputWindow window;

    auto result = window.goFullscreen();

    EXPECT_FALSE(result.isSuccess());
}

TEST(OutputWindowTest, GoWindowedFailsWhenNotCreated) {
    OutputWindow window;

    auto result = window.goWindowed();

    EXPECT_FALSE(result.isSuccess());
}

// Resolution presets tests
TEST(ResolutionPresetTest, PresetsExist) {
    EXPECT_GT(RESOLUTION_PRESET_COUNT, 0);
}

TEST(ResolutionPresetTest, ContainsCommonResolutions) {
    bool has1080p = false;
    bool has4k = false;

    for (size_t i = 0; i < RESOLUTION_PRESET_COUNT; ++i) {
        if (RESOLUTION_PRESETS[i].width == 1920 && RESOLUTION_PRESETS[i].height == 1080) {
            has1080p = true;
        }
        if (RESOLUTION_PRESETS[i].width == 3840 && RESOLUTION_PRESETS[i].height == 2160) {
            has4k = true;
        }
    }

    EXPECT_TRUE(has1080p);
    EXPECT_TRUE(has4k);
}

} // namespace test
} // namespace quadmapper
